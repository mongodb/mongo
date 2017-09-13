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

BID128_FUNCTION_ARG1 (bid128_exp10, x)

BID_UINT128 CX, res, threshold, xn, tmp, fd;
BID_UINT64  sign_x;
BID_SINT64 kl, k2l, scorr;
BID_F128_TYPE rq, xq;
int exponent_x, cmp_res, k, k2;

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
	  res.w[BID_HIGH_128W] = sign_x? 0: 0x7800000000000000ull;
	  res.w[BID_LOW_128W] = 0;
    BID_RETURN (res);
  }
    // x is 0, return 1.0
	 res.w[BID_HIGH_128W] = 0x3040000000000000ull;
	 res.w[BID_LOW_128W] = 0;
     BID_RETURN (res);
}

    threshold.w[BID_HIGH_128W] = 0x3040000000000000ull;
	threshold.w[BID_LOW_128W] = 0x17df;   // 6111 = emax - bias

	xn.w[BID_HIGH_128W] = x.w[BID_HIGH_128W] ^ sign_x;
	xn.w[BID_LOW_128W] = x.w[BID_LOW_128W];

	// compare |x| to threshold
    BIDECIMAL_CALL2_NORND (bid128_quiet_less, cmp_res, threshold, xn);

	if(cmp_res)
	{
		// compare to 6400 
		threshold.w[BID_HIGH_128W] = 0x3040000000000000ull;
		threshold.w[BID_LOW_128W] = 0x1900;   // 6400
		// compare |x| to threshold
		BIDECIMAL_CALL2_NORND (bid128_quiet_less, cmp_res, threshold, xn);

		if(cmp_res) {
			// |x|>6400, overflow or underflow case
			if(sign_x)
				tmp.w[BID_HIGH_128W] = 0x1100000000000000ull;
			else tmp.w[BID_HIGH_128W] = 0x4f80000000000000ull;
			tmp.w[BID_LOW_128W] = 1;

			// dummy op to set the result and flags
			BIDECIMAL_CALL2 (bid128_mul, res, tmp, tmp);
			BID_RETURN (res);
		}

		// get k=(int)(|x|)
		BIDECIMAL_CALL1_NORND (bid128_to_int32_rnint, k, xn);
		// tmp = -(int)(x)
		tmp.w[BID_HIGH_128W] = sign_x ^ 0xb040000000000000ull;
		tmp.w[BID_LOW_128W] = k;
	    // fd = x - (int)(x)
		BIDECIMAL_CALL2 (bid128_add, fd, x, tmp);
	    // 10^fd
	    BIDECIMAL_CALL1 (bid128_to_binary128, xq, fd);
		__bid_f128_exp10(rq, xq);
		BIDECIMAL_CALL1 (binary128_to_bid128, res, rq);

		if(sign_x) k = -k;

        k2 = k>>1;  k -= k2;
		k2l = (BID_SINT64)k2;  kl = (BID_SINT64)k;

		res.w[BID_HIGH_128W] += (k2l<<49);  // first scaling

		tmp.w[BID_HIGH_128W] = 0x3040000000000000ull + (kl<<49);
		tmp.w[BID_LOW_128W] = 1;
		// second scaling will set flags and result correctly
		BIDECIMAL_CALL2 (bid128_mul, res, res, tmp);

		BID_RETURN (res);
	}

   // get k=(int)(|x|)
	BIDECIMAL_CALL1_NORND (bid128_to_int32_rnint, k, xn);

	// tmp = -(int)(x)
	tmp.w[BID_HIGH_128W] = sign_x ^ 0xb040000000000000ull;
	tmp.w[BID_LOW_128W] = k;

   // fd = x - (int)(|x|)
   BIDECIMAL_CALL2 (bid128_add, fd, x, tmp);

   // 10^fd
   BIDECIMAL_CALL1 (bid128_to_binary128, xq, fd);
   __bid_f128_exp10(rq, xq);
   BIDECIMAL_CALL1 (binary128_to_bid128, res, rq);

   kl = (BID_SINT64)k;
   // set correct sign of kl
   scorr = (BID_SINT64)sign_x;  scorr >>= 63;
   kl = scorr ^ (kl + scorr);
   res.w[BID_HIGH_128W] += (kl<<49);

   BID_RETURN (res);
}

