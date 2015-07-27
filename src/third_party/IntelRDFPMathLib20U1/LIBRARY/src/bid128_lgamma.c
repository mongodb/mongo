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

// 1/2
static BID_UINT128 BID128_HALF =
  {BID128_LH_INIT( 0x0000000000000005ull, 0x303e000000000000ull )};

// log(2 pi) / 2
static BID_UINT128 BID128_LOG_2PI_OVER_2 =
  {BID128_LH_INIT( 0x8512e0b1f71b1870ull, 0x2ffdc512596bf2beull )};

BID_F128_CONST_DEF(c_m1e34,      c06fed09defd561e, 75b290c510000000); // -1.000001e34Q
BID_F128_CONST_DEF(c_1e34,       406fed09defd561e, 75b290c510000000); // 1.000001e34Q
BID_F128_CONST_DEF(c_1_plus_eps, 3fff250d048e7a1b, 0000000000000034); // 1+1e-34
BID_F128_CONST_DEF(c_log_pi,     3fff250d048e7a1b, d0bd5f956c6a843f); // log(pi)
BID_F128_CONST_DEF(c_one,        3fff000000000000, 0000000000000000); // 1.
BID_F128_CONST_DEF(c_minus_one,  bfff000000000000, 0000000000000000); // -1.
BID_F128_CONST_DEF(c_half,       3ffe000000000000, 0000000000000000); // .5
BID_F128_CONST_DEF(c_1em100,     3eb2bff2ee48e052, fd7ab2f0fc572779); // 1e-100
BID_F128_CONST_DEF(c_pi,         4000921fb54442d1, 8469898cc51701b8); // pi

BID128_FUNCTION_ARG1 (bid128_lgamma, x)

// Declare local variables

  BID_UINT128 res, x_int, x_frac;
  BID_F128_TYPE xd_hi, xd_up, xd_lo, yd, zd, fd, xd_tmp, xd_rem, rd, rt, abs_xd_hi;
  int cmp_res;

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
          res.w[0] >= 0x38c15b0a00000000ull))
      { res.w[BID_HIGH_128W] &= ~0x00003fffffffffffull;
        res.w[BID_LOW_128W] = 0ull;
      }
     BID_RETURN(res);
   }

// If the input is 0, return +infinity

  BIDECIMAL_CALL1_NORND_NOSTAT(bid128_isZero,cmp_res,x);
  if (cmp_res)
   { res = BID128_INF;
      #ifdef BID_SET_STATUS_FLAGS
        __set_status_flags (pfpsf, BID_ZERO_DIVIDE_EXCEPTION);
     #endif
    BID_RETURN (res);
   }

// For infinite inputs, return +infinity

  BIDECIMAL_CALL1_NORND_NOSTAT(bid128_isInf,cmp_res,x);
  if (cmp_res)
   { res = BID128_INF;
     BID_RETURN (res);
   }

// Perform 2-part conversion to quad

  bid128_to_binary128_2part(&xd_hi,&xd_lo,x);

// If x <= -10^34 then it's a nonnegative integer so return NaN
// Leave a little slop in comparison just in case.

  if (__bid_f128_le(xd_hi, c_m1e34.v))
   { res = BID128_INF;
     #ifdef BID_SET_STATUS_FLAGS
     __set_status_flags (pfpsf, BID_ZERO_DIVIDE_EXCEPTION);
     #endif
     BID_RETURN (res);
   }

// Otherwise if x >= 10^34, we may if it's much more than that need to
// worry about the quad lgamma overflowing, but by even 10^34 it's safe
// to just use the top terms of Stirling's approximation
//
// log(Gamma(x)) = (x - 1/2) * log(x) - x + log(2 * pi) / 2
//
// We would be safe doing the operation in binary since log is
// well-conditioned at that point, except that we need also to
// worry about overflow. So we basically do it all in decimal.

  if (__bid_f128_ge(xd_hi, c_1e34.v))
   { BID_UINT128 lg1, lg2, lg3;
     BIDECIMAL_CALL2(bid128_sub,lg1,x,BID128_HALF);
     BIDECIMAL_CALL1(bid128_log,lg2,x);
     BIDECIMAL_CALL2(bid128_sub,lg3,BID128_LOG_2PI_OVER_2,x);
     BIDECIMAL_CALL3(bid128_fma,res,lg1,lg2,lg3);
     BID_RETURN (res);
   }

// Check that the input is not exactly a nonpositive integer.
// If it is, then return +infinity as usual.

  if (__bid_f128_le(xd_hi, c_half.v))
   { BIDECIMAL_CALL1_NORND(bid128_round_integral_nearest_even, x_int, x);
     BIDECIMAL_CALL2_NORND(bid128_quiet_equal,cmp_res,x_int,x);
     if (cmp_res)
      { res = BID128_INF;
      #ifdef BID_SET_STATUS_FLAGS
        __set_status_flags (pfpsf, BID_ZERO_DIVIDE_EXCEPTION);
      #endif
        BID_RETURN (res);
      }
   }

// Given that the input is *not* a nonpositive integer, neither is
// xd_hi, since |xd_lo| <= xd_hi / 2^113, whereas if x is around n,
// |x - n| / |x| >= 10^-34 >= 2^-113.
//
// Otherwise, we can assume |x| <= 1.000001e34, and this means that
// |x| < 2^113, so if x is an exact integer, we will have xd_hi = x
// and xd_lo = 0.

// Otherwise, if x >= 0.5, use the binary function but make a
// simple interpolating correction for the low part of the conversion

  if (__bid_f128_ge(xd_hi, c_half.v))
   { __bid_f128_lgamma(yd, xd_hi);
     __bid_f128_mul(xd_up, c_1_plus_eps.v, xd_hi);
     __bid_f128_nextafter(xd_up, xd_hi, xd_up);
     __bid_f128_lgamma(zd, xd_up);
     __bid_f128_sub(rt, zd, yd);
     __bid_f128_sub(rd, xd_up, xd_hi);
     __bid_f128_div(rd, xd_lo, rd);
     __bid_f128_mul(rd, rd, rt);
     __bid_f128_add(yd, yd, rd);
     BIDECIMAL_CALL1(binary128_to_bid128,res,yd);
     BID_RETURN(res);
   }

// Handle the case of really tiny inputs, where the computation
// might otherwise underflow or become inaccurate.
// By the reflection formula we have
//
// Gamma(e) = pi/(sin(pi e) * Gamma(1 - e)) =~= 1/e
// so return -log|e|.
  __bid_f128_fabs(abs_xd_hi, xd_hi);
  if (__bid_f128_le(abs_xd_hi, c_1em100.v))
   { x.w[BID_HIGH_128W] &= ~SIGNMASK64;
     BIDECIMAL_CALL1(bid128_log,res,x);
     res.w[BID_HIGH_128W] ^= SIGNMASK64;
     BID_RETURN(res);
   }
// Otherwise we have even more condition worries: do all that *and*
// factor out the singularities using the reflection formula
//
// Gamma(x) = pi / (sin (pi * x) * Gamma(1 - x))
// log|Gamma(x)| = log pi - lgamma(1 - x) - log|sin(pi * x)|
//
// Form the integer and fractional parts of x, and convert fractional
// part to quad.

  BIDECIMAL_CALL1_NORND(bid128_round_integral_nearest_even, x_int, x);
  BIDECIMAL_CALL2(bid128_sub,x_frac,x,x_int);

/*// If the fractional part is 0, return Inf

  BIDECIMAL_CALL1_NORND_NOSTAT(bid128_isZero,cmp_res,x_frac);
  if (cmp_res)
   { res = BID128_INF;
      #ifdef BID_SET_STATUS_FLAGS
        __set_status_flags (pfpsf, BID_ZERO_DIVIDE_EXCEPTION);
     #endif
     BID_RETURN (res);
   }*/

// Get representation x_hi + x_lo = 1 - x
// Maintain 2-part accuracy by appropriate compensated sum.
//
// Note: if x <= 0 then |1 - x| = 1 + |x| >= |x|
// while if 0 <= x <= 1/2 then |1 - x| >= 1/2 >= |x|
// so in either case the low part is still proportionally small.
// and we can then just add up the tails.
  
  __bid_f128_sub(xd_tmp, c_one.v, xd_hi);

  if (__bid_f128_le(xd_hi, c_minus_one.v))
   { __bid_f128_add(xd_rem, xd_tmp, xd_hi);
     __bid_f128_sub(xd_rem, c_one.v, xd_rem);
   }
  else
   { __bid_f128_sub(xd_rem, c_one.v, xd_tmp);
     __bid_f128_sub(xd_rem, xd_rem, xd_hi);
   }

  xd_hi = xd_tmp;
  __bid_f128_sub(xd_lo, xd_rem, xd_lo);

// Compute lgamma(1 - x) using exactly the same interpolating correction
// as before.

  __bid_f128_lgamma(yd, xd_hi);
  __bid_f128_mul(xd_up, c_1_plus_eps.v, xd_hi);
  __bid_f128_lgamma(zd, xd_up);
   __bid_f128_sub(rt, zd, yd);
   __bid_f128_sub(rd, xd_up, xd_hi);
   __bid_f128_div(rd, xd_lo, rd);
   __bid_f128_mul(rd, rd, rt);
   __bid_f128_add(yd, yd, rd);
// Perform the rest of the computation in quad.
//
// NB: this is not really perfect because we may get cancellation
// when the overall gamma function is close to 1, and hence lgamma
// is small and errors in the bits get blown up.

  BIDECIMAL_CALL1(bid128_to_binary128,fd,x_frac);
   __bid_f128_mul(rt, c_pi.v, fd);
   __bid_f128_sin(rt, rt);
   __bid_f128_fabs(rt, rt);
   __bid_f128_log(rt, rt);
   __bid_f128_sub(rt, c_log_pi.v, rt);
   __bid_f128_sub(yd, rt, yd);

// Convert back and return.

  BIDECIMAL_CALL1(binary128_to_bid128,res,yd);
  BID_RETURN (res);
}
