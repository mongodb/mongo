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

static BID_UINT128 BID128_MINUS_HALF =
  {BID128_LH_INIT( 0x0000000000000005ull, 0xb03e000000000000ull )};

static BID_UINT128 BID128_1 =
  {BID128_LH_INIT( 0x0000000000000001ull, 0x3040000000000000ull )};

static BID_UINT128 BID128_10POW4464 =
  { BID128_LH_INIT( 0x0000000000000001ull, 0x5320000000000000ull ) };
static BID_UINT128 BID128_10POWN4464 =
  { BID128_LH_INIT( 0x0000000000000001ull, 0x0d60000000000000ull ) };

static BID_UINT128 BID128_NAN = 
  { BID128_LH_INIT( 0x0000000000000000ull, 0x7c00000000000000ull ) };

BID_F128_CONST_DEF( c_4464_ln_10, 400c4135eb3929fb, a719f2c946d2d728); // 4454*ln(10)

BID128_FUNCTION_ARG1 (bid128_log1p, x)

// Declare local variables

  BID_UINT128 res, y, x_abs;
  int sm;
  BID_F128_TYPE xd, yd;

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

// If x < -1/2 we have condition issues with the naive computation.
// Instead, do y = 1 + x exactly in decimal and call usual log function.
// Deal with negative values 1 + x, returning NaN explicitly

  BIDECIMAL_CALL2_NORND(bid128_quiet_less,sm,x,BID128_MINUS_HALF);
  if (sm)
   { BIDECIMAL_CALL2(bid128_add,y,x,BID128_1);
     if ((y.w[BID_HIGH_128W] & MASK_SIGN) == MASK_SIGN)
      { 
        #ifdef BID_SET_STATUS_FLAGS
        __set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
        #endif
        BID_RETURN(BID128_NAN);
      }
     BIDECIMAL_CALL1(bid128_to_binary128,xd,y);
     __bid_f128_log(yd, xd);
     BIDECIMAL_CALL1(binary128_to_bid128,res,yd);
     BID_RETURN(res);
   }

// If x > 10^4464 we have overflow worries for quad. In this case,
// to all intents and purposes log(1 + x) = log(x), so we can simply
// do the same thing as for the basic log function

  BIDECIMAL_CALL2_NORND (bid128_quiet_greater,sm, x, BID128_10POW4464);
  if (sm)
   { BID_UINT128 x_mod;
     BIDECIMAL_CALL2 (bid128_mul, x_mod, x, BID128_10POWN4464);
     BIDECIMAL_CALL1 (bid128_to_binary128, xd, x_mod);
     __bid_f128_log(yd, xd);
     __bid_f128_add(yd, yd, c_4464_ln_10.v);
     BIDECIMAL_CALL1(binary128_to_bid128,res,yd);
     BID_RETURN(res);
   }

// If the input is so small it would underflow to zero in quad, the
// computation is effectively x * (1 - x/2), which we can approximate
// by (-x) * x + x just to get directed rounding sensible.

  x_abs = x;
  x_abs.w[BID_HIGH_128W] &= ~MASK_SIGN;
  BIDECIMAL_CALL2_NORND(bid128_quiet_less,sm,x_abs,BID128_10POWN4464);
  if (sm)
   { BID_UINT128 x_neg = x;
     x_neg.w[BID_HIGH_128W] ^= MASK_SIGN;
     BIDECIMAL_CALL3(bid128_fma,res,x,x_neg,x);
     BID_RETURN (res);
   }

// Otherwise just do the operation "naively".
// Inherit all other special cases (infinity, negative,...) from binary.

   { BIDECIMAL_CALL1(bid128_to_binary128,xd,x);
     __bid_f128_log1p(yd, xd);
     BIDECIMAL_CALL1(binary128_to_bid128,res,yd);
     BID_RETURN(res);
   }
}
