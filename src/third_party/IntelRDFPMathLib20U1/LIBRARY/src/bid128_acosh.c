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


#include "bid_trans.h"

BID_F128_CONST_DEF( c_log10, 400026bb1bbb5551, 582dd4adac5705a6); // ln(10)

BID128_FUNCTION_ARG1 (bid128_acosh, x)

BID_UINT128 CX, CY, xn, yn, res, tmp, coeff_res, one, z, z2, near_one;
BID_UINT64  valid_y, sign_x, sign_y, sign_z;
int exponent_x, exponent_y, cmp_res, exponent_res;
BID_F128_TYPE rq, xq, yq, rt;

  // unpack arguments, check for NaN or Infinity
	if (!unpack_BID128_value_BLE (&sign_x, &exponent_x, &CX, x)) {
   // test if x is NaN
if ((x.w[BID_HIGH_128W] & 0x7c00000000000000ull) == 0x7c00000000000000ull) {
#ifdef BID_SET_STATUS_FLAGS
  if ((x.w[BID_HIGH_128W] & 0x7e00000000000000ull) == 0x7e00000000000000ull)	// sNaN
    __set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
#endif
	res.w[BID_HIGH_128W] = (CX.w[BID_HIGH_128W]) & QUIET_MASK64;
	res.w[BID_LOW_128W] = CX.w[BID_LOW_128W];
	BID_RETURN (res);
}
    // x is Infinity?
if ((x.w[BID_HIGH_128W] & 0x7800000000000000ull) == 0x7800000000000000ull) {
#ifdef BID_SET_STATUS_FLAGS
  if (sign_x)	// -Inf
    __set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
#endif
      res.w[BID_HIGH_128W] = sign_x? 0x7c00000000000000ull : 0x7800000000000000ull;
	  res.w[BID_LOW_128W] = 0;
    BID_RETURN (res);
  }
    // x is 0
}
           
	 // calculate asinh(sqrt(x*x-1)) for x near 1 (x<1+1/32 = (10^5 + 5^5)/10^5 )
	 near_one.w[BID_LOW_128W] = 103125;
	 near_one.w[BID_HIGH_128W] = 0x3036000000000000ull;

	 BIDECIMAL_CALL2_NORND (bid128_quiet_less, 
            cmp_res, x, near_one);
	 if(cmp_res) {
		 // x<1+1/32
		one.w[BID_HIGH_128W] = 0x3040000000000000ull;  one.w[BID_LOW_128W]=1;

		BIDECIMAL_CALL2_NORND (bid128_quiet_greater, 
            cmp_res, one, x);
		if(cmp_res) {
			// x < 1
#ifdef BID_SET_STATUS_FLAGS
			__set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
#endif
			res.w[BID_HIGH_128W] = 0x7c00000000000000ull;
			res.w[BID_LOW_128W] = 0;
			BID_RETURN (res);
		}

		// -1
		one.w[BID_HIGH_128W] = 0xb040000000000000ull;

		// x*x-1
		BIDECIMAL_CALL3(bid128_fma, z2, x, x, one);
		// sqrt(x*x-1)
		BIDECIMAL_CALL1 (bid128_sqrt, z, z2);

		BIDECIMAL_CALL1 (bid128_to_binary128, xq, z);
		__bid_f128_asinh(rq, xq);
		BIDECIMAL_CALL1 (binary128_to_bid128, res, rq);
		BID_RETURN (res);
	 }

	if(exponent_x > (DECIMAL_EXPONENT_BIAS_128+34))
	{
		bid_get_BID128_very_fast_BLE(&xn, 0, DECIMAL_EXPONENT_BIAS_128, CX);
		BIDECIMAL_CALL1 (bid128_to_binary128, xq, xn);
		__bid_f128_add(xq, xq, xq);
		__bid_f128_itof(yq, exponent_x-DECIMAL_EXPONENT_BIAS_128);
		__bid_f128_log(rq, xq);
		__bid_f128_mul(rt, yq, c_log10.v);
		__bid_f128_add(rq, rq, rt); 
		BIDECIMAL_CALL1 (binary128_to_bid128, res, rq);

	    BID_RETURN (res);
	}

	BIDECIMAL_CALL1 (bid128_to_binary128, xq, x);
	__bid_f128_acosh(rq, xq);
    BIDECIMAL_CALL1 (binary128_to_bid128, res, rq);

	BID_RETURN (res);
}


