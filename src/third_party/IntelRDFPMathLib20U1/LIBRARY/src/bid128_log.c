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
  {BID128_LH_INIT( 0x0000000000000001ull, 0x3040000000000000ull )};
static BID_UINT128 BID128_10POW4464 =
  {BID128_LH_INIT( 0x0000000000000001ull, 0x5320000000000000ull )};
static BID_UINT128 BID128_10POWN4464 =
  {BID128_LH_INIT( 0x0000000000000001ull, 0x0d60000000000000ull )};

BID_F128_CONST_DEF( c_one,        3fff000000000000, 0000000000000000); // 1.0
BID_F128_CONST_DEF( c_half,       3ffe000000000000, 0000000000000000); // 0.5
BID_F128_CONST_DEF( c_4464_ln_10, 400c4135eb3929fb, a719f2c946d2d728); // 4454*ln(10)

BID128_FUNCTION_ARG1 (bid128_log, x)

  BID_F128_TYPE xq, rq, abs_e_bin, rt;
  BID_UINT128 res;
  int z, cmp_res;

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

  BIDECIMAL_CALL1_NORND_NOSTAT (bid128_isZero, z, x);
  if (z)
   { // -Infinite and Divide by Zero according C99
     res.w[BID_HIGH_128W] = 0xf800000000000000ull;
     res.w[BID_LOW_128W] = 0ull;
     #ifdef BID_SET_STATUS_FLAGS
     __set_status_flags (pfpsf, BID_ZERO_DIVIDE_EXCEPTION);
     #endif
     BID_RETURN(res);
   }

  if (x.w[BID_HIGH_128W] & MASK_SIGN)
   { // QNaN Indefinite
     res.w[BID_HIGH_128W] = 0x7c00000000000000ull;
     res.w[BID_LOW_128W] = 0ull;
     #ifdef BID_SET_STATUS_FLAGS
     __set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
     #endif
     BID_RETURN (res);
   }

// Inputs too large to fit in quad.

  BIDECIMAL_CALL2_NORND(bid128_quiet_greater,cmp_res, x, BID128_10POW4464);
  if (cmp_res)
   { BID_UINT128 x_mod;
     BIDECIMAL_CALL2 (bid128_mul, x_mod, x, BID128_10POWN4464);
     BIDECIMAL_CALL1 (bid128_to_binary128, xq, x_mod);
     __bid_f128_log(rq, xq);
     __bid_f128_add(rq, rq, c_4464_ln_10.v);
     BIDECIMAL_CALL1 (binary128_to_bid128, res, rq);
     BID_RETURN (res);
   }

// Inputs so small they underflow to zero in quad.

  BIDECIMAL_CALL2_NORND(bid128_quiet_less,cmp_res,x,BID128_10POWN4464);
  if (cmp_res)
   { BID_UINT128 x_mod;
     BIDECIMAL_CALL2(bid128_mul, x_mod, x, BID128_10POW4464);
     BIDECIMAL_CALL1(bid128_to_binary128, xq, x_mod);
     __bid_f128_log(rq, xq);
     __bid_f128_sub(rq, rq, c_4464_ln_10.v);
     BIDECIMAL_CALL1 (binary128_to_bid128, res, rq);
     BID_RETURN (res);
   }

// Ordinary inputs

  else
   { BID_F128_TYPE e_bin;
     BIDECIMAL_CALL1 (bid128_to_binary128, xq, x);
     __bid_f128_log(rq, xq);
     __bid_f128_sub(e_bin, xq, c_one.v);
     __bid_f128_fabs(abs_e_bin, e_bin);
     if (__bid_f128_lt(abs_e_bin, c_half.v))
      { BID_F128_TYPE tmp_e_bin;
        BID_UINT128 e;
        BIDECIMAL_CALL2 (bid128_sub, e, x, BID128_1);
        BIDECIMAL_CALL1 (bid128_to_binary128, tmp_e_bin, e);
        __bid_f128_sub(rt, e_bin, tmp_e_bin);
        __bid_f128_div(rt, rt, xq);
        __bid_f128_sub(rq, rq, rt);
      }
     BIDECIMAL_CALL1 (binary128_to_bid128, res, rq);
     BID_RETURN (res);
   }
}
