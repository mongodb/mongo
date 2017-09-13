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

BID_F80_CONST_DEF( c_half,  3ffe000000000000, 0000000000000000); // 0.5
BID_F80_CONST_DEF( c_one,   3fff000000000000, 0000000000000000); // 1.0
BID_F80_CONST_DEF( c_pi,    4000921fb54442d1, 8469898cc51701b8); // pi
BID_F80_CONST_DEF( c_ln_pi, 3fff250d048e7a1b, d0bd5f956c6a843f); // ln(pi)

#define BID64_INF 0x7800000000000000ull

BID_TYPE0_FUNCTION_ARGTYPE1(BID_UINT64, bid64_lgamma, BID_UINT64, x)

// Declare local variables

  BID_UINT64 res, x_int, x_frac;
  BID_F128_TYPE xd, yd, fd, rt;
  int cmp_res;

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

  BIDECIMAL_CALL1(bid64_to_binary128,xd,x);

// If x >= 1/2 then we're just safe doing the operation naively;
// the condition at the top is a bit marginal but within reasonable bounds.
// This applies even to the case x = +inf where lgamma(x) = +inf

  if (__bid_f128_ge(xd, c_half.v) )
   { 
	 __bid_f128_lgamma(yd,xd);
	 BIDECIMAL_CALL1(binary128_to_bid64,res,yd); 
     BID_RETURN (res);
   }

// Filter out the case of negative infinity, where we return +inf

  BIDECIMAL_CALL1_NORND_NOSTAT(bid64_isInf,cmp_res,x);
  if (cmp_res)
   { res = BID64_INF;
     BID_RETURN (res);
   }

// Otherwise, even with the extra precision, we may need to worry
// about the singularities at nonnegative integers. So we use the reflection
// formula
//
// Gamma(x) = pi / (sin (pi * x) * Gamma(1 - x))
// log|Gamma(x)| = log pi - lgamma(1 - x) - log|sin(pi * x)|
//
// Form the integer and fractional parts of x, and convert fractional
// part to long double.

  BIDECIMAL_CALL1_NORND(bid64_round_integral_nearest_even, x_int, x);
  BIDECIMAL_CALL2(bid64_sub,x_frac,x,x_int);

// If the fractional part is 0, return +inf

  BIDECIMAL_CALL1_NORND_NOSTAT(bid64_isZero,cmp_res,x_frac);
  if (cmp_res)
   { res = BID64_INF;
      #ifdef BID_SET_STATUS_FLAGS
        __set_status_flags (pfpsf, BID_ZERO_DIVIDE_EXCEPTION);
     #endif
     BID_RETURN (res);
   }

  __bid_f128_lgamma(yd, xd);

  BIDECIMAL_CALL1(binary128_to_bid64,res,yd);
  BID_RETURN (res);
}
