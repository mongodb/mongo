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

#define BID_128RES
#define BID_FUNCTION_SETS_BINARY_FLAGS
#include "bid_internal.h"
#include "bid_sqrt_macros.h"

BID_EXTERN_C double sqrt (double);

BID_TYPE_FUNCTION_ARG1(BID_UINT64, bid64_sqrt, x)
  BID_UINT128 CA, CT;
  BID_UINT64 sign_x, coefficient_x;
  BID_UINT64 Q, Q2, A10, C4, R, R2, QE, res;
  BID_SINT64 D;
  int_double t_scale;
  int_float tempx;
  double da, dq, da_h, da_l, dqe;
  int exponent_x, exponent_q, bin_expon_cx;
  int digits_x;
  int scale;

  BID_OPT_SAVE_BINARY_FLAGS()

  // unpack arguments, check for NaN or Infinity
  if (!unpack_BID64 (&sign_x, &exponent_x, &coefficient_x, x)) {
    // x is Inf. or NaN or 0
    if ((x & INFINITY_MASK64) == INFINITY_MASK64) {
      res = coefficient_x;
      if ((coefficient_x & SSNAN_MASK64) == SINFINITY_MASK64)	// -Infinity
      {
	res = NAN_MASK64;
#ifdef BID_SET_STATUS_FLAGS
	__set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
#endif
      }
#ifdef BID_SET_STATUS_FLAGS
      if ((x & SNAN_MASK64) == SNAN_MASK64)	// sNaN
	__set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
#endif
      BID_RETURN_VAL (res & QUIET_MASK64);
    }
    // x is 0
    exponent_x = (exponent_x + DECIMAL_EXPONENT_BIAS) >> 1;
    res = sign_x | (((BID_UINT64) exponent_x) << 53);
    BID_RETURN_VAL (res);
  }
  // x<0?
  if (sign_x && coefficient_x) {
    res = NAN_MASK64;
#ifdef BID_SET_STATUS_FLAGS
    __set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
#endif
    BID_RETURN_VAL (res);
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
  if (exponent_x & 1) {
    A10 = (A10 << 2) + A10;
    A10 += A10;
  }

  dqe = sqrt ((double) A10);
//dq=(double)A10;  dqe=sqrt(dq); 
  QE = (BID_UINT32) dqe;
//printf("QE=%I64d, A10=%I64d, P=%I64d, dq=%016I64x,dqe=%016I64x\n",QE,A10,QE*QE,*(BID_UINT64*)&dq,*(BID_UINT64*)&dqe);
  if (QE * QE == A10) {
    res =
      very_fast_get_BID64 (0, (exponent_x + DECIMAL_EXPONENT_BIAS) >> 1,
			   QE);
#ifdef UNCHANGED_BINARY_STATUS_FLAGS
    // (void) fesetexceptflag (&binaryflags, BID_FE_ALL_FLAGS);
#endif
    BID_RETURN_VAL (res);
  }
  // if exponent is odd, scale coefficient by 10
  scale = 31 - digits_x;
  exponent_q = exponent_x - scale;
  scale += (exponent_q & 1);	// exp. bias is even

  CT = bid_power10_table_128[scale];
  __mul_64x128_short (CA, coefficient_x, CT);

  // 2^64
  t_scale.i = 0x43f0000000000000ull;
  // convert CA to DP
  da_h = CA.w[1];
  da_l = CA.w[0];
  da = da_h * t_scale.d + da_l;

  dq = sqrt (da);

  Q = (BID_UINT64) dq;

  // get sign(sqrt(CA)-Q)
  R = CA.w[0] - Q * Q;
  R = ((BID_SINT64) R) >> 63;
  D = R + R + 1;

  exponent_q = (exponent_q + DECIMAL_EXPONENT_BIAS) >> 1;

#ifdef BID_SET_STATUS_FLAGS
  __set_status_flags (pfpsf, BID_INEXACT_EXCEPTION);
#endif

#ifndef IEEE_ROUND_NEAREST
#ifndef IEEE_ROUND_NEAREST_TIES_AWAY
  if (!((rnd_mode) & 3)) {
#endif
#endif

    // midpoint to check
    Q2 = Q + Q + D;
    C4 = CA.w[0] << 2;

    // get sign(-sqrt(CA)+Midpoint)
    R2 = Q2 * Q2 - C4;
    R2 = ((BID_SINT64) R2) >> 63;

    // adjust Q if R!=R2
    Q += (D & (R ^ R2));
#ifndef IEEE_ROUND_NEAREST
#ifndef IEEE_ROUND_NEAREST_TIES_AWAY
  } else {
    C4 = CA.w[0];
    Q += D;
    if ((BID_SINT64) (Q * Q - C4) > 0)
      Q--;
    if (rnd_mode == BID_ROUNDING_UP)
      Q++;
  }
#endif
#endif

  res = fast_get_BID64 (0, exponent_q, Q);
#ifdef UNCHANGED_BINARY_STATUS_FLAGS
  // (void) fesetexceptflag (&binaryflags, BID_FE_ALL_FLAGS);
#endif
  BID_RETURN_VAL (res);
}


BID_TYPE0_FUNCTION_ARG1 (BID_UINT64, bid64q_sqrt, x)

     BID_UINT256 M256, C4, C8;
     BID_UINT128 CX, CX2, A10, S2, T128, CS, CSM, CS2, C256, CS1,
       mul_factor2_long = { {0x0ull, 0x0ull} }, QH, Tmp, TP128, Qh, Ql;
BID_UINT64 sign_x, Carry, B10, res, mul_factor, mul_factor2 = 0x0ull, CS0;
BID_SINT64 D;
int_float fx, f64;
int exponent_x, bin_expon_cx, done = 0;
int digits, scale, exponent_q = 0, exact = 1, amount, extra_digits;

  BID_OPT_SAVE_BINARY_FLAGS()

	// unpack arguments, check for NaN or Infinity
if (!unpack_BID128_value (&sign_x, &exponent_x, &CX, x)) {
  res = CX.w[1];
  // NaN ?
  if ((x.w[1] & 0x7c00000000000000ull) == 0x7c00000000000000ull) {
#ifdef BID_SET_STATUS_FLAGS
    if ((x.w[1] & 0x7e00000000000000ull) == 0x7e00000000000000ull)	// sNaN
      __set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
#endif
    Tmp.w[1] = (CX.w[1] & 0x00003fffffffffffull);
    Tmp.w[0] = CX.w[0];
    TP128 = bid_reciprocals10_128[18];
    __mul_128x128_full (Qh, Ql, Tmp, TP128);
    amount = bid_recip_scale[18];
    __shr_128 (Tmp, Qh, amount);
    res = (CX.w[1] & 0xfc00000000000000ull) | Tmp.w[0];
    BID_RETURN_VAL (res);
  }
  // x is Infinity?
  if ((x.w[1] & 0x7800000000000000ull) == 0x7800000000000000ull) {
    if (sign_x) {
      // -Inf, return NaN
      res = 0x7c00000000000000ull;
#ifdef BID_SET_STATUS_FLAGS
      __set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
#endif
    }
    BID_RETURN_VAL (res);
  }
  // x is 0 otherwise

  exponent_x =
    ((exponent_x - DECIMAL_EXPONENT_BIAS_128) >> 1) +
    DECIMAL_EXPONENT_BIAS;
  if (exponent_x < 0)
    exponent_x = 0;
  if (exponent_x > DECIMAL_MAX_EXPON_64)
    exponent_x = DECIMAL_MAX_EXPON_64;
  //res= sign_x | (((BID_UINT64)exponent_x)<<53);
  res = get_BID64 (sign_x, exponent_x, 0, rnd_mode, pfpsf);
  BID_RETURN_VAL (res);
}
if (sign_x) {
  res = 0x7c00000000000000ull;
#ifdef BID_SET_STATUS_FLAGS
  __set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
#endif
  BID_RETURN_VAL (res);
}
#ifdef UNCHANGED_BINARY_STATUS_FLAGS
// (void) fegetexceptflag (&binaryflags, BID_FE_ALL_FLAGS);
#endif

	   // 2^64
f64.i = 0x5f800000;

	   // fx ~ CX
fx.d = (float) CX.w[1] * f64.d + (float) CX.w[0];
bin_expon_cx = ((fx.i >> 23) & 0xff) - 0x7f;
digits = bid_estimate_decimal_digits[bin_expon_cx];

A10 = CX;
if (exponent_x & 1) {
  A10.w[1] = (CX.w[1] << 3) | (CX.w[0] >> 61);
  A10.w[0] = CX.w[0] << 3;
  CX2.w[1] = (CX.w[1] << 1) | (CX.w[0] >> 63);
  CX2.w[0] = CX.w[0] << 1;
  __add_128_128 (A10, A10, CX2);
}

C256.w[1] = A10.w[1];
C256.w[0] = A10.w[0];
CS.w[0] = short_sqrt128 (A10);
CS.w[1] = 0;
mul_factor = 0;
	   // check for exact result  
if (CS.w[0] < 10000000000000000ull) {
  if (CS.w[0] * CS.w[0] == A10.w[0]) {
    __sqr64_fast (S2, CS.w[0]);
    if (S2.w[1] == A10.w[1])	// && S2.w[0]==A10.w[0])
    {
      res =
	get_BID64 (0,
		   ((exponent_x - DECIMAL_EXPONENT_BIAS_128) >> 1) +
		   DECIMAL_EXPONENT_BIAS, CS.w[0], rnd_mode, pfpsf);
#ifdef UNCHANGED_BINARY_STATUS_FLAGS
      // (void) fesetexceptflag (&binaryflags, BID_FE_ALL_FLAGS);
#endif
      BID_RETURN_VAL (res);
    }
  }
  if (CS.w[0] >= 1000000000000000ull) {
    done = 1;
    exponent_q = exponent_x;
    C256.w[1] = A10.w[1];
    C256.w[0] = A10.w[0];
  }
#ifdef BID_SET_STATUS_FLAGS
  __set_status_flags (pfpsf, BID_INEXACT_EXCEPTION);
#endif
  exact = 0;
} else {
  B10 = 0x3333333333333334ull;
  __mul_64x64_to_128_full (CS2, CS.w[0], B10);
  CS0 = CS2.w[1] >> 1;
  if (CS.w[0] != ((CS0 << 3) + (CS0 << 1))) {
#ifdef BID_SET_STATUS_FLAGS
    __set_status_flags (pfpsf, BID_INEXACT_EXCEPTION);
#endif
    exact = 0;
  }
  done = 1;
  CS.w[0] = CS0;
  exponent_q = exponent_x + 2;
  mul_factor = 10;
  mul_factor2 = 100;
  if (CS.w[0] >= 10000000000000000ull) {
    __mul_64x64_to_128_full (CS2, CS.w[0], B10);
    CS0 = CS2.w[1] >> 1;
    if (CS.w[0] != ((CS0 << 3) + (CS0 << 1))) {
#ifdef BID_SET_STATUS_FLAGS
      __set_status_flags (pfpsf, BID_INEXACT_EXCEPTION);
#endif
      exact = 0;
    }
    exponent_q += 2;
    CS.w[0] = CS0;
    mul_factor = 100;
    mul_factor2 = 10000;
  }
  if (exact) {
    CS0 = CS.w[0] * mul_factor;
    __sqr64_fast (CS1, CS0)
      if ((CS1.w[0] != A10.w[0]) || (CS1.w[1] != A10.w[1])) {
#ifdef BID_SET_STATUS_FLAGS
      __set_status_flags (pfpsf, BID_INEXACT_EXCEPTION);
#endif
      exact = 0;
    }
  }
}

if (!done) {
  // get number of digits in CX
  D = CX.w[1] - bid_power10_index_binexp_128[bin_expon_cx].w[1];
  if (D > 0
      || (!D && CX.w[0] >= bid_power10_index_binexp_128[bin_expon_cx].w[0]))
    digits++;

  // if exponent is odd, scale coefficient by 10
  scale = 31 - digits;
  exponent_q = exponent_x - scale;
  scale += (exponent_q & 1);	// exp. bias is even

  T128 = bid_power10_table_128[scale];
  __mul_128x128_low (C256, CX, T128);


  CS.w[0] = short_sqrt128 (C256);
}
   

exponent_q =
  ((exponent_q - DECIMAL_EXPONENT_BIAS_128) >> 1) +
  DECIMAL_EXPONENT_BIAS;
if ((exponent_q < 0) && (exponent_q + MAX_FORMAT_DIGITS >= 0)) {
  extra_digits = -exponent_q;
  exponent_q = 0;

  // get coeff*(2^M[extra_digits])/10^extra_digits
  __mul_64x64_to_128 (QH, CS.w[0], bid_reciprocals10_64[extra_digits]);

  // now get P/10^extra_digits: shift Q_high right by M[extra_digits]-128
  amount = bid_short_recip_scale[extra_digits];

  CS0 = QH.w[1] >> amount;

#ifdef BID_SET_STATUS_FLAGS
  if (exact) {
    if (CS.w[0] != CS0 * bid_power10_table_128[extra_digits].w[0])
      exact = 0;
  }
  if (!exact)
    __set_status_flags (pfpsf, BID_UNDERFLOW_EXCEPTION | BID_INEXACT_EXCEPTION);
#endif

  CS.w[0] = CS0;
  if (!mul_factor)
    mul_factor = 1;
  mul_factor *= bid_power10_table_128[extra_digits].w[0];
  __mul_64x64_to_128 (mul_factor2_long, mul_factor, mul_factor);
  if (mul_factor2_long.w[1])
    mul_factor2 = 0;
  else
    mul_factor2 = mul_factor2_long.w[1];
}
	   // 4*C256
C4.w[1] = (C256.w[1] << 2) | (C256.w[0] >> 62);
C4.w[0] = C256.w[0] << 2;

#ifndef IEEE_ROUND_NEAREST
#ifndef IEEE_ROUND_NEAREST_TIES_AWAY
if (!((rnd_mode) & 3)) {
#endif
#endif
  // compare to midpoints
  CSM.w[0] = (CS.w[0] + CS.w[0]) | 1;
  if (mul_factor)
    CSM.w[0] *= mul_factor;
  // CSM^2
  __mul_64x64_to_128 (M256, CSM.w[0], CSM.w[0]);

  if (C4.w[1] > M256.w[1] ||
      (C4.w[1] == M256.w[1] && C4.w[0] > M256.w[0])) {
    // round up
    CS.w[0]++;
  } else {
    C8.w[0] = CS.w[0] << 3;
    C8.w[1] = 0;
    if (mul_factor) {
      if (mul_factor2) {
	__mul_64x64_to_128 (C8, C8.w[0], mul_factor2);
      } else {
	__mul_64x128_low (C8, C8.w[0], mul_factor2_long);
      }
    }
    // M256 - 8*CSM
    __sub_borrow_out (M256.w[0], Carry, M256.w[0], C8.w[0]);
    M256.w[1] = M256.w[1] - C8.w[1] - Carry;

    // if CSM' > C256, round up
    if (M256.w[1] > C4.w[1] ||
	(M256.w[1] == C4.w[1] && M256.w[0] > C4.w[0])) {
      // round down
      if (CS.w[0])
	CS.w[0]--;
    }
  }
#ifndef IEEE_ROUND_NEAREST
#ifndef IEEE_ROUND_NEAREST_TIES_AWAY
} else {
  CS.w[0]++;
  CSM.w[0] = CS.w[0];
  C8.w[0] = CSM.w[0] << 1;
  if (mul_factor)
    CSM.w[0] *= mul_factor;
  __mul_64x64_to_128 (M256, CSM.w[0], CSM.w[0]);
  C8.w[1] = 0;
  if (mul_factor) {
    if (mul_factor2) {
      __mul_64x64_to_128 (C8, C8.w[0], mul_factor2);
    } else {
      __mul_64x128_low (C8, C8.w[0], mul_factor2_long);
    }
  }

  if (M256.w[1] > C256.w[1] ||
      (M256.w[1] == C256.w[1] && M256.w[0] > C256.w[0])) {
    __sub_borrow_out (M256.w[0], Carry, M256.w[0], C8.w[0]);
    M256.w[1] = M256.w[1] - Carry - C8.w[1];
    M256.w[0]++;
    if (!M256.w[0]) {
      M256.w[1]++;

    }

    if ((M256.w[1] > C256.w[1] ||
	 (M256.w[1] == C256.w[1] && M256.w[0] > C256.w[0]))
	&& (CS.w[0] > 1)) {

      CS.w[0]--;

      if (CS.w[0] > 1) {
	__sub_borrow_out (M256.w[0], Carry, M256.w[0], C8.w[0]);
	M256.w[1] = M256.w[1] - Carry - C8.w[1];
	M256.w[0]++;
	if (!M256.w[0]) {
	  M256.w[1]++;
	}

	if (M256.w[1] > C256.w[1] ||
	    (M256.w[1] == C256.w[1] && M256.w[0] > C256.w[0]))
	  CS.w[0]--;
      }
    }
  }

  else {

    CS.w[0]++;
  }
  // RU?
  if (((rnd_mode) != BID_ROUNDING_UP) || exact) {
    if (CS.w[0])
      CS.w[0]--;
  }

}
#endif
#endif

res = get_BID64 (0, exponent_q, CS.w[0], rnd_mode, pfpsf);
#ifdef UNCHANGED_BINARY_STATUS_FLAGS
// (void) fesetexceptflag (&binaryflags, BID_FE_ALL_FLAGS);
#endif
BID_RETURN_VAL (res);


}
