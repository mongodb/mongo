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

// -10^-40, used in trivial path

static BID_UINT128 BID128_10PM40 =
  {{ 0x0000000000000001ull, 0xaff0000000000000ull }};

// 1 for dummy canonizing operation
                                                                             
static BID_UINT128 BID128_1 =                                                  
  {{ 0x0000000000000001ull, 0x3040000000000000ull }};                       

BID_F128_CONST_DEF( c_1em40, 3f7a16c262777579, c58c46475896767b); // 1e-40

BID128_FUNCTION_ARG1 (bid128_tanh, x)

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
//
// However, we need to handle the case of very small inputs, which can
// underflow to zero in quad and incorrectly return zero instead of the
// argument. Trap this after conversion and do a crude operation to get
// appropriate directed rounding.
//
// Note that for very large decimal128 inputs, the result of conversion
// will be infinity. However, since the binary tanh function will return
// the right answer of +/- 1 in such cases, it hardly seems worth putting
// in a special-case check, which will rarely be needed and slows down
// the usual cases.

  BIDECIMAL_CALL1(bid128_to_binary128,xd,x);

  __bid_f128_fabs(abs_xd, xd);
  if (__bid_f128_lt(abs_xd, c_1em40.v))
   { int zf;
     BIDECIMAL_CALL1_NORND_NOSTAT(bid128_isZero,zf,x);
     if (zf)
      { BIDECIMAL_CALL2(bid128_mul,res,x,BID128_1);
      }
     else
      { BIDECIMAL_CALL3(bid128_fma,res,x,BID128_10PM40,x);
      }
     BID_RETURN(res);
   }
  else
   { __bid_f128_tanh(yd, xd);
     BIDECIMAL_CALL1(binary128_to_bid128,res,yd);
     BID_RETURN(res);
   }
}
