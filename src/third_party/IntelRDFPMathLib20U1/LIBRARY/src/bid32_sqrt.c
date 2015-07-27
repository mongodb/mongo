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

/*****************************************************************************
 *    BID64 square root
 *****************************************************************************
 *
 *  Algorithm description:
 *
 *  if(exponent_x is odd)
 *     scale coefficient_x by 10, adjust exponent
 *  - get lower estimate for number of digits in coefficient_x
 *  - scale coefficient x to between 31 and 33 decimal digits
 *  - in parallel, check for exact case and return if true
 *  - get high part of result coefficient using double precision sqrt
 *  - compute remainder and refine coefficient in one iteration (which 
 *                                 modifies it by at most 1)
 *  - result exponent is easy to compute from the adjusted arg. exponent 
 *
 ****************************************************************************/

#define BID_FUNCTION_SETS_BINARY_FLAGS
#include "bid_internal.h"
#include "bid_sqrt_macros.h"

BID_EXTERN_C double sqrt (double);


BID_TYPE_FUNCTION_ARG1(BID_UINT32, bid32_sqrt, x)
  BID_UINT64 CA, CT;
  BID_UINT32 sign_x, coefficient_x;
  BID_UINT32 Q, A10, QE, res;
  int_float tempx;
  double dq, dqe;
  int exponent_x, exponent_q, bin_expon_cx;
  int digits_x;
  int scale;

  BID_OPT_SAVE_BINARY_FLAGS()

  // unpack arguments, check for NaN or Infinity
  if (!unpack_BID32 (&sign_x, &exponent_x, &coefficient_x, x)) {
    // x is Inf. or NaN or 0
    if ((x & INFINITY_MASK32) == INFINITY_MASK32) {
      res = coefficient_x;
      if ((coefficient_x & SSNAN_MASK32) == SINFINITY_MASK32)	// -Infinity
      {
	res = NAN_MASK32;
#ifdef BID_SET_STATUS_FLAGS
	__set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
#endif
      }
#ifdef BID_SET_STATUS_FLAGS
      if ((x & SNAN_MASK32) == SNAN_MASK32)	// sNaN
	__set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
#endif
      BID_RETURN (res & QUIET_MASK32);
    }
    // x is 0
    exponent_x = (exponent_x + DECIMAL_EXPONENT_BIAS_32) >> 1;
    res = sign_x | (( exponent_x) << 23);
    BID_RETURN (res);
  }
  // x<0?
  if (sign_x && coefficient_x) {
    res = NAN_MASK32;
#ifdef BID_SET_STATUS_FLAGS
    __set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
#endif
    BID_RETURN (res);
  }
#ifdef UNCHANGED_BINARY_STATUS_FLAGS
  // (void) fegetexceptflag (&binaryflags, BID_FE_ALL_FLAGS);
#endif

  //--- get number of bits in the coefficient of x ---
  tempx.d = (float) coefficient_x;
  bin_expon_cx = ((tempx.i >> 23) & 0xff) - 0x7f;
  digits_x = bid_estimate_decimal_digits[bin_expon_cx];
  // add test for range
  if (coefficient_x >= bid_power10_index_binexp[bin_expon_cx])
    digits_x++;

  A10 = coefficient_x;
  if (!(exponent_x & 1)) {
    A10 = (A10 << 2) + A10;
    A10 += A10;
  }

  dqe = sqrt ((double) A10);
  QE = (BID_UINT32) dqe;
  if (QE * QE == A10) {
    res =
      very_fast_get_BID32 (0, (exponent_x + DECIMAL_EXPONENT_BIAS_32) >> 1,
			   QE);
#ifdef UNCHANGED_BINARY_STATUS_FLAGS
    // (void) fesetexceptflag (&binaryflags, BID_FE_ALL_FLAGS);
#endif
    BID_RETURN (res);
  }
  // if exponent is odd, scale coefficient by 10
  scale = 13 - digits_x;
  exponent_q = exponent_x + DECIMAL_EXPONENT_BIAS_32 - scale;
  scale += (exponent_q & 1);	// exp. bias is even

  CT = bid_power10_table_128[scale].w[0];
  CA = coefficient_x * CT;

  dq = sqrt (((double)CA));

  exponent_q = (exponent_q) >> 1;

#ifdef BID_SET_STATUS_FLAGS
  __set_status_flags (pfpsf, BID_INEXACT_EXCEPTION);
#endif

#ifndef IEEE_ROUND_NEAREST
#ifndef IEEE_ROUND_NEAREST_TIES_AWAY
  if (!((rnd_mode) & 3)) {
#endif
#endif
    Q = (BID_UINT32)(dq+0.5);

#ifndef IEEE_ROUND_NEAREST
#ifndef IEEE_ROUND_NEAREST_TIES_AWAY
  } else {
    Q = (BID_UINT32) dq;

    /*// get sign(sqrt(CA)-Q)
    R = CA - Q * Q;
    R = ((BID_SINT32) R) >> 31;
    D = R + R + 1;

    C4 = CA;
    Q += D;
    if ((BID_SINT32) (Q * Q - C4) > 0)
      Q--;*/
    if (rnd_mode == BID_ROUNDING_UP)
      Q++;
  }
#endif
#endif

  res = fast_get_BID32 (0, exponent_q, Q);
#ifdef UNCHANGED_BINARY_STATUS_FLAGS
  // (void) fesetexceptflag (&binaryflags, BID_FE_ALL_FLAGS);
#endif
  BID_RETURN (res);
}


