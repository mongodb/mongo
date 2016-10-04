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

#include "bid_internal.h"

BID_TYPE0_FUNCTION_ARGTYPE1_NORND(int, bid32_ilogb, BID_UINT32, x)

  BID_UINT32 sign_x, coefficient_x;
  int_float dx;
  int exponent_x, bin_expon_cx, digits, res;


  // unpack arguments, check for NaN or Infinity
  if (!unpack_BID32 (&sign_x, &exponent_x, &coefficient_x, x)) {
    // x is Inf. or NaN
#ifdef BID_SET_STATUS_FLAGS
      __set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
#endif
	  res = ((x & 0x7c000000) == 0x78000000) ? 0x7fffffff : 0x80000000;

      BID_RETURN (res);
  }
  // find number of digits in coefficient
  if (coefficient_x >= 1000000ull) {
    digits = 7;
  } else {
    dx.d = (float)coefficient_x;   // exact conversion;
    bin_expon_cx = (int)(dx.i >> 23) - 127;
    digits = bid_estimate_decimal_digits[bin_expon_cx];
    if (coefficient_x >= bid_power10_table_128[digits].w[0])
      digits++;
  }
  exponent_x = exponent_x - DECIMAL_EXPONENT_BIAS_32 + digits - 1;

  BID_RETURN (exponent_x);
}
