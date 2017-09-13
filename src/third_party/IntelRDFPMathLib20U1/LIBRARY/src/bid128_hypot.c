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

BID128_FUNCTION_ARG2 (bid128_hypot, x, y)

BID_UINT128 CX, CY, xn, yn, res, tmp, coeff_res;
BID_UINT64  valid_x, valid_y, sign_x, sign_y, sign_z;
int exponent_x, exponent_y, cmp_res, exponent_res;
BID_F128_TYPE rq, xq, yq;

	// take absolute values
	xn.w[BID_HIGH_128W] = x.w[BID_HIGH_128W] & 0x7fffffffffffffffull;
	yn.w[BID_HIGH_128W] = y.w[BID_HIGH_128W] & 0x7fffffffffffffffull;
	xn.w[BID_LOW_128W] = x.w[BID_LOW_128W];
	yn.w[BID_LOW_128W] = y.w[BID_LOW_128W];

    BIDECIMAL_CALL2_NORND (bid128_quiet_greater, 
            cmp_res, yn, xn);
	if(cmp_res) {
		tmp.w[BID_HIGH_128W]=x.w[BID_HIGH_128W];  tmp.w[BID_LOW_128W]=x.w[BID_LOW_128W];

		x.w[BID_HIGH_128W] = y.w[BID_HIGH_128W];
		x.w[BID_LOW_128W] = y.w[BID_LOW_128W];

		y.w[BID_HIGH_128W] = tmp.w[BID_HIGH_128W];
		y.w[BID_LOW_128W] = tmp.w[BID_LOW_128W];
	}

valid_y = unpack_BID128_value_BLE (&sign_y, &exponent_y, &CY, y);
valid_x = unpack_BID128_value_BLE (&sign_x, &exponent_x, &CX, x);

  // unpack arguments, check for NaN or Infinity
	if (!valid_x) {
    // test if x is NaN
if ((x.w[BID_HIGH_128W] & 0x7c00000000000000ull) == 0x7c00000000000000ull) {
#ifdef BID_SET_STATUS_FLAGS
  if ((x.w[BID_HIGH_128W] & 0x7e00000000000000ull) == 0x7e00000000000000ull ||	// sNaN
      (y.w[BID_HIGH_128W] & 0x7e00000000000000ull) == 0x7e00000000000000ull)
    __set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
#endif
  if (((x.w[BID_HIGH_128W] & 0x7e00000000000000ull) == 0x7e00000000000000ull) ||((y.w[BID_HIGH_128W] & 0x7c00000000000000ull) != 0x7800000000000000ull)) {
	res.w[BID_HIGH_128W] = (CX.w[BID_HIGH_128W]) & QUIET_MASK64;
	res.w[BID_LOW_128W] = CX.w[BID_LOW_128W];
  }
  else {  res.w[BID_HIGH_128W] = 0x7800000000000000ull;   res.w[BID_LOW_128W] = 0;  }
	BID_RETURN (res);
  
}
    // x is Infinity?
if (((x.w[BID_HIGH_128W] & 0x7800000000000000ull) == 0x7800000000000000ull) && ((y.w[BID_HIGH_128W] & 0x7e00000000000000ull) != 0x7e00000000000000ull)) {
	  res.w[BID_HIGH_128W] = 0x7800000000000000ull;
	  res.w[BID_LOW_128W] = 0;
    BID_RETURN (res);
  }
    // x is 0
  if (valid_y) {
	 res.w[BID_HIGH_128W] = y.w[BID_HIGH_128W] & 0x7fffffffffffffffull;
	 res.w[BID_LOW_128W] = y.w[BID_LOW_128W];
     BID_RETURN (res);
}
}

	if (!valid_y) {
  // y is Inf. or NaN

  // test if y is NaN
  if ((y.w[BID_HIGH_128W] & 0x7c00000000000000ull) == 0x7c00000000000000ull) {
#ifdef BID_SET_STATUS_FLAGS
    if ((y.w[BID_HIGH_128W] & 0x7e00000000000000ull) == 0x7e00000000000000ull)	// sNaN
      __set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
#endif
    res.w[BID_HIGH_128W] = CY.w[BID_HIGH_128W] & QUIET_MASK64;
    res.w[BID_LOW_128W] = CY.w[BID_LOW_128W];
    BID_RETURN (res);
  }

  // y is Infinity?
if ((y.w[BID_HIGH_128W] & 0x7800000000000000ull) == 0x7800000000000000ull) {
	  res.w[BID_HIGH_128W] = 0x7800000000000000ull;
	  res.w[BID_LOW_128W] = 0;
    BID_RETURN (res);
  }

  // y is 0
if(valid_x) {
	 res.w[BID_HIGH_128W] = x.w[BID_HIGH_128W] & 0x7fffffffffffffffull;
	 res.w[BID_LOW_128W] = x.w[BID_LOW_128W];
}
else {
	 res.w[BID_HIGH_128W] = CX.w[BID_HIGH_128W] & 0x7fffffffffffffffull;
	 res.w[BID_LOW_128W] = CX.w[BID_LOW_128W];
}
     BID_RETURN (res);
}

	// take absolute values
	x.w[BID_HIGH_128W] &= 0x7fffffffffffffffull;
	y.w[BID_HIGH_128W] &= 0x7fffffffffffffffull;

	if(exponent_x - exponent_y >= 35)
	{
	 res.w[BID_HIGH_128W] = x.w[BID_HIGH_128W];
	 res.w[BID_LOW_128W] = x.w[BID_LOW_128W];
     BID_RETURN (res);
	}

	// separate exponent_x (to avoid OF/UF)
	bid_get_BID128_very_fast_BLE(&xn, 0, DECIMAL_EXPONENT_BIAS_128, CX);
	bid_get_BID128_very_fast_BLE(&yn, 0, DECIMAL_EXPONENT_BIAS_128+exponent_y-exponent_x, CY);

	BIDECIMAL_CALL1 (bid128_to_binary128, xq, xn);
	BIDECIMAL_CALL1 (bid128_to_binary128, yq, yn);
	__bid_f128_hypot(rq, xq, yq);
    BIDECIMAL_CALL1 (binary128_to_bid128, res, rq);

	//quick_unpack_BID128_em (&exponent_res, &coeff_res, res);
	coeff_res.w[0] = res.w[BID_LOW_128W];
	coeff_res.w[1] = (res.w[BID_HIGH_128W]) & SMALL_COEFF_MASK128;
	exponent_res = (res.w[BID_HIGH_128W]) >> 49;
	exponent_res = ((int) exponent_res) & EXPONENT_MASK128;

    bid_get_BID128 (&res, 0, exponent_res+exponent_x-DECIMAL_EXPONENT_BIAS_128, coeff_res, &rnd_mode,
		pfpsf);

#if BID_BIG_ENDIAN
	BID_SWAP128(res);
#endif
	BID_RETURN (res);
}

