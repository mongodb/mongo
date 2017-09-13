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

BID_TYPE0_FUNCTION_ARGTYPE1_OTHER_ARGTYPE2(BID_UINT32, bid32_scalbn, BID_UINT32, x, int, n)

  BID_UINT32 sign_x, coefficient_x, res;
  BID_SINT64 exp64;
  int exponent_x, rmode;

  // unpack arguments, check for NaN or Infinity
  if (!unpack_BID32 (&sign_x, &exponent_x, &coefficient_x, x)) {
    // x is Inf. or NaN or 0
#ifdef BID_SET_STATUS_FLAGS
    if ((x & SNAN_MASK32) == SNAN_MASK32)	// y is sNaN
      __set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
#endif
    if (coefficient_x)
      res = coefficient_x & QUIET_MASK32;
	else {
       exp64 = (BID_SINT64) exponent_x + (BID_SINT64) n;
	   if(exp64<0) exp64=0;
	   if(exp64>DECIMAL_MAX_EXPON_32) exp64=DECIMAL_MAX_EXPON_32;
       exponent_x = exp64;
     res = very_fast_get_BID32 (sign_x, exponent_x, coefficient_x);	// 0
	}
    BID_RETURN (res);
  }

  exp64 = (BID_SINT64) exponent_x + (BID_SINT64) n;
  exponent_x = exp64;

  if ((BID_UINT32) exponent_x <= DECIMAL_MAX_EXPON_32) {
    res = very_fast_get_BID32 (sign_x, exponent_x, coefficient_x);
    BID_RETURN (res);
  }
  // check for overflow
  if (exp64 > DECIMAL_MAX_EXPON_32) {
    // try to normalize coefficient
    while ((coefficient_x < 1000000ul)
	   && (exp64 > DECIMAL_MAX_EXPON_32)) {
      // coefficient_x < 10^15, scale by 10
      coefficient_x = (coefficient_x << 1) + (coefficient_x << 3);
      exponent_x--;
      exp64--;
    }
    if (exp64 <= DECIMAL_MAX_EXPON_32) {
      res = very_fast_get_BID32 (sign_x, exponent_x, coefficient_x);
      BID_RETURN (res);
    } else
      exponent_x = 0x7fffffff;	// overflow
  }
  // exponent < 0
  // the BID pack routine will round the coefficient
  rmode = rnd_mode;
  res = get_BID32 (sign_x, exponent_x, coefficient_x, rmode, pfpsf);
  BID_RETURN (res);

}
