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

BID_F128_CONST_DEF( c_half,    3ffe000000000000, 0000000000000000); // 0.5

BID128_FUNCTION_ARG1 (bid128_atanh, x)

BID_UINT128 CX, CY, xn, one, one_m_x, one_p_x, res, tmp, coeff_res, y;
BID_UINT64  sign_x;
int exponent_x, exponent_y, cmp_res, exponent_res;
BID_F128_TYPE rq, xq, yq;

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
    __set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
#endif
	  res.w[BID_HIGH_128W] = 0x7c00000000000000ull;
	  res.w[BID_LOW_128W] = 0;
    BID_RETURN (res);
  }
    // x is 0
	 res.w[BID_HIGH_128W] = sign_x | CX.w[BID_HIGH_128W];
	 res.w[BID_LOW_128W] = CX.w[BID_LOW_128W];
     BID_RETURN (res);
}

	
	if(exponent_x <= DECIMAL_EXPONENT_BIAS_128 - 51)
	{
		res.w[BID_HIGH_128W] = x.w[BID_HIGH_128W];
		res.w[BID_LOW_128W] = x.w[BID_LOW_128W];
		BID_RETURN (res);
	}

	// |x| 
	xn.w[BID_HIGH_128W] = x.w[BID_HIGH_128W] & 0x7fffffffffffffffull;
	xn.w[BID_LOW_128W] = x.w[BID_LOW_128W];

	// 1.0
	one.w[BID_HIGH_128W] = 0x3040000000000000ull;    one.w[BID_LOW_128W] = 1;

	// 1 - |x|
	BIDECIMAL_CALL2 (bid128_sub, one_m_x, one, xn);

	if(one_m_x.w[BID_HIGH_128W] & 0x8000000000000000ull)
	{
		// |x|>1
#ifdef BID_SET_STATUS_FLAGS
    __set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
#endif
	  res.w[BID_HIGH_128W] = 0x7c00000000000000ull;
	  res.w[BID_LOW_128W] = 0;
      BID_RETURN (res);
  }

  if((!one_m_x.w[BID_LOW_128W]) && !(one_m_x.w[BID_HIGH_128W]<<15))
  {
	  // |x|==1
#ifdef BID_SET_STATUS_FLAGS
    __set_status_flags (pfpsf, BID_ZERO_DIVIDE_EXCEPTION);
#endif
	  res.w[BID_HIGH_128W] = sign_x | 0x7800000000000000ull;
	  res.w[BID_LOW_128W] = 0;
      BID_RETURN (res);
  }

	// (2*|x|)/(1-|x|)
	BIDECIMAL_CALL2 (bid128_div, tmp, xn, one_m_x);
	BIDECIMAL_CALL2 (bid128_add, y, tmp, tmp);

	BIDECIMAL_CALL1 (bid128_to_binary128, xq, y);
	__bid_f128_log1p(rq, xq);        
	__bid_f128_mul(rq, rq, c_half.v);
    BIDECIMAL_CALL1 (binary128_to_bid128, res, rq);

	res.w[BID_HIGH_128W] ^= sign_x;

	BID_RETURN (res);
}


