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

BID128_FUNCTION_ARG1_NORND_CUSTOMRESTYPE(int, bid128_ilogb, x)

  BID_UINT128 CX;
  BID_UINT64 sign_x;
  BID_SINT64 D;
  int_float f64, fx;
  int exponent_x, bin_expon_cx, digits, res;

  BID_OPT_SAVE_BINARY_FLAGS()

   if (!unpack_BID128_value (&sign_x, &exponent_x, &CX, x)) {
#ifdef BID_SET_STATUS_FLAGS
      __set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
#endif
	 res =  ((x.w[1] & 0x7c00000000000000ull) == 0x7800000000000000ull)? 0x7fffffff : 0x80000000;
     BID_RETURN_VAL (res);
  }
  // find number of digits in coefficient
  // 2^64
  f64.i = 0x5f800000;
  // fx ~ CX
  fx.d = (float) CX.w[1] * f64.d + (float) CX.w[0];
  bin_expon_cx = ((fx.i >> 23) & 0xff) - 0x7f;
  digits = bid_estimate_decimal_digits[bin_expon_cx];
  // scale = 38-estimate_decimal_digits[bin_expon_cx];
  D = CX.w[1] - bid_power10_index_binexp_128[bin_expon_cx].w[1];
  if (D > 0 || (!D && CX.w[0] >= bid_power10_index_binexp_128[bin_expon_cx].w[0])) {
    digits++;
  }

  exponent_x = exponent_x - DECIMAL_EXPONENT_BIAS_128 - 1 + digits;

  BID_RETURN_VAL (exponent_x);

}
