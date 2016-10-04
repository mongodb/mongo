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

// 2-part conversion.

BID_EXTERN_C void bid128_to_binary128_2part(BID_F128_TYPE *,BID_F128_TYPE *,BID_UINT128);

static BID_UINT128 BID128_0 =
 {BID128_LH_INIT( 0x0000000000000000ull, 0x3040000000000000ull )};

static BID_UINT128 BID128_INF =
 {BID128_LH_INIT( 0x0000000000000000ull, 0x7800000000000000ull )};

static BID_UINT128 BID128_25000 =
 {BID128_LH_INIT( 0x00000000000061a8ull, 0x3040000000000000ull )};

static BID_UINT128 BID128_N25000 =
 {BID128_LH_INIT( 0x00000000000061a8ull, 0xb040000000000000ull )};

static BID_UINT128 BID128_1 =
 {BID128_LH_INIT( 0x0000000000000001ull, 0x3040000000000000ull )};

static BID_UINT128 BID128_EXP2_11000 =
 {BID128_LH_INIT( 0x910be3407d25b9c8ull, 0x49dc6965e972d2c8ull )};
static BID_UINT128 BID128_EXP2_M11000 =
 {BID128_LH_INIT( 0x0555ddab03e9e679ull, 0x161ee6a2f56f0580ull )};

// 10^-6000, to create dummy underflowing computation

static BID_UINT128 BID128_10POWN6000 =
 {BID128_LH_INIT( 0x0000000000000001ull, 0x0160000000000000ull )};

BID_F128_CONST_DEF( c_11000, 400c57c000000000, 0000000000000000); // 11000
BID_F128_CONST_DEF(c_neg_11000, c00c57c000000000, 0000000000000000); // -11000
BID_F128_CONST_DEF( c_ln2,   3ffe62e42fefa39e, f35793c7673007e6); // ln(2)

BID128_FUNCTION_ARG1 (bid128_exp2, x)

// Declare local variables

  BID_F128_TYPE rq;
  BID_UINT128 res;
  BID_UINT128 m, n, t;
  BID_F128_TYPE mq, nq, rt;
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

// If the input is actually infinity, return +inf or 0

  if ((x.w[BID_HIGH_128W] & MASK_ANY_INF) == MASK_INF)
   { if ((x.w[BID_HIGH_128W] & MASK_SIGN) != 0)
        res = BID128_0;
     else
        res = BID128_INF;
     BID_RETURN(res);
   }

// For x = 0, return 1 exactly in all rounding modes.

  BIDECIMAL_CALL1_NORND_NOSTAT (bid128_isZero, z, x);
  if (z)
   { res = BID128_1;
     BID_RETURN(res);
   }

// For large positive inputs, use a dummy overflowing computation
// to ensure things work correctly in all rounding modes (clamping to max etc.)

  BIDECIMAL_CALL2_NORND (bid128_quiet_greater, cmp_res, x, BID128_25000);
  if (cmp_res)
   { BIDECIMAL_CALL2(bid128_mul,res,BID128_EXP2_11000,BID128_EXP2_11000);
     BID_RETURN(res);
   }

// For large negative inputs, use a dummy underflowing computation

  BIDECIMAL_CALL2_NORND (bid128_quiet_less, cmp_res, x, BID128_N25000);
  if (cmp_res)
   { BIDECIMAL_CALL2(bid128_mul,res,BID128_10POWN6000,BID128_10POWN6000);
     BID_RETURN (res);
   }

// Do a 2-part input conversion into x = nq + mq [nq being high]

  bid128_to_binary128_2part(&nq,&mq,x);

// Handle case where quad exponential would overflow.
// Otherwise, do the obvious thing.

  if (__bid_f128_gt(nq, c_11000.v))
   { __bid_f128_sub(nq, nq, c_11000.v);
     __bid_f128_exp2(rq, nq);
     __bid_f128_mul(rt, rq, c_ln2.v); //0.6931471805599453094172321214581765680755Q
     __bid_f128_mul(rt, rt, mq);
     __bid_f128_add(rq, rq, rt);
     BIDECIMAL_CALL1 (binary128_to_bid128, res, rq);
     BIDECIMAL_CALL2 (bid128_mul, res, res, BID128_EXP2_11000);
   }
  else if (__bid_f128_lt(nq, c_neg_11000.v))
   { __bid_f128_add(nq, nq, c_11000.v);
     __bid_f128_exp2(rq, nq);
     __bid_f128_mul(rt, rq, c_ln2.v); //0.6931471805599453094172321214581765680755Q
     __bid_f128_mul(rt, rt, mq);
     __bid_f128_add(rq, rq, rt);
     BIDECIMAL_CALL1 (binary128_to_bid128, res, rq);
     BIDECIMAL_CALL2 (bid128_mul, res, res, BID128_EXP2_M11000);
   }
  else
   { __bid_f128_exp2(rq, nq);
     __bid_f128_mul(rt, rq, c_ln2.v); //0.6931471805599453094172321214581765680755Q
     __bid_f128_mul(rt, rt, mq);
     __bid_f128_add(rq, rq, rt);
     BIDECIMAL_CALL1 (binary128_to_bid128, res, rq);
   }
     BID_RETURN (res);

}
