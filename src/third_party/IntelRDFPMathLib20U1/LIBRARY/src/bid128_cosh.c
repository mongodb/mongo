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

// +10^-40, used in trivial path

static BID_UINT128 BID128_10PM40 =
  {BID128_LH_INIT( 0x0000000000000001ull, 0x2ff0000000000000ull )};

// Constants +1, +1/2 and -1/2 used elsewhere

static BID_UINT128 BID128_1 =
  {BID128_LH_INIT( 0x0000000000000001ull, 0x3040000000000000ull )};

static BID_UINT128 BID128_POSHALF =
  {BID128_LH_INIT( 0x0000000000000005ull, 0x303e000000000000ull )};

static BID_UINT128 BID128_NEGHALF =
  {BID128_LH_INIT( 0x0000000000000005ull, 0xb03e000000000000ull )};

static BID_UINT128 BID128_EXP_11000 =
  {BID128_LH_INIT( 0xd43ede775707fd0aull, 0x5550558ada285f8bull )};

static BID_UINT128 BID128_SHIFTER =
  {BID128_LH_INIT( 0xbe00000000000000ull, 0x3040363bf3b1ceeeull )};

// +Infinity

static BID_UINT128 BID128_INF = 
  {BID128_LH_INIT( 0x0000000000000000ull, 0x7800000000000000ull )};

BID_F128_CONST_DEF( c_1em40, 3f7a16c262777579, c58c46475896767b); // 1e-40
BID_F128_CONST_DEF(c_64,     4005000000000000, 0000000000000000); // 64
BID_F128_CONST_DEF( c_11000, 400c57c000000000, 0000000000000000); // 11000
BID_F128_CONST_DEF( c_one,   3fff000000000000, 0000000000000000); // 1.0
BID_F128_CONST_DEF( c_half,  3ffe000000000000, 0000000000000000); // 0.5
BID_F128_CONST_DEF( c_zero,  0000000000000000, 0000000000000000); // 0.0

BID128_FUNCTION_ARG1 (bid128_cosh, x)

// Declare local variables

  BID_UINT128 res;
  BID_F128_TYPE xd, yd, abs_xd, rt;

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

// Deal with infinite inputs

  if ((x.w[BID_HIGH_128W] & INFINITY_MASK64) == INFINITY_MASK64)
   { BID_RETURN(BID128_INF);
   }
  
// Convert to binary

  BIDECIMAL_CALL1(bid128_to_binary128,xd,x);

// If the input is really small, the result is about 1 + x^2/2, which
// we do weakly just to make sure all the directed roundings are OK.

  __bid_f128_fabs(abs_xd, xd);
  if (__bid_f128_le(abs_xd, c_1em40.v))
   { BIDECIMAL_CALL2(bid128_add,res,BID128_1,BID128_10PM40);
     BID_RETURN(res);
   }

// Otherwise if the input is <= 1 in magnitude, the naive computation is
// well-conditioned and will neither overflow nor underflow

  else if (__bid_f128_le(abs_xd, c_one.v))
   { __bid_f128_cosh(yd, xd);
     BIDECIMAL_CALL1(binary128_to_bid128,res,yd);
     BID_RETURN(res);
   }
  
// Otherwise, unless the input is totally huge, just "using the formula"
// cosh(x) = (e^x + e^-x) / 2 is OK, but we need to to it directly in
// decimal so that we don't hit ill-conditioning. Also use an FMA to try
// to minimize the additional rounding errors, and take care to isolate
// which is the dominant part to control these errors better: it depends
// on the sign of the input.

  else if (__bid_f128_le(abs_xd, c_64.v))
   { BID_UINT128 e, i;
     if (__bid_f128_le(c_zero.v, xd))
      { BIDECIMAL_CALL1(bid128_exp,e,x);
        BIDECIMAL_CALL2(bid128_div,i,BID128_1,e);
        BIDECIMAL_CALL2(bid128_mul,i,BID128_POSHALF,i);
        BIDECIMAL_CALL3(bid128_fma,res,e,BID128_POSHALF,i);
      }
     else
      { x.w[BID_HIGH_128W] &= 0x7FFFFFFFFFFFFFFFull;
        BIDECIMAL_CALL1(bid128_exp,e,x);
        BIDECIMAL_CALL2(bid128_div,i,BID128_1,e);
        BIDECIMAL_CALL2(bid128_mul,i,BID128_POSHALF,i);
        BIDECIMAL_CALL3(bid128_fma,res,e,BID128_POSHALF,i);
      }
     BID_RETURN (res);
   }
  
// For huge arguments, it's effectively exp |x| / 2.
// We need to copy and tweak the exp code rather than call it
// in order to avoid cases where e^x/2 < MAXNUM < e^x.

  else
   { BID_UINT128 m, n, t;
     BID_F128_TYPE rd, md, nd;
     x.w[BID_HIGH_128W] &= 0x7FFFFFFFFFFFFFFFull;
     BIDECIMAL_CALL2(bid128_add, t, x, BID128_SHIFTER);
     BIDECIMAL_CALL2(bid128_sub, n, t, BID128_SHIFTER);
     BIDECIMAL_CALL2(bid128_sub, m, x, n);
     BIDECIMAL_CALL1(bid128_to_binary128, nd, n);
     BIDECIMAL_CALL1(bid128_to_binary128, md, m);
     if (__bid_f128_gt(nd, c_11000.v))
      { __bid_f128_sub(nd, nd, c_11000.v);
        __bid_f128_exp(rt, nd);
        __bid_f128_mul(rd, c_half.v, rt);
        __bid_f128_exp(rt, md);
        __bid_f128_mul(rd, rd, rt);
        BIDECIMAL_CALL1 (binary128_to_bid128, res, rd);
        BIDECIMAL_CALL2 (bid128_mul, res, res, BID128_EXP_11000);
      }
     else
      { __bid_f128_exp(rt, nd);
        __bid_f128_mul(rd, c_half.v, rt);
        __bid_f128_exp(rt, md);
       __bid_f128_mul(rd, rd, rt); 
        BIDECIMAL_CALL1 (binary128_to_bid128, res, rd);
      }
     BID_RETURN (res);
   }
}
