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

#define BID64_NAN 0x7c00000000000000ull
#define BID64_SHIFTER 0x31c0000000010000ull
#define BID64_INF 0x7800000000000000ull

BID_F80_CONST_DEF( c_pi,     4000921fb54442d1, 8469898cc51701b8); // pi
BID_F80_CONST_DEF( c_one,    3fff000000000000, 0000000000000000); // 1.0
BID_F80_CONST_DEF( c_half,   3ffe000000000000, 0000000000000000); // 0.5
BID_F80_CONST_DEF( c_8000,   400bf40000000000, 0000000000000000); // 8000
BID_F80_CONST_DEF( c_1e2000, 59f2cf6c9c9bc5f8, 84a294e53edc955f); // 1e2000

BID_TYPE0_FUNCTION_ARGTYPE1(BID_UINT64, bid64_tgamma, BID_UINT64, x)

// Declare local variables

  BID_UINT64 res, x_int, x_frac;
  BID_F80_TYPE xd, fd, yd, rt;
  int cmp_res, e;

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

// If the input is 0, return signed infinity

  BIDECIMAL_CALL1_NORND_NOSTAT(bid64_isZero,cmp_res,x);
  if (cmp_res)
   { res = BID64_INF ^ (x & SIGNMASK64);
     *pfpsf |= BID_ZERO_DIVIDE_EXCEPTION;
     BID_RETURN (res);
   }

// For infinite inputs, return NaN or infinity

  BIDECIMAL_CALL1_NORND_NOSTAT(bid64_isInf,cmp_res,x);
  if (cmp_res)
   { if ((x & SIGNMASK64) != 0)
      {
        res = BID64_NAN;
        #ifdef BID_SET_STATUS_FLAGS
        __set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
        #endif
      }
     else res = BID64_INF;
     BID_RETURN (res);
   }

// Convert to binary

  BIDECIMAL_CALL1(bid64_to_binary80,xd,x);

// If x >= 1/2 then we're very safe doing the operation naively.
// However, separate out very large inputs for appropriate
// clamping in directed rounding modes.

  if (__bid_f80_ge( xd, c_half.v ) )
   { if (__bid_f80_ge( xd, c_8000.v ) ) {
        BID_F80_ASSIGN( yd, c_1e2000);
     } else {
        __bid_f80_tgamma( yd, xd );
     } 
     BIDECIMAL_CALL1(binary80_to_bid64,res,yd);
     BID_RETURN (res);
   }

// Otherwise, even with the extra precision, we may need to worry
// about the singularities at nonnegative integers. So we use the reflection
// formula
//
// Gamma(x) = pi / (sin (pi * x) * Gamma(1 - x))
//
// Form the integer and fractional parts of x, and convert fractional
// part to double.

  BIDECIMAL_CALL1_NORND(bid64_round_integral_nearest_even, x_int, x);
  BIDECIMAL_CALL2(bid64_sub,x_frac,x,x_int);

// If the fractional part is 0, return a NaN

  BIDECIMAL_CALL1_NORND_NOSTAT(bid64_isZero,cmp_res,x_frac);
  if (cmp_res)
   { res = BID64_NAN;
     #ifdef BID_SET_STATUS_FLAGS
     __set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
     #endif
     BID_RETURN (res);
   }

// Otherwise do the main computation in double.

  BIDECIMAL_CALL1(bid64_to_binary80,fd,x_frac);
  __bid_f80_sub( rt, c_one.v, xd );
  __bid_f80_mul( yd, c_pi.v, fd );
  __bid_f80_sin( yd, yd );
  __bid_f80_tgamma( rt, rt );
  __bid_f80_mul( yd, yd, rt );
  __bid_f80_div( yd, c_pi.v, yd );

// If the integer part is odd, negate the result since
// sin(pi * x) = -sin(pi * xf)
//
// To avoid relying on the fact that bid64_round_integral_nearest_even
// gives a canonical integer, add a shifter where it might be needed.
// If the exponent is -ve then |x| < 10^6, so adding to 2 * 10^6 will
// give something with exactly the complement of digits.

  e = (((x_int & (3ull<<61)) == (3ull<<61)) ? (x_int >> 51) : (x_int >> 53)) &
      ((1ull<<10)-1);
  if (e <= 398)
   { if (e < 398)
      { BID_UINT64 localshifter = BID64_SHIFTER;
        BIDECIMAL_CALL2 (bid64_add, x_int, localshifter, x_int);
      }
     if (x_int & 1) __bid_f80_neg( yd, yd);
   }

// Convert back and return

  BIDECIMAL_CALL1(binary80_to_bid64,res,yd);
  BID_RETURN (res);
}
