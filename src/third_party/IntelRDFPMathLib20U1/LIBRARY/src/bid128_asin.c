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

// -1, used in sqrt(1 - x^2) computation

static BID_UINT128 BID128_MINUS1 =
  {BID128_LH_INIT( 0x0000000000000001ull, 0xb040000000000000ull )};

// 10^-40, used in trivial path

static BID_UINT128 BID128_10PM40 =
  {BID128_LH_INIT( 0x0000000000000001ull, 0x2ff0000000000000ull )};

// NaN for inputs |x| > 1                                                   
                                                                          
static BID_UINT128 BID128_NAN =                                          
  {BID128_LH_INIT( 0x0000000000000000ull, 0x7c00000000000000ull )};                   

BID_F128_CONST_DEF( c_1em40, 3f7a16c262777579, c58c46475896767b); // 1e-40
BID_F128_CONST_DEF( c_7_10ths, 3ffe666666666666, 6666666666666666); // .7
BID_F128_CONST_DEF( c_one,     3fff000000000000, 0000000000000000); // 1.0
BID_F128_CONST_DEF( c_zero,    0000000000000000, 0000000000000000); // 0.0

BID128_FUNCTION_ARG1 (bid128_asin, x)

// Declare local variables

  BID_UINT128 res, t;
  BID_F128_TYPE xd, td, yd, abs_xd;
  BID_UINT128 tm1 = BID128_MINUS1;
  BID_UINT128 t10pm40 = BID128_10PM40;

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

// If the input is very small indeed, do a special computation, since
// the conversion to binary may already have underflowed to zero.
// The computation is just x * (1 + e), to work with directed rounding.

  __bid_f128_fabs(abs_xd, xd);
  if (__bid_f128_lt(abs_xd, c_1em40.v))
   { BIDECIMAL_CALL3(bid128_fma,res,x,t10pm40,x);
     BID_RETURN(res);
   }

// If the input is not too close to +/- 1 then do it "naively"

  if (__bid_f128_le(abs_xd, c_7_10ths.v))
   { __bid_f128_asin(yd, xd);
     BIDECIMAL_CALL1(binary128_to_bid128,res,yd);
     BID_RETURN (res);
   }

// If the input is > 1 in magnitude, fail                      
                                                                             
  else if (__bid_f128_gt(abs_xd, c_one.v))                                             
   { res = BID128_NAN;                                                      
     #ifdef BID_SET_STATUS_FLAGS                                              
     __set_status_flags (pfpsf, BID_INVALID_EXCEPTION);                  
     #endif                                                           
     BID_RETURN(res)                                                      
   }                                                              

// Otherwise compute sqrt(1 - x^2) accurately and use acos instead.

  else
   { BIDECIMAL_CALL3(bid128_fma,t,x,x,tm1);
     BIDECIMAL_CALL1(bid128_to_binary128,td,t);
     __bid_f128_neg(yd, td); 
     __bid_f128_sqrt(yd, yd);
     __bid_f128_acos(yd, yd);
     if (__bid_f128_lt(xd, c_zero.v)) __bid_f128_neg(yd, yd);
     BIDECIMAL_CALL1(binary128_to_bid128,res,yd);
     BID_RETURN (res);
   }
}
