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

BID_EXTERN_C double asinh(double);
BID_EXTERN_C double acosh(double);


BID_TYPE0_FUNCTION_ARGTYPE1(BID_UINT32, bid32_acosh, BID_UINT32, x)

  BID_UINT32 sign_x, coefficient_x, near_one, one;
  BID_UINT32 valid_x, res, z, z2;
  double xd, zd;
  int exponent_x, cmp_res;


  valid_x = unpack_BID32 (&sign_x, &exponent_x, &coefficient_x, x);

  if (!valid_x) {
    // test if x is NaN
if ((x & 0x7c000000) == 0x7c000000) {
#ifdef BID_SET_STATUS_FLAGS
  if ((x & 0x7e000000) == 0x7e000000)	// sNaN
    __set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
#endif
	res = (coefficient_x) & QUIET_MASK32;
	BID_RETURN (res);
}
    // x is Infinity?
if ((x & 0x78000000) == 0x78000000) {
#ifdef BID_SET_STATUS_FLAGS
  if (sign_x)	// -Inf
    __set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
#endif
      res = sign_x? 0x7c000000 : 0x78000000;
    BID_RETURN (res);
  }
    // x is 0
}
           
	 // calculate asinh(sqrt(x*x-1)) for x near 1 (x<1+1/32 = (10^5 + 5^5)/10^5 )
	 near_one = 0x300192d5;

	 BIDECIMAL_CALL2_NORND (bid32_quiet_less, 
            cmp_res, x, near_one);
	 if(cmp_res) {
		 // x<1+1/32
		one = 0x32800001; 

		BIDECIMAL_CALL2_NORND (bid32_quiet_greater, 
            cmp_res, one, x);
		if(cmp_res) {
			// x < 1
#ifdef BID_SET_STATUS_FLAGS
			__set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
#endif
			res = 0x7c000000;
			BID_RETURN (res);
		}

		// -1
		one = 0xb2800001;

		// x*x-1
		BIDECIMAL_CALL3(bid32_fma, z2, x, x, one);
		// sqrt(x*x-1)
		BIDECIMAL_CALL1 (bid32_sqrt, z, z2);

		BIDECIMAL_CALL1 (bid32_to_binary64, xd, z);
		zd = asinh(xd);
		BIDECIMAL_CALL1 (binary64_to_bid32, res, zd);
		BID_RETURN (res);
	 }

  BIDECIMAL_CALL1(bid32_to_binary64,xd,x);
  zd = acosh(xd);
  BIDECIMAL_CALL1(binary64_to_bid32,res,zd);
  BID_RETURN (res);


}

