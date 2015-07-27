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

static BID_UINT128 BID128_1 =
  {{ 0x0000000000000001ull, 0x3040000000000000ull }};

BID_F128_CONST_DEF( c_1em40, 3f7a16c262777579, c58c46475896767b); // 1e-40
BID_F128_CONST_DEF( c_one,        3fff000000000000, 0000000000000000); // 1.0

BID128_FUNCTION_ARG1 (bid128_expm1, x)

// Declare local variables

  BID_UINT128 res, t;
  BID_F128_TYPE xd, yd, abs_xd;

// Check for NaN and just return the same NaN, quieted and canonized

  if ((x.w[BID_HIGH_128W] & NAN_MASK64) == NAN_MASK64)
   {
     #ifdef BID_SET_STATUS_FLAGS
     if (((x.w[BID_HIGH_128W] & SNAN_MASK64) == SNAN_MASK64))
        __set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
     #endif
     res.w[BID_HIGH_128W] = x.w[BID_HIGH_128W] & 0xfc003fffffffffffull;
     res.w[BID_LOW_128W] = x.w[BID_LOW_128W];
     if (((res.w[BID_HIGH_128W] & 0x00003fffffffffffull) >
          0x0000314dc6448d93ull) ||
         (((res.w[BID_HIGH_128W] & 0x00003fffffffffffull) ==
            0x0000314dc6448d93ull) &&
          res.w[BID_LOW_128W] >= 0x38c15b0a00000000ull))
      { res.w[BID_HIGH_128W] &= ~0x00003fffffffffffull;
        res.w[BID_LOW_128W] = 0ull;
      }
     BID_RETURN(res);
   }

// Convert to binary

  BIDECIMAL_CALL1(bid128_to_binary128,xd,x);

// If the input is really small, the result is about x + x^2/2, which
// we do weakly just to make sure all the directed roundings are OK.
// Treat zero specially to copy its sign

  __bid_f128_fabs(abs_xd, xd); 
  if (__bid_f128_le(abs_xd, c_1em40.v))
   { int zf;
     BIDECIMAL_CALL1_NORND_NOSTAT(bid128_isZero,zf,x);              
     if (zf)         
      { BIDECIMAL_CALL2(bid128_mul,res,x,BID128_1);
      }
     else                       
      { BIDECIMAL_CALL3(bid128_fma,res,x,x,x);
      }                                               
     BID_RETURN(res);                              
     BID_RETURN(res);
   }

// Otherwise if the input is <= 1, the naive computation is well-conditioned
// and will neither overflow nor underflow

  else if (__bid_f128_le(xd, c_one.v))
   { __bid_f128_expm1(yd, xd);
     BIDECIMAL_CALL1(binary128_to_bid128,res,yd);
     BID_RETURN(res);
   }

// Otherwise it's not bad to just call exp and subtract 1. For
// moderate results, this will be exact, and for large results, it
// will be irrelevant. Even in the awkward middle ground where
// ulp(y) is about 2, it will be OK, just another ulp at most.

  else
   { BIDECIMAL_CALL1(bid128_exp,t,x);
     BIDECIMAL_CALL2(bid128_sub,res,t,BID128_1);
     BID_RETURN(res);
   }
}
