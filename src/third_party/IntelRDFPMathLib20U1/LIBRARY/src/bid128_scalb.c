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

#define BID_128RES
#include "bid_internal.h"

#define DECIMAL_EXPONENT_BIAS_128 6176
#define MAX_DECIMAL_EXPONENT_128  12287



BID128_FUNCTION_ARG128_CUSTOMARGTYPE2 (bid128_scalbn, x, int, n)

     BID_UINT128 CX, CX2, CBID_X8, res;
     BID_SINT64 exp64;
     BID_UINT64 sign_x;
     int exponent_x, rmode;

  // unpack arguments, check for NaN or Infinity
if (!unpack_BID128_value (&sign_x, &exponent_x, &CX, x)) {
    // x is Inf. or NaN or 0
#ifdef BID_SET_STATUS_FLAGS
if ((x.w[1] & SNAN_MASK64) == SNAN_MASK64)	// y is sNaN
  __set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
#endif
res.w[1] = CX.w[1] & QUIET_MASK64;
res.w[0] = CX.w[0];
if (!CX.w[1]) {
       exp64 = (BID_SINT64) exponent_x + (BID_SINT64) n;
	   if(exp64<0) exp64=0;
	   if(exp64>MAX_DECIMAL_EXPONENT_128) exp64=MAX_DECIMAL_EXPONENT_128;
       exponent_x = exp64;
  bid_get_BID128_very_fast (&res, sign_x, exponent_x, CX);
}
BID_RETURN (res);
}

exp64 = (BID_SINT64) exponent_x + (BID_SINT64) n;
exponent_x = exp64;

if ((BID_UINT32) exponent_x <= MAX_DECIMAL_EXPONENT_128) {
  bid_get_BID128_very_fast (&res, sign_x, exponent_x, CX);
  BID_RETURN (res);
}
  // check for overflow
if (exp64 > MAX_DECIMAL_EXPONENT_128) {
  if (CX.w[1] < 0x314dc6448d93ull) {
    // try to normalize coefficient
    do {
      CBID_X8.w[1] = (CX.w[1] << 3) | (CX.w[0] >> 61);
      CBID_X8.w[0] = CX.w[0] << 3;
      CX2.w[1] = (CX.w[1] << 1) | (CX.w[0] >> 63);
      CX2.w[0] = CX.w[0] << 1;
      __add_128_128 (CX, CX2, CBID_X8);

      exponent_x--;
      exp64--;
    }
    while (CX.w[1] < 0x314dc6448d93ull
	   && exp64 > MAX_DECIMAL_EXPONENT_128);

  }
  if (exp64 <= MAX_DECIMAL_EXPONENT_128) {
    bid_get_BID128_very_fast (&res, sign_x, exponent_x, CX);
    BID_RETURN (res);
  } else
    exponent_x = 0x7fffffff;	// overflow
}
  // exponent < 0
  // the BID pack routine will round the coefficient
rmode = rnd_mode;
bid_get_BID128 (&res, sign_x, exponent_x, CX, (unsigned int *) &rmode,
	    pfpsf);
BID_RETURN (res);

}
