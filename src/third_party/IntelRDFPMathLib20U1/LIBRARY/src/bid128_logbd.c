/******************************************************************************
  Copyright (c) 2011, Intel Corp.
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

BID128_FUNCTION_ARG1_NORND (bid128_logb, x)

  int ires, exponent_x;
  BID_UINT64  sign_x;
  BID_UINT128 res, CX;

  BID_SWAP128 (x);
  if (!unpack_BID128_value (&sign_x, &exponent_x, &CX, x)) {
    // test if x is NaN/Inf
  BID_SWAP128 (x);
if ((x.w[BID_HIGH_128W] & 0x7800000000000000ull) == 0x7800000000000000ull) {
#ifdef BID_SET_STATUS_FLAGS
  if ((x.w[BID_HIGH_128W] & 0x7e00000000000000ull) == 0x7e00000000000000ull)	// sNaN
    __set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
#endif
	res.w[BID_HIGH_128W] = (CX.w[1]) & QUIET_MASK64;
	res.w[BID_LOW_128W] = CX.w[0];
	if ((x.w[BID_HIGH_128W] & 0x7c00000000000000ull) == 0x7800000000000000ull)
		res.w[BID_HIGH_128W] &= 0x7fffffffffffffffull;
	BID_RETURN (res);
  }

  // x is 0
#ifdef BID_SET_STATUS_FLAGS
    __set_status_flags (pfpsf, BID_ZERO_DIVIDE_EXCEPTION);
#endif
	res.w[BID_HIGH_128W] = 0xf800000000000000ull;
	res.w[BID_LOW_128W] = 0;
	BID_RETURN (res);

}

  BID_SWAP128 (x);
  BIDECIMAL_CALL1_NORND (bid128_ilogb, ires, x);
  if (ires & 0x80000000) {
    res.w[BID_HIGH_128W] = 0xb040000000000000ull;
    res.w[BID_LOW_128W] = (BID_UINT64)(-ires);
  } else {
    res.w[BID_HIGH_128W] = 0x3040000000000000ull;
    res.w[BID_LOW_128W] = (BID_UINT64)ires;
  }
  BID_RETURN (res);
}
