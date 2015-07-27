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

#define BID64_0 0x31c0000000000000ull
#define BID64_1 0x31c0000000000001ull
#define BID64_NAN 0x7c00000000000000ull
#define BID64_INF 0x7800000000000000ull

BID_F80_CONST_DEF( c_one,    3fff000000000000, 0000000000000000); // 1.0
BID_F80_CONST_DEF( c_half,   3ffe000000000000, 0000000000000000); // 0.5

#if DECIMAL_CALL_BY_REFERENCE
void bid64_pow (BID_UINT64 * pres, BID_UINT64 * px, BID_UINT64 * py
                _RND_MODE_PARAM _EXC_FLAGS_PARAM _EXC_MASKS_PARAM
                _EXC_INFO_PARAM);
#else
BID_UINT64 bid64_pow (BID_UINT64 x, BID_UINT64 y
                  _RND_MODE_PARAM _EXC_FLAGS_PARAM
                  _EXC_MASKS_PARAM _EXC_INFO_PARAM);
#endif



BID_TYPE_FUNCTION_ARG2(BID_UINT64, bid64_pow, x, y)

  BID_UINT64 y_int, res;
  BID_F80_TYPE xd, yd, rd, ld, e_bin, abs_e_bin;
  int cmp_res, is_odd, is_int;
  BID_UINT64 lval_1 = BID64_1;

// We will always signal on signalling NaNs anyway

#ifdef BID_SET_STATUS_FLAGS
  if (((x & SNAN_MASK64) == SNAN_MASK64) ||
      ((y & SNAN_MASK64) == SNAN_MASK64))
   {
     __set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
   }
#endif

// We have 1^y = x^+0 = x^-0 = 1 even when x or y is a NaN

  BIDECIMAL_CALL1_NORND_NOSTAT(bid64_isZero,cmp_res,y);
  if (cmp_res && ((x & SNAN_MASK64) != SNAN_MASK64))
   { res = BID64_1;
     BID_RETURN(res);
   }

  BIDECIMAL_CALL2_NORND(bid64_quiet_equal,cmp_res,x,lval_1);
  if (cmp_res && ((y & SNAN_MASK64) != SNAN_MASK64))
   { res = BID64_1;
     BID_RETURN(res);
   }

// Otherwise a NaN input leads to a NaN result.
// Just return the same NaN, quieted and canonized

  if ((x & NAN_MASK64) == NAN_MASK64)
   { res = x & 0xfc03ffffffffffffull;
     if ((res & 0x0003ffffffffffffull) > 999999999999999ull)
        res &= ~0x0003ffffffffffffull;
     BID_RETURN(res);
   }
  else if ((y & NAN_MASK64) == NAN_MASK64)
   { res = y & 0xfc03ffffffffffffull;
     if ((res & 0x0003ffffffffffffull) > 999999999999999ull)
        res &= ~0x0003ffffffffffffull;
     BID_RETURN(res);
   }

// Deal with other cases where second arg is infinite:
//
//  pow(-1,+-inf) = 1
//  pow(x,+inf) = +inf when |x| > 1
//  pow(x,+inf) = +0 when |x| < 1
//  pow(x,-inf) = +0 when |x| > 1
//  pow(x,-inf) = +inf when |x| < 1

  BIDECIMAL_CALL1_NORND_NOSTAT(bid64_isInf,cmp_res,y);
  if (cmp_res)
   { BID_UINT64 a = x & ~SIGNMASK64;
     BIDECIMAL_CALL2_NORND(bid64_quiet_equal,cmp_res,a,lval_1);
     if (cmp_res)
      { res = BID64_1;
        BID_RETURN(res);
      }
     BIDECIMAL_CALL2_NORND(bid64_quiet_less,cmp_res,a,lval_1);
     if (cmp_res)
        if ((y & SIGNMASK64) != 0) res = BID64_INF;
        else res = BID64_0;
     else
        if ((y & SIGNMASK64) != 0) res = BID64_0;
        else res = BID64_INF;
     BID_RETURN(res);
   }

// See if the exponent is an integer, and if so, find its parity.
// We can assume that bid64_round_integral_nearest_even returns a
// result with exponent >= 0, and if it's > 0 it's trivially even.

  BIDECIMAL_CALL1_NORND(bid64_round_integral_nearest_even, y_int, y);
  BIDECIMAL_CALL2_NORND(bid64_quiet_equal,is_int,y_int,y);
  is_odd = 0;

  if (is_int)
   { int e = (((y_int & (3ull<<61)) == (3ull<<61))
             ? (y_int >> 51) : (y_int >> 53)) & ((1ull<<10)-1);
     if ((e == 398) && (y_int & 1)) is_odd = 1;
   }

// Now the cases where the first arg is infinite:
//
//  pow(+inf,y) = 0 for y < 0
//  pow(+inf,y) = +inf for y > 0
//  and pow(-inf,y) the same with sign swapped for odd integers

  BIDECIMAL_CALL1_NORND_NOSTAT(bid64_isInf,cmp_res,x);
  if (cmp_res)
   { if ((y & SIGNMASK64) != 0) res = BID64_0;
     else res = BID64_INF;
     if (is_odd && ((x & SIGNMASK64) != 0))
        res = res ^ SIGNMASK64;
     BID_RETURN(res);
   }

// Now cases where first argument is 0, where we return +0 or +inf,
// or -0 or -inf if the second argument is an odd integer.

  BIDECIMAL_CALL1_NORND_NOSTAT(bid64_isZero,cmp_res,x);
  if (cmp_res)
   { if ((y & SIGNMASK64) != 0) {
        res = BID64_INF;
        __set_status_flags (pfpsf, BID_ZERO_DIVIDE_EXCEPTION);
     } else res = BID64_0;
     if (is_odd && ((x & SIGNMASK64) != 0))
        res = res ^ SIGNMASK64;

     BID_RETURN(res);
   }

// Finally, we can assume all arguments are finite and nonzero.
// So launch into the naive computation. But because we can be
// more discriminating about integer status prior to conversion,
// separate out the sign and correct it later.

  BIDECIMAL_CALL1 (bid64_to_binary80, xd, x);
  __bid_f80_fabs( xd, xd );
  BIDECIMAL_CALL1 (bid64_to_binary80, yd, y);
  __bid_f80_log( ld, xd );
  __bid_f80_sub( e_bin, xd, c_one.v);
  __bid_f80_fabs( abs_e_bin, e_bin);
  if ( __bid_f80_lt( abs_e_bin, c_half.v ) )
   { BID_F80_TYPE tmp_e_bin;
     BID_UINT64 e;
     BID_UINT64 local1 = BID64_1;
     BIDECIMAL_CALL2 (bid64_sub, e, x, local1);
     BIDECIMAL_CALL1 (bid64_to_binary80, tmp_e_bin, e);
     __bid_f80_sub( tmp_e_bin, e_bin, tmp_e_bin );
     __bid_f80_div( tmp_e_bin, tmp_e_bin, xd );
     __bid_f80_sub( ld, ld, tmp_e_bin );
   }
  __bid_f80_mul( rd, yd, ld );
  __bid_f80_exp( rd, rd );
  BIDECIMAL_CALL1 (binary80_to_bid64, res, rd);

// If we got a NaN from all that, then canonize it
// Also raise exception since it wasn't from the input.
// Do likewise for negative^noninteger

  if (((res & NAN_MASK64) == NAN_MASK64) ||
      (((x & SIGNMASK64) != 0) && !is_int))
   {
     #ifdef BID_SET_STATUS_FLAGS
     __set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
     #endif
     BID_RETURN(BID64_NAN);
   }

// Otherwise correct the sign.

  if (is_odd && ((x & SIGNMASK64) != 0)) res = res ^ SIGNMASK64;
  BID_RETURN(res);
}
