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

double exp(double);

BID_TYPE0_FUNCTION_ARGTYPE1(BID_UINT32, bid32_exp, BID_UINT32, x)

  BID_UINT32 res;
  double xd, rd;
  int z;

    // test if x is NaN
    if ((x & NAN_MASK32) == NAN_MASK32) {
#ifdef BID_SET_STATUS_FLAGS
      if (((x & SNAN_MASK32) == SNAN_MASK32)    // sNaN
          )
        __set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
#endif
                res = x & 0xfc0fffff; //quiet and make combination 0 (canonize)
                if ((res & 0x000fffff) > 999999) { // payload
                        res &= ~0x000fffff;
                }
      BID_RETURN (res);
    }

        BIDECIMAL_CALL1_NORND_NOSTAT (bid32_isZero, z, x);
        if (z) {
                // 1 according C99
                res = 0x32800001;
           BID_RETURN (res);
        }
        BIDECIMAL_CALL1_NORND_NOSTAT (bid32_isInf, z, x);
        if (z) {
                // 0 or Inf according C99
                if (x & MASK_SIGN32) {
                        res = 0x32800000;
                } else {
                        res = 0x78000000;
                }
#ifdef BID_SET_STATUS_FLAGS
                *pfpsf = 0;
#endif
           BID_RETURN (res);
        }


// Otherwise just do the operation "naively".
// We inherit the special cases from the binary function,
// but deal with overflowing finite inputs carefully so
// things work in directed rounding modes.

  BIDECIMAL_CALL1(bid32_to_binary64,xd,x);

  if (xd > 700.0) rd = 1.0e200;
  else if (xd < -700.0) rd = 1.0e-200;
  else  rd = exp(xd);

  BIDECIMAL_CALL1(binary64_to_bid32,res,rd);

  BID_RETURN (res);

}
