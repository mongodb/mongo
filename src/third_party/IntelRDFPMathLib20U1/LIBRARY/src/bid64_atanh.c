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

BID_F80_CONST_DEF( c_half,    3ffe000000000000, 0000000000000000); // 0.5

BID_TYPE0_FUNCTION_ARGTYPE1(BID_UINT64, bid64_atanh, BID_UINT64, x)

  BID_UINT64 sign_x, coefficient_x, xn, tmp, y;
  BID_UINT64 valid_x, res, one, one_m_x;
  BID_F80_TYPE xq, rq;
  int exponent_x;

  valid_x = unpack_BID64 (&sign_x, &exponent_x, &coefficient_x, x);

  if (!valid_x) {
    // test if x is NaN
if ((x & 0x7c00000000000000ull) == 0x7c00000000000000ull) {
#ifdef BID_SET_STATUS_FLAGS
  if ((x & 0x7e00000000000000ull) == 0x7e00000000000000ull)	// sNaN
    __set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
#endif
	res = (coefficient_x) & QUIET_MASK64;
	BID_RETURN (res);
}
    // x is Infinity?
if ((x & 0x7800000000000000ull) == 0x7800000000000000ull) {
#ifdef BID_SET_STATUS_FLAGS
    __set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
#endif
	  res = 0x7c00000000000000ull;
      BID_RETURN (res);
  }
    // x is 0
	 res = sign_x | coefficient_x;
     BID_RETURN (res);
}

	
    if(exponent_x <= DECIMAL_EXPONENT_BIAS - 24)
	{
		res = x;
		BID_RETURN (res);
	}

	// |x| 
	xn = x & 0x7fffffffffffffffull;

	// 1.0
	one = 0x31c0000000000001ull;    

	// 1 - |x|
	BIDECIMAL_CALL2 (bid64_sub, one_m_x, one, xn);

	if(one_m_x & 0x8000000000000000ull)
	{
		// |x|>1
#ifdef BID_SET_STATUS_FLAGS
    __set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
#endif
	  res = 0x7c00000000000000ull;
      BID_RETURN (res);
  }

  if(!(one_m_x<<(64-53)) && ((one_m_x & SPECIAL_ENCODING_MASK64)!=SPECIAL_ENCODING_MASK64))
  {
	  // |x|==1
#ifdef BID_SET_STATUS_FLAGS
    __set_status_flags (pfpsf, BID_ZERO_DIVIDE_EXCEPTION);
#endif
	  res = sign_x | 0x7800000000000000ull;
      BID_RETURN (res);
  }

	// (2*|x|)/(1-|x|)
	BIDECIMAL_CALL2 (bid64_div, tmp, xn, one_m_x);
	BIDECIMAL_CALL2 (bid64_add, y, tmp, tmp);

	BIDECIMAL_CALL1 (bid64_to_binary80, xq, y);
	__bid_f80_log1p(rq, xq);
	__bid_f80_mul( rq,rq, c_half.v );
	BIDECIMAL_CALL1 (binary80_to_bid64, res, rq);

	res ^= sign_x;

	BID_RETURN (res);
}


