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

static BID_UINT128 BID128_DEC_PI =
  { BID128_LH_INIT( 0xbabe5564e6f39f8full, 0x2ffe9ae4795796a7ull ) };
static BID_UINT128 BID128_DEC_PI12 =
  { BID128_LH_INIT( 0xdd5f2ab27379cfc7ull, 0x2ffe4d723cabcb53ull ) };
static BID_UINT128 BID128_DEC_PI14 =
  { BID128_LH_INIT( 0xeeaf955939bce7e4ull, 0x2ffe26b91e55e5a9ull ) };
static BID_UINT128 BID128_DEC_PI34 =
  { BID128_LH_INIT( 0xCC0EC00BAD36B7ABull, 0x2ffe742B5B01B0FDull ) };
static BID_UINT128 BID128_10POW36 =
  { BID128_LH_INIT( 0x0000000000000001ull, 0x3088000000000000ull ) };
static BID_UINT128 BID128_10POW_M36 =
  { BID128_LH_INIT( 0x0000000000000001ull, 0x2ff8000000000000ull ) };


BID128_FUNCTION_ARG2 (bid128_atan2, x, y)

BID_UINT128 CX, CY, z, zabs, res;
BID_UINT64  valid_y, sign_x, sign_y, sign_z;
int exponent_x, exponent_y, cmp_res;
_IDEC_flags save_flags;
BID_F128_TYPE rq, zq;

valid_y = unpack_BID128_value_BLE (&sign_y, &exponent_y, &CY, y);

  // unpack arguments, check for NaN or Infinity
	if (!unpack_BID128_value_BLE (&sign_x, &exponent_x, &CX, x)) {
    // test if x is NaN
if ((x.w[BID_HIGH_128W] & 0x7c00000000000000ull) == 0x7c00000000000000ull) {
#ifdef BID_SET_STATUS_FLAGS
  if ((x.w[BID_HIGH_128W] & 0x7e00000000000000ull) == 0x7e00000000000000ull ||	// sNaN
      (y.w[BID_HIGH_128W] & 0x7e00000000000000ull) == 0x7e00000000000000ull)
    __set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
#endif
  res.w[BID_HIGH_128W] = (CX.w[BID_HIGH_128W]) & QUIET_MASK64;
  res.w[BID_LOW_128W] = CX.w[BID_LOW_128W];
  BID_RETURN (res);
}
    // x is Infinity?
if ((x.w[BID_HIGH_128W] & 0x7800000000000000ull) == 0x7800000000000000ull) {
  // check if y is Inf. 
  if (((y.w[BID_HIGH_128W] & 0x7c00000000000000ull) == 0x7800000000000000ull))
    // return NaN 
  {
	  if(sign_y) {
		  res.w[BID_HIGH_128W] = sign_x ^ BID128_DEC_PI34.w[BID_HIGH_128W];
		  res.w[BID_LOW_128W] = BID128_DEC_PI34.w[BID_LOW_128W];
	  }
	  else {
		  res.w[BID_HIGH_128W] = sign_x ^ BID128_DEC_PI14.w[BID_HIGH_128W];
		  res.w[BID_LOW_128W] = BID128_DEC_PI14.w[BID_LOW_128W];
	  }
    BID_RETURN (res);
  }
  // y is NaN?
  if (((y.w[BID_HIGH_128W] & 0x7c00000000000000ull) != 0x7c00000000000000ull))
    // not NaN 
  {
    // return +/-pi/2
		  res.w[BID_HIGH_128W] = sign_x ^ BID128_DEC_PI12.w[BID_HIGH_128W];
		  res.w[BID_LOW_128W] = BID128_DEC_PI12.w[BID_LOW_128W];
		BID_RETURN (res);
  }
}
    // x is 0
if(valid_y) {
	if(sign_y)
	{
		res.w[BID_HIGH_128W] = sign_x^BID128_DEC_PI.w[BID_HIGH_128W];
		res.w[BID_LOW_128W] = BID128_DEC_PI.w[BID_LOW_128W];
	}
	else {
		res.w[BID_HIGH_128W] = sign_x;
		res.w[BID_LOW_128W] = 0;
	}
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
	  if(sign_y) {
		res.w[BID_HIGH_128W] = sign_x ^ BID128_DEC_PI.w[BID_HIGH_128W];
		res.w[BID_LOW_128W] = BID128_DEC_PI.w[BID_LOW_128W];
	  }
	  else {
		res.w[BID_HIGH_128W] = sign_x;
		res.w[BID_LOW_128W] = 0;
	  }
    BID_RETURN (res);
  }
  // y is 0
  if(!(CX.w[BID_HIGH_128W]|CX.w[BID_LOW_128W]))
  {
	if(sign_y)
	{
		res.w[BID_HIGH_128W] = sign_x^BID128_DEC_PI.w[BID_HIGH_128W];
		res.w[BID_LOW_128W] = BID128_DEC_PI.w[BID_LOW_128W];
	}
	else {
		res.w[BID_HIGH_128W] = sign_x;
		res.w[BID_LOW_128W] = 0;
	}
  }
  else {
	  // x finite, return +/-pi/2
		res.w[BID_HIGH_128W] = sign_x^BID128_DEC_PI12.w[BID_HIGH_128W];
		res.w[BID_LOW_128W] = BID128_DEC_PI12.w[BID_LOW_128W];
  }
  BID_RETURN (res);
}

	save_flags = *pfpsf;

    BIDECIMAL_CALL2 (bid128_div, z, x, y);
	zabs.w[BID_HIGH_128W] = z.w[BID_HIGH_128W] & 0x7fffffffffffffffull;
	zabs.w[BID_LOW_128W] = z.w[BID_LOW_128W];

	*pfpsf = save_flags;    // avoided incorrect OF/UF

    BIDECIMAL_CALL2_NORND (bid128_quiet_greater, 
            cmp_res, zabs, BID128_10POW36);
	if(cmp_res) {
		// |x/y|>10^36
		res.w[BID_HIGH_128W] = sign_x ^ BID128_DEC_PI12.w[BID_HIGH_128W];
		res.w[BID_LOW_128W] = BID128_DEC_PI12.w[BID_LOW_128W];
		BID_RETURN (res);
	}
    BIDECIMAL_CALL2_NORND (bid128_quiet_less, 
            cmp_res, zabs, BID128_10POW_M36);
	if(cmp_res) {
		// |x/y|<10^(-36)
		if(sign_y) {
			res.w[BID_HIGH_128W] = sign_x ^ BID128_DEC_PI.w[BID_HIGH_128W];
			res.w[BID_LOW_128W] = BID128_DEC_PI.w[BID_LOW_128W];
		}
		else {
			// Here could set UF correctly based on flags set in bid128_div
			res.w[BID_HIGH_128W] = z.w[BID_HIGH_128W];
			res.w[BID_LOW_128W] = z.w[BID_LOW_128W];
		}
		BID_RETURN (res);
	}

	BIDECIMAL_CALL1 (bid128_to_binary128, zq, zabs);
	__bid_f128_atan(rq, zq);
    BIDECIMAL_CALL1 (binary128_to_bid128, res, rq);

	if(sign_y) {
		BIDECIMAL_CALL2 (bid128_sub, res, BID128_DEC_PI, res);
	}

    res.w[BID_HIGH_128W] |= sign_x;

	BID_RETURN (res);
}
