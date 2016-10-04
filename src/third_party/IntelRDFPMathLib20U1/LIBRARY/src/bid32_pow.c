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

#define BID32_NAN 0x7c000000ul
#define BID32_1   0x32800001ul
#define BID32_0   0x00000000ul
#define BID32_INF 0x78000000ul

double fabs(double);
double pow(double, double);

BID_TYPE_FUNCTION_ARG2(BID_UINT32, bid32_pow, x, y)

  BID_UINT32 res, y_int;
  double xd, yd, rd;
  int cmp_res, is_int, is_odd;
  BID_UINT32 lval_1 = BID32_1;

// We will always signal on signalling NaNs anyway

#ifdef BID_SET_STATUS_FLAGS
  if (((x & SNAN_MASK32) == SNAN_MASK32) ||
      ((y & SNAN_MASK32) == SNAN_MASK32))
   {
     __set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
   }
#endif

// We have 1^y = x^+0 = x^-0 = 1 even when x or y is a NaN

  BIDECIMAL_CALL1_NORND_NOSTAT(bid32_isZero,cmp_res,y);
  if (cmp_res && ((x & SNAN_MASK32) != SNAN_MASK32))
   { res = BID32_1;
     BID_RETURN(res);
   }

  BIDECIMAL_CALL2_NORND(bid32_quiet_equal,cmp_res,x,lval_1);
  if (cmp_res && ((y & SNAN_MASK32) != SNAN_MASK32))
   { res = BID32_1;
     BID_RETURN(res);
   }

// Otherwise a NaN input leads to a NaN result.
// Just return the same NaN, quieted and canonized

  if ((x & NAN_MASK32) == NAN_MASK32)
   { res = x & 0xfc0ffffful;
     if ((res & 0x000ffffful) > 999999ul) res &= ~0x000ffffful;
     BID_RETURN(res);
   }
  else if ((y & NAN_MASK32) == NAN_MASK32)
   { res = y & 0xfc0ffffful;
     if ((res & 0x000ffffful) > 999999ul) res &= ~0x000ffffful;
     BID_RETURN(res);
   }

// Deal with other cases where second arg is infinite:
//
//  pow(-1,+-inf) = 1
//  pow(x,+inf) = +inf when |x| > 1
//  pow(x,+inf) = +0 when |x| < 1
//  pow(x,-inf) = +0 when |x| > 1
//  pow(x,-inf) = +inf when |x| < 1

  BIDECIMAL_CALL1_NORND_NOSTAT(bid32_isInf,cmp_res,y);
  if (cmp_res)
   { BID_UINT32 a = x & ~SIGNMASK32;
     BIDECIMAL_CALL2_NORND(bid32_quiet_equal,cmp_res,a,lval_1);
     if (cmp_res)
      { res = BID32_1;
        BID_RETURN(res);
      }
     BIDECIMAL_CALL2_NORND(bid32_quiet_less,cmp_res,a,lval_1);
     if (cmp_res)
        if ((y & SIGNMASK32) != 0) res = BID32_INF;
        else res = BID32_0;
     else
        if ((y & SIGNMASK32) != 0) res = BID32_0;
        else res = BID32_INF;
     BID_RETURN(res);
   }

// See if the exponent is an integer, and if so, find its parity.
// We can assume that bid32_round_integral_nearest_even returns a
// result with exponent >= 0, and if it's > 0 it's trivially even.

  BIDECIMAL_CALL1_NORND(bid32_round_integral_nearest_even, y_int, y);
  BIDECIMAL_CALL2_NORND(bid32_quiet_equal,is_int,y_int,y);
  is_odd = 0;

  if (is_int)
   { int e = (((y_int & (3ull<<29)) == (3ull<<29))
             ? (y_int >> 21) : (y_int >> 23)) & ((1ull<<8)-1);
     if ((e == 101) && (y_int & 1)) is_odd = 1;
   }

// Now the cases where the first arg is infinite:
//
//  pow(+inf,y) = 0 for y < 0
//  pow(+inf,y) = +inf for y > 0
//  and pow(-inf,y) the same with sign swapped for odd integers

  BIDECIMAL_CALL1_NORND_NOSTAT(bid32_isInf,cmp_res,x);
  if (cmp_res)
   { if ((y & SIGNMASK32) != 0) res = BID32_0;
     else res = BID32_INF;
     if (is_odd && ((x & SIGNMASK32) != 0))
        res = res ^ SIGNMASK32;
     BID_RETURN(res);
   }

// Now cases where first argument is 0, where we return +0 or +inf,
// or -0 or -inf if the second argument is an odd integer.

  BIDECIMAL_CALL1_NORND_NOSTAT(bid32_isZero,cmp_res,x);
  if (cmp_res)
   { if ((y & SIGNMASK32) != 0) {
        res = BID32_INF;
        __set_status_flags (pfpsf, BID_ZERO_DIVIDE_EXCEPTION);
     } else res = BID32_0;
     if (is_odd && ((x & SIGNMASK32) != 0))
        res = res ^ SIGNMASK32;
     BID_RETURN(res);
   }

// Finally, we can assume all arguments are finite and nonzero.
// So launch into the naive computation. But because we can be
// more discriminating about integer status prior to conversion,
// separate out the sign and correct it later.

  BIDECIMAL_CALL1 (bid32_to_binary64, xd, x);
  xd = fabs(xd);
  BIDECIMAL_CALL1 (bid32_to_binary64, yd, y);
  rd = pow(xd, yd);
  BIDECIMAL_CALL1 (binary64_to_bid32, res, rd);

// If we got a NaN from all that, then canonize it
// Also raise exception since it wasn't from the input.
// Do likewise for negative^noninteger

  if (((res & NAN_MASK32) == NAN_MASK32) ||
      (((x & SIGNMASK32) != 0) && !is_int))
   {
     #ifdef BID_SET_STATUS_FLAGS
     __set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
     #endif
     BID_RETURN(BID32_NAN);
   }

// Otherwise correct the sign.

  if (is_odd && ((x & SIGNMASK32) != 0)) res = res ^ SIGNMASK32;
  BID_RETURN(res);
}
