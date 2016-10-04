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
BID_EXTERN_C double hypot(double, double);


BID_TYPE_FUNCTION_ARG2(BID_UINT32, bid32_hypot, x, y)
  BID_UINT32 sign_x, sign_y, coefficient_x, coefficient_y;
  BID_UINT32 valid_x, valid_y, res;
  double xd, yd, zd;
  int exponent_x, exponent_y;


  valid_x = unpack_BID32 (&sign_x, &exponent_x, &coefficient_x, x);
  valid_y = unpack_BID32 (&sign_y, &exponent_y, &coefficient_y, y);

  if (!valid_x) {
if ((x & 0x7c000000) == 0x7c000000) {
#ifdef BID_SET_STATUS_FLAGS
  if ((x & 0x7e000000) == 0x7e000000 ||	// sNaN
      (y & 0x7e000000) == 0x7e000000)
    __set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
#endif
  if (((x & 0x7e000000) == 0x7e000000) || ((y & 0x7c000000) != 0x78000000)) 
	res = (coefficient_x) & QUIET_MASK32;
  else res = 0x78000000;  
	BID_RETURN (res);
  
}
    // x is Infinity?
if (((x & 0x78000000) == 0x78000000) && ((y & 0x7e000000) != 0x7e000000)) {
	  res = 0x78000000;
    BID_RETURN (res);
  }
    // x is 0
  if (valid_y) {
	 res = y & 0x7fffffff;
     BID_RETURN (res);
}
}

	if (!valid_y) {
  // y is Inf. or NaN

  // test if y is NaN
  if ((y & 0x7c000000) == 0x7c000000) {
#ifdef BID_SET_STATUS_FLAGS
    if ((y & 0x7e000000) == 0x7e000000)	// sNaN
      __set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
#endif
    res = coefficient_y & QUIET_MASK32;
    BID_RETURN (res);
  }


  if ((y & 0x78000000) == 0x78000000) {
	  res = 0x78000000;
      BID_RETURN (res);
  }
    // y is 0
  if (valid_x) {
	 res = x & 0x7fffffff;
     BID_RETURN (res);
  }
	}

  BIDECIMAL_CALL1(bid32_to_binary64,xd,x);
  BIDECIMAL_CALL1(bid32_to_binary64,yd,y);
  zd = hypot(xd,yd);
  BIDECIMAL_CALL1(binary64_to_bid32,res,zd);
  BID_RETURN (res);

}

