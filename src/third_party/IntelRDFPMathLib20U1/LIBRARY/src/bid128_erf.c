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

// 2/sqrt(pi), used for cases where conversion would underflow

static BID_UINT128 BID128_2_OVER_SQRTPI =
 {BID128_LH_INIT( 0xf009a099f5c1b689ull, 0x2ffe37a225baa150ull )};

BID_F128_CONST_DEF( c_1em2000, 260b1ad56d712a5d, 7f02384e5ded39be); // 1e-2000

BID128_FUNCTION_ARG1 (bid128_erf, x)

// Declare local variables

  BID_UINT128 res;
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

// Otherwise just do the operation "naively".
// We inherit the erf([-]inf) = [-]1 case from the binary function,
// rather than having a special case for it. This applies to the cases
// where the input is actually an infinity, or where it is so large
// that it overflows or underflows in quad.
//
// The only special treatment is for the case where we'd underflow
// in quad; but then the answer is just [2/sqrt(pi)] * x

  BIDECIMAL_CALL1(bid128_to_binary128,xd,x);
  __bid_f128_fabs(abs_xd, xd);
  if (__bid_f128_lt(abs_xd, c_1em2000.v))
   { BIDECIMAL_CALL2(bid128_mul,res,BID128_2_OVER_SQRTPI,x);
   }
  else
   { __bid_f128_erf(yd, xd);
     BIDECIMAL_CALL1(binary128_to_bid128,res,yd);
   }
  BID_RETURN(res);
}
