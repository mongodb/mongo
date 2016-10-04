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
double acos(double);
double asin(double);
double fabs(double);
double sqrt(double);

#define BID32_1 0x32800001ul

// NaN for inputs |x| > 1

#define BID32_NAN 0x7c000000ul

BID_TYPE0_FUNCTION_ARGTYPE1(BID_UINT32, bid32_asin, BID_UINT32, x)


// Declare local variables

  BID_UINT32 res, t, t1 = BID32_1;
  double xd, td, yd;

// Check for NaN and just return the same NaN, quieted and canonized

  if ((x & NAN_MASK32) == NAN_MASK32)
   {
     #ifdef BID_SET_STATUS_FLAGS
     if ((x & SNAN_MASK32) == SNAN_MASK32)
        __set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
     #endif
     res = x & 0xfc0ffffful;
     if ((res & 0x000ffffful) > 999999ul) res &= ~0x000ffffful;
     BID_RETURN(res);
   }

// Convert to binary

  BIDECIMAL_CALL1(bid32_to_binary64,xd,x);

// If the input is not too close to +/- 1 then do it "naively"

  if (fabs(xd) <= 0.9)
   { yd = asin(xd);
     BIDECIMAL_CALL1(binary64_to_bid32,res,yd);
     BID_RETURN (res);
   }

// If the input is > 1 in magnitude, fail

  else if (fabs(xd) > 1.0)
   { res = BID32_NAN;
     #ifdef BID_SET_STATUS_FLAGS
     __set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
     #endif
     BID_RETURN(res)
   }

// Otherwise compute sqrt(1 - x^2) accurately and use acos instead.
// Use 1 - |x| as direct decimal computation, since direct fma would
// give only about working precision error near +1

  else
   { BIDECIMAL_CALL1_NORND_NOSTAT(bid32_abs,t,x);
     BIDECIMAL_CALL2(bid32_sub,t,t1,t);
     BIDECIMAL_CALL1(bid32_to_binary64,td,t);
     td = (2.0 - td) * td;
     yd = acos(sqrt(td));
     if (xd < 0.0) yd = -yd;
     BIDECIMAL_CALL1(binary64_to_bid32,res,yd);
     BID_RETURN (res);
   }
}
