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

#define BID64_1 0x31c0000000000001ull

// NaN for values |x| > 1

#define BID64_NAN 0x7c00000000000000ull

BID_F80_CONST_DEF( c_9_10ths, 3ffecccccccccccc, cccccccccccccccd); // .9
BID_F80_CONST_DEF( c_one,     3fff000000000000, 0000000000000000); // 1.0
BID_F80_CONST_DEF( c_two,     4000000000000000, 0000000000000000); // 2.0
BID_F80_CONST_DEF( c_zero,    0000000000000000, 0000000000000000); // 0.0

BID_TYPE0_FUNCTION_ARGTYPE1(BID_UINT64, bid64_asin, BID_UINT64, x)

// Declare local variables

  BID_UINT64 res, t, t1 = BID64_1;
  BID_F80_TYPE xd, td, yd, abs_xd, rt;

// Check for NaN and just return the same NaN, quieted and canonized

  if ((x & NAN_MASK64) == NAN_MASK64)
   {
     #ifdef BID_SET_STATUS_FLAGS
     if ((x & SNAN_MASK64) == SNAN_MASK64)
        __set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
     #endif
     res = x & 0xfc03ffffffffffffull;
     if ((res & 0x0003ffffffffffffull) > 999999999999999ull)
        res &= ~0x0003ffffffffffffull;
     BID_RETURN(res);
   }

// Convert to binary

  BIDECIMAL_CALL1(bid64_to_binary80,xd,x);

// If the input is not too close to +/- 1 then do it "naively"

   __bid_f80_fabs(abs_xd, xd);
  if (__bid_f80_le(abs_xd, c_9_10ths.v))
   { __bid_f80_asin( yd, xd);
     BIDECIMAL_CALL1(binary80_to_bid64,res,yd);
     BID_RETURN (res);
   }

// If the input is > 1 in magnitude, fail

  else if (__bid_f80_gt(abs_xd, c_one.v))
   { res = BID64_NAN;
     #ifdef BID_SET_STATUS_FLAGS
     __set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
     #endif
     BID_RETURN(res)
   }

// Otherwise compute sqrt(1 - x^2) accurately and use acos instead.
// Use 1 - |x| as direct decimal computation, since direct fma would
// give only about working precision error near +1

  else
   { BIDECIMAL_CALL1_NORND_NOSTAT(bid64_abs,t,x);
     BIDECIMAL_CALL2(bid64_sub,t,t1,t);
     BIDECIMAL_CALL1(bid64_to_binary80,td,t);
     __bid_f80_sub(rt, c_two.v, td);
     __bid_f80_mul(td, rt, td);
     __bid_f80_sqrt(yd, td);
     __bid_f80_acos(yd, yd);
     if (__bid_f80_lt(xd, c_zero.v) ) __bid_f80_neg(yd, yd);
     BIDECIMAL_CALL1(binary80_to_bid64,res,yd);
     BID_RETURN (res);
   }
}
