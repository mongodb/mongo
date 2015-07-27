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

// Standard NaN

static BID_UINT128 BID128_NAN =
  {BID128_LH_INIT( 0x0000000000000000ull, 0x7c00000000000000ull )};

// +Infinity

static BID_UINT128 BID128_INF =
  {BID128_LH_INIT( 0x0000000000000000ull, 0x7800000000000000ull )};

// Shifter 2 * 10^33

static BID_UINT128 BID128_SHIFTER =
  {BID128_LH_INIT( 0x7182b61400000000ull, 0x3040629b8c891b26ull )};

static BID_UINT128 BID128_ZERO =
  {BID128_LH_INIT( 0x0000000000000000ull, 0x0000000000000000ull )};

BID128_FUNCTION_ARG1 (bid128_tgamma, x)

// Declare local variables

  BID_UINT128 res, y, x_int, x_frac;
  BID_F128_TYPE xd_hi, xd_lo, yd;
  int e, cmp_res;

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

// If the input is 0, return signed infinity

  BIDECIMAL_CALL1_NORND_NOSTAT(bid128_isZero,cmp_res,x);
  if (cmp_res)
   { res = BID128_INF;
     res.w[BID_HIGH_128W] ^= (x.w[BID_HIGH_128W] & SIGNMASK64);
     *pfpsf |= BID_ZERO_DIVIDE_EXCEPTION;
     BID_RETURN (res);
   }

// For infinite inputs, return NaN or infinity

  BIDECIMAL_CALL1_NORND_NOSTAT(bid128_isInf,cmp_res,x);
  if (cmp_res)
   { if ((x.w[BID_HIGH_128W] & SIGNMASK64) != 0)
      {
        res = BID128_NAN;
        #ifdef BID_SET_STATUS_FLAGS
        __set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
        #endif
      }
     else res = BID128_INF;
     BID_RETURN (res);
   }

  // check for nonpositive integers
  BIDECIMAL_CALL2_NORND (bid128_quiet_less_equal, cmp_res, x, BID128_ZERO);
  if(cmp_res) {
	BIDECIMAL_CALL1_NORND(bid128_round_integral_nearest_even, x_int, x);
	BIDECIMAL_CALL2(bid128_sub,x_frac,x,x_int);
	// If the fractional part is 0, return NaN
	BIDECIMAL_CALL1_NORND_NOSTAT(bid128_isZero,cmp_res,x_frac);
	if (cmp_res)
	{ res = BID128_NAN;
      #ifdef BID_SET_STATUS_FLAGS
        __set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
     #endif
     BID_RETURN (res);
	}
  }

// Compute lgamma and take the exponential.
// This is crude and inaccurate, but it is what the quad gamma
// function does anyway, so we can't improve things using that.

  BIDECIMAL_CALL1(bid128_lgamma,y,x);
  BIDECIMAL_CALL1(bid128_exp,res,y);

// If we somehow got a NaN from that, just give up
// The result is also final if the input is nonnegative.

  if (((res.w[BID_HIGH_128W] & NAN_MASK64) == NAN_MASK64) ||
      ((x.w[BID_HIGH_128W] & SIGNMASK64) == 0))
   { BID_RETURN(res);
   }

// Otherwise need to fix up the sign. If the input is
// negative and falls in an -odd < x < -even interval, then negate.

  BIDECIMAL_CALL1_NORND(bid128_round_integral_zero, x_int, x);
  e = ((x_int.w[BID_HIGH_128W] >> 49) & ((1ull<<14)-1));
  if (e <= 6176)
   { if (e < 6176)
      { BID_UINT128 localshifter = BID128_SHIFTER;
        BIDECIMAL_CALL2 (bid128_add, x_int, localshifter, x_int);
      }
     if ((x_int.w[BID_LOW_128W] & 1) == 0) res.w[BID_HIGH_128W] ^= SIGNMASK64;
   }

  BID_RETURN(res);
}
