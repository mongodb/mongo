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

#include "bid_internal.h"
double sin(double);
double tgamma(double);

#define BID32_NAN 0x7c000000ul
#define BID32_SHIFTER 0x329e8480ul
#define BID32_INF 0x78000000ul

BID_TYPE0_FUNCTION_ARGTYPE1(BID_UINT32, bid32_tgamma, BID_UINT32, x)

// Declare local variables

  BID_UINT32 res, x_int, x_frac;
  double xd, fd, yd;
  int cmp_res, e;

// Check for NaN and just return the same NaN, quieted and canonized

  if ((x & NAN_MASK32) == NAN_MASK32)
   {
     #ifdef BID_SET_STATUS_FLAGS
     if ((x & SNAN_MASK32) == SNAN_MASK32)
        __set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
     #endif
     res = x & 0xfc0ffffful;
     if ((res & 0x000ffffful) > 999999ul) res &= ~0x000ffffful;
     BID_RETURN(res);
   }

// If the input is 0, return signed infinity
// and raise division by zero

  BIDECIMAL_CALL1_NORND_NOSTAT(bid32_isZero,cmp_res,x);
  if (cmp_res)
   { res = BID32_INF ^ (x & SIGNMASK32);
     *pfpsf |= BID_ZERO_DIVIDE_EXCEPTION;
     BID_RETURN (res);
   }

// For infinite inputs, return NaN or infinity

  BIDECIMAL_CALL1_NORND_NOSTAT(bid32_isInf,cmp_res,x);
  if (cmp_res)
   { if ((x & SIGNMASK32) != 0)
      {
        res = BID32_NAN;
        #ifdef BID_SET_STATUS_FLAGS
        __set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
        #endif
      }
     else res = BID32_INF;
     BID_RETURN (res);
   }

// Convert to binary

  BIDECIMAL_CALL1(bid32_to_binary64,xd,x);

// If x >= 1/2 then we're very safe doing the operation naively.
// However, separate out very large inputs for appropriate
// clamping in directed rounding modes.

  if (xd >= 0.5)
   { if (xd >= 700.0) yd = 1e200; else yd = tgamma(xd);
     BIDECIMAL_CALL1(binary64_to_bid32,res,yd);
     BID_RETURN (res);
   }

// Otherwise, even with the huge extra precision, we may need to worry
// about the singularities at nonnegative integers. So we use the reflection
// formula
//
// Gamma(x) = pi / (sin (pi * x) * Gamma(1 - x))
//
// Form the integer and fractional parts of x, and convert fractional
// part to double.

  BIDECIMAL_CALL1_NORND(bid32_round_integral_nearest_even, x_int, x);
  BIDECIMAL_CALL2(bid32_sub,x_frac,x,x_int);

// If the fractional part is 0, return a NaN

  BIDECIMAL_CALL1_NORND_NOSTAT(bid32_isZero,cmp_res,x_frac);
  if (cmp_res)
   { res = BID32_NAN;
     #ifdef BID_SET_STATUS_FLAGS
     __set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
     #endif
     BID_RETURN (res);
   }

// Otherwise do the main computation in double.

  BIDECIMAL_CALL1(bid32_to_binary64,fd,x_frac);
  yd = 3.14159265358979323846 /
       (sin(3.14159265358979323846 * fd) *
        tgamma(1.0 - xd));

// If the integer part is odd, negate the result since
// sin(pi * x) = -sin(pi * xf)
//
// To avoid relying on the fact that bid32_round_integral_nearest_even
// gives a canonical integer, add a shifter where it might be needed.
// If the exponent is -ve then |x| < 10^6, so adding to 2 * 10^6 will
// give something with exactly the complement of digits.

  e = (((x_int & (3ull<<29)) == (3ull<<29)) ? (x_int >> 21) : (x_int >> 23)) &
      ((1ull<<8)-1);
  if (e <= 101)
   { if (e < 101)
      { BID_UINT32 localshifter = BID32_SHIFTER;
        BIDECIMAL_CALL2 (bid32_add, x_int, localshifter, x_int);
      }
     if (x_int & 1) yd = -yd;
   }

// Convert back and return

  BIDECIMAL_CALL1(binary64_to_bid32,res,yd);
  BID_RETURN (res);
}
