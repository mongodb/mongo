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

#define BID_128RES
#define BID_FUNCTION_SETS_BINARY_FLAGS
#include "bid_internal.h"
#include "bid_sqrt_macros.h"

BID128_FUNCTION_ARG1 (bid128_sqrt, x)

     BID_UINT256 M256, C256, C4, C8;
     BID_UINT128 CX, CX1, CX2, A10, S2, T128, TP128, CS, CSM, res;
     BID_UINT64 sign_x, Carry;
     BID_SINT64 D;
     int_float fx, f64;
     int exponent_x, bin_expon_cx;
     int digits, scale, exponent_q;

  BID_OPT_SAVE_BINARY_FLAGS()

  // unpack arguments, check for NaN or Infinity
if (!unpack_BID128_value (&sign_x, &exponent_x, &CX, x)) {
res.w[1] = CX.w[1];
res.w[0] = CX.w[0];
    // NaN ?
if ((x.w[1] & 0x7c00000000000000ull) == 0x7c00000000000000ull) {
#ifdef BID_SET_STATUS_FLAGS
  if ((x.w[1] & 0x7e00000000000000ull) == 0x7e00000000000000ull)	// sNaN
    __set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
#endif
  res.w[1] = CX.w[1] & QUIET_MASK64;
  BID_RETURN (res);
}
    // x is Infinity?
if ((x.w[1] & 0x7800000000000000ull) == 0x7800000000000000ull) {
  res.w[1] = CX.w[1];
  if (sign_x) {
    // -Inf, return NaN
    res.w[1] = 0x7c00000000000000ull;
#ifdef BID_SET_STATUS_FLAGS
    __set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
#endif
  }
  BID_RETURN (res);
}
    // x is 0 otherwise

res.w[1] =
  sign_x |
  ((((BID_UINT64) (exponent_x + DECIMAL_EXPONENT_BIAS_128)) >> 1) << 49);
res.w[0] = 0;
BID_RETURN (res);
}
if (sign_x) {
  res.w[1] = 0x7c00000000000000ull;
  res.w[0] = 0;
#ifdef BID_SET_STATUS_FLAGS
  __set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
#endif
  BID_RETURN (res);
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

CS.w[0] = short_sqrt128 (A10);
CS.w[1] = 0;
  // check for exact result
if (CS.w[0] * CS.w[0] == A10.w[0]) {
  __mul_64x64_to_128_fast (S2, CS.w[0], CS.w[0]);
  if (S2.w[1] == A10.w[1])	// && S2.w[0]==A10.w[0])
  {
    bid_get_BID128_very_fast (&res, 0,
			  (exponent_x +
			   DECIMAL_EXPONENT_BIAS_128) >> 1, CS);
#ifdef UNCHANGED_BINARY_STATUS_FLAGS
    // (void) fesetexceptflag (&binaryflags, BID_FE_ALL_FLAGS);
#endif
    BID_RETURN (res);
  }
}
  // get number of digits in CX
D = CX.w[1] - bid_power10_index_binexp_128[bin_expon_cx].w[1];
if (D > 0
    || (!D && CX.w[0] >= bid_power10_index_binexp_128[bin_expon_cx].w[0]))
  digits++;

  // if exponent is odd, scale coefficient by 10
scale = 67 - digits;
exponent_q = exponent_x - scale;
scale += (exponent_q & 1);	// exp. bias is even

if (scale > 38) {
  T128 = bid_power10_table_128[scale - 37];
  __mul_128x128_low (CX1, CX, T128);

  TP128 = bid_power10_table_128[37];
  __mul_128x128_to_256 (C256, CX1, TP128);
} else {
  T128 = bid_power10_table_128[scale];
  __mul_128x128_to_256 (C256, CX, T128);
}


  // 4*C256
C4.w[3] = (C256.w[3] << 2) | (C256.w[2] >> 62);
C4.w[2] = (C256.w[2] << 2) | (C256.w[1] >> 62);
C4.w[1] = (C256.w[1] << 2) | (C256.w[0] >> 62);
C4.w[0] = C256.w[0] << 2;

bid_long_sqrt128 (&CS, C256);
   //printf("C256=%016I64x %016I64x %016I64x %016I64x, CS=%016I64x %016I64x \n",C256.w[3],C256.w[2],C256.w[1],C256.w[0],CS.w[1],CS.w[0]);

#ifndef IEEE_ROUND_NEAREST
#ifndef IEEE_ROUND_NEAREST_TIES_AWAY
if (!((rnd_mode) & 3)) {
#endif
#endif
  // compare to midpoints
  CSM.w[1] = (CS.w[1] << 1) | (CS.w[0] >> 63);
  CSM.w[0] = (CS.w[0] + CS.w[0]) | 1;
  // CSM^2
  //__mul_128x128_to_256(M256, CSM, CSM);
  __sqr128_to_256 (M256, CSM);

  if (C4.w[3] > M256.w[3]
      || (C4.w[3] == M256.w[3]
	  && (C4.w[2] > M256.w[2]
	      || (C4.w[2] == M256.w[2]
		  && (C4.w[1] > M256.w[1]
		      || (C4.w[1] == M256.w[1]
			  && C4.w[0] > M256.w[0])))))) {
    // round up
    CS.w[0]++;
    if (!CS.w[0])
      CS.w[1]++;
  } else {
    C8.w[1] = (CS.w[1] << 3) | (CS.w[0] >> 61);
    C8.w[0] = CS.w[0] << 3;
    // M256 - 8*CSM
    __sub_borrow_out (M256.w[0], Carry, M256.w[0], C8.w[0]);
    __sub_borrow_in_out (M256.w[1], Carry, M256.w[1], C8.w[1], Carry);
    __sub_borrow_in_out (M256.w[2], Carry, M256.w[2], 0, Carry);
    M256.w[3] = M256.w[3] - Carry;

    // if CSM' > C256, round up
    if (M256.w[3] > C4.w[3]
	|| (M256.w[3] == C4.w[3]
	    && (M256.w[2] > C4.w[2]
		|| (M256.w[2] == C4.w[2]
		    && (M256.w[1] > C4.w[1]
			|| (M256.w[1] == C4.w[1]
			    && M256.w[0] > C4.w[0])))))) {
      // round down
      if (!CS.w[0])
	CS.w[1]--;
      CS.w[0]--;
    }
  }
#ifndef IEEE_ROUND_NEAREST
#ifndef IEEE_ROUND_NEAREST_TIES_AWAY
} else {
  __sqr128_to_256 (M256, CS);
  C8.w[1] = (CS.w[1] << 1) | (CS.w[0] >> 63);
  C8.w[0] = CS.w[0] << 1;
  if (M256.w[3] > C256.w[3]
      || (M256.w[3] == C256.w[3]
	  && (M256.w[2] > C256.w[2]
	      || (M256.w[2] == C256.w[2]
		  && (M256.w[1] > C256.w[1]
		      || (M256.w[1] == C256.w[1]
			  && M256.w[0] > C256.w[0])))))) {
    __sub_borrow_out (M256.w[0], Carry, M256.w[0], C8.w[0]);
    __sub_borrow_in_out (M256.w[1], Carry, M256.w[1], C8.w[1], Carry);
    __sub_borrow_in_out (M256.w[2], Carry, M256.w[2], 0, Carry);
    M256.w[3] = M256.w[3] - Carry;
    M256.w[0]++;
    if (!M256.w[0]) {
      M256.w[1]++;
      if (!M256.w[1]) {
	M256.w[2]++;
	if (!M256.w[2])
	  M256.w[3]++;
      }
    }

    if (!CS.w[0])
      CS.w[1]--;
    CS.w[0]--;

    if (M256.w[3] > C256.w[3]
	|| (M256.w[3] == C256.w[3]
	    && (M256.w[2] > C256.w[2]
		|| (M256.w[2] == C256.w[2]
		    && (M256.w[1] > C256.w[1]
			|| (M256.w[1] == C256.w[1]
			    && M256.w[0] > C256.w[0])))))) {

      if (!CS.w[0])
	CS.w[1]--;
      CS.w[0]--;
    }
  }

  else {
    __add_carry_out (M256.w[0], Carry, M256.w[0], C8.w[0]);
    __add_carry_in_out (M256.w[1], Carry, M256.w[1], C8.w[1], Carry);
    __add_carry_in_out (M256.w[2], Carry, M256.w[2], 0, Carry);
    M256.w[3] = M256.w[3] + Carry;
    M256.w[0]++;
    if (!M256.w[0]) {
      M256.w[1]++;
      if (!M256.w[1]) {
	M256.w[2]++;
	if (!M256.w[2])
	  M256.w[3]++;
      }
    }
    if (M256.w[3] < C256.w[3]
	|| (M256.w[3] == C256.w[3]
	    && (M256.w[2] < C256.w[2]
		|| (M256.w[2] == C256.w[2]
		    && (M256.w[1] < C256.w[1]
			|| (M256.w[1] == C256.w[1]
			    && M256.w[0] <= C256.w[0])))))) {

      CS.w[0]++;
      if (!CS.w[0])
	CS.w[1]++;
    }
  }
  // RU?
  if ((rnd_mode) == BID_ROUNDING_UP) {
    CS.w[0]++;
    if (!CS.w[0])
      CS.w[1]++;
  }

}
#endif
#endif

#ifdef BID_SET_STATUS_FLAGS
__set_status_flags (pfpsf, BID_INEXACT_EXCEPTION);
#endif
bid_get_BID128_fast (&res, 0,
		 (exponent_q + DECIMAL_EXPONENT_BIAS_128) >> 1, CS);
#ifdef UNCHANGED_BINARY_STATUS_FLAGS
// (void) fesetexceptflag (&binaryflags, BID_FE_ALL_FLAGS);
#endif
BID_RETURN (res);
}



BID128_FUNCTION_ARGTYPE1 (bid128d_sqrt, BID_UINT64, x)

     BID_UINT256 M256, C256, C4, C8;
     BID_UINT128 CX, CX1, CX2, A10, S2, T128, TP128, CS, CSM, res;
     BID_UINT64 sign_x, Carry;
     BID_SINT64 D;
     int_float fx, f64;
     int exponent_x, bin_expon_cx;
     int digits, scale, exponent_q;

  BID_OPT_SAVE_BINARY_FLAGS()

	// unpack arguments, check for NaN or Infinity
   // unpack arguments, check for NaN or Infinity
CX.w[1] = 0;
if (!unpack_BID64 (&sign_x, &exponent_x, &CX.w[0], x)) {
res.w[1] = CX.w[0];
res.w[0] = 0;
	   // NaN ?
if ((x & 0x7c00000000000000ull) == 0x7c00000000000000ull) {
#ifdef BID_SET_STATUS_FLAGS
  if ((x & SNAN_MASK64) == SNAN_MASK64)	// sNaN
    __set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
#endif
  res.w[0] = (CX.w[0] & 0x0003ffffffffffffull);
  __mul_64x64_to_128 (res, res.w[0], bid_power10_table_128[18].w[0]);
  res.w[1] |= ((CX.w[0]) & 0xfc00000000000000ull);
  BID_RETURN (res);
}
	   // x is Infinity?
if ((x & 0x7800000000000000ull) == 0x7800000000000000ull) {
  if (sign_x) {
    // -Inf, return NaN
    res.w[1] = 0x7c00000000000000ull;
#ifdef BID_SET_STATUS_FLAGS
    __set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
#endif
  }
  BID_RETURN (res);
}
	   // x is 0 otherwise

exponent_x =
  exponent_x - DECIMAL_EXPONENT_BIAS + DECIMAL_EXPONENT_BIAS_128;
res.w[1] =
  sign_x | ((((BID_UINT64) (exponent_x + DECIMAL_EXPONENT_BIAS_128)) >> 1)
	    << 49);
res.w[0] = 0;
BID_RETURN (res);
}
if (sign_x) {
  res.w[1] = 0x7c00000000000000ull;
  res.w[0] = 0;
#ifdef BID_SET_STATUS_FLAGS
  __set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
#endif
  BID_RETURN (res);
}
#ifdef UNCHANGED_BINARY_STATUS_FLAGS
// (void) fegetexceptflag (&binaryflags, BID_FE_ALL_FLAGS);
#endif
exponent_x =
  exponent_x - DECIMAL_EXPONENT_BIAS + DECIMAL_EXPONENT_BIAS_128;

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

CS.w[0] = short_sqrt128 (A10);
CS.w[1] = 0;
	   // check for exact result
if (CS.w[0] * CS.w[0] == A10.w[0]) {
  __mul_64x64_to_128_fast (S2, CS.w[0], CS.w[0]);
  if (S2.w[1] == A10.w[1]) {
    bid_get_BID128_very_fast (&res, 0,
			  (exponent_x + DECIMAL_EXPONENT_BIAS_128) >> 1,
			  CS);
#ifdef UNCHANGED_BINARY_STATUS_FLAGS
    // (void) fesetexceptflag (&binaryflags, BID_FE_ALL_FLAGS);
#endif
    BID_RETURN (res);
  }
}
	   // get number of digits in CX
D = CX.w[1] - bid_power10_index_binexp_128[bin_expon_cx].w[1];
if (D > 0
    || (!D && CX.w[0] >= bid_power10_index_binexp_128[bin_expon_cx].w[0]))
  digits++;

		// if exponent is odd, scale coefficient by 10
scale = 67 - digits;
exponent_q = exponent_x - scale;
scale += (exponent_q & 1);	// exp. bias is even

if (scale > 38) {
  T128 = bid_power10_table_128[scale - 37];
  __mul_128x128_low (CX1, CX, T128);

  TP128 = bid_power10_table_128[37];
  __mul_128x128_to_256 (C256, CX1, TP128);
} else {
  T128 = bid_power10_table_128[scale];
  __mul_128x128_to_256 (C256, CX, T128);
}


	   // 4*C256
C4.w[3] = (C256.w[3] << 2) | (C256.w[2] >> 62);
C4.w[2] = (C256.w[2] << 2) | (C256.w[1] >> 62);
C4.w[1] = (C256.w[1] << 2) | (C256.w[0] >> 62);
C4.w[0] = C256.w[0] << 2;

bid_long_sqrt128 (&CS, C256);

#ifndef IEEE_ROUND_NEAREST
#ifndef IEEE_ROUND_NEAREST_TIES_AWAY
if (!((rnd_mode) & 3)) {
#endif
#endif
  // compare to midpoints
  CSM.w[1] = (CS.w[1] << 1) | (CS.w[0] >> 63);
  CSM.w[0] = (CS.w[0] + CS.w[0]) | 1;
  // CSM^2
  //__mul_128x128_to_256(M256, CSM, CSM);
  __sqr128_to_256 (M256, CSM);

  if (C4.w[3] > M256.w[3]
      || (C4.w[3] == M256.w[3]
	  && (C4.w[2] > M256.w[2]
	      || (C4.w[2] == M256.w[2]
		  && (C4.w[1] > M256.w[1]
		      || (C4.w[1] == M256.w[1]
			  && C4.w[0] > M256.w[0])))))) {
    // round up
    CS.w[0]++;
    if (!CS.w[0])
      CS.w[1]++;
  } else {
    C8.w[1] = (CS.w[1] << 3) | (CS.w[0] >> 61);
    C8.w[0] = CS.w[0] << 3;
    // M256 - 8*CSM
    __sub_borrow_out (M256.w[0], Carry, M256.w[0], C8.w[0]);
    __sub_borrow_in_out (M256.w[1], Carry, M256.w[1], C8.w[1], Carry);
    __sub_borrow_in_out (M256.w[2], Carry, M256.w[2], 0, Carry);
    M256.w[3] = M256.w[3] - Carry;

    // if CSM' > C256, round up
    if (M256.w[3] > C4.w[3]
	|| (M256.w[3] == C4.w[3]
	    && (M256.w[2] > C4.w[2]
		|| (M256.w[2] == C4.w[2]
		    && (M256.w[1] > C4.w[1]
			|| (M256.w[1] == C4.w[1]
			    && M256.w[0] > C4.w[0])))))) {
      // round down
      if (!CS.w[0])
	CS.w[1]--;
      CS.w[0]--;
    }
  }
#ifndef IEEE_ROUND_NEAREST
#ifndef IEEE_ROUND_NEAREST_TIES_AWAY
} else {
  __sqr128_to_256 (M256, CS);
  C8.w[1] = (CS.w[1] << 1) | (CS.w[0] >> 63);
  C8.w[0] = CS.w[0] << 1;
  if (M256.w[3] > C256.w[3]
      || (M256.w[3] == C256.w[3]
	  && (M256.w[2] > C256.w[2]
	      || (M256.w[2] == C256.w[2]
		  && (M256.w[1] > C256.w[1]
		      || (M256.w[1] == C256.w[1]
			  && M256.w[0] > C256.w[0])))))) {
    __sub_borrow_out (M256.w[0], Carry, M256.w[0], C8.w[0]);
    __sub_borrow_in_out (M256.w[1], Carry, M256.w[1], C8.w[1], Carry);
    __sub_borrow_in_out (M256.w[2], Carry, M256.w[2], 0, Carry);
    M256.w[3] = M256.w[3] - Carry;
    M256.w[0]++;
    if (!M256.w[0]) {
      M256.w[1]++;
      if (!M256.w[1]) {
	M256.w[2]++;
	if (!M256.w[2])
	  M256.w[3]++;
      }
    }

    if (!CS.w[0])
      CS.w[1]--;
    CS.w[0]--;

    if (M256.w[3] > C256.w[3]
	|| (M256.w[3] == C256.w[3]
	    && (M256.w[2] > C256.w[2]
		|| (M256.w[2] == C256.w[2]
		    && (M256.w[1] > C256.w[1]
			|| (M256.w[1] == C256.w[1]
			    && M256.w[0] > C256.w[0])))))) {

      if (!CS.w[0])
	CS.w[1]--;
      CS.w[0]--;
    }
  }

  else {
    __add_carry_out (M256.w[0], Carry, M256.w[0], C8.w[0]);
    __add_carry_in_out (M256.w[1], Carry, M256.w[1], C8.w[1], Carry);
    __add_carry_in_out (M256.w[2], Carry, M256.w[2], 0, Carry);
    M256.w[3] = M256.w[3] + Carry;
    M256.w[0]++;
    if (!M256.w[0]) {
      M256.w[1]++;
      if (!M256.w[1]) {
	M256.w[2]++;
	if (!M256.w[2])
	  M256.w[3]++;
      }
    }
    if (M256.w[3] < C256.w[3]
	|| (M256.w[3] == C256.w[3]
	    && (M256.w[2] < C256.w[2]
		|| (M256.w[2] == C256.w[2]
		    && (M256.w[1] < C256.w[1]
			|| (M256.w[1] == C256.w[1]
			    && M256.w[0] <= C256.w[0])))))) {

      CS.w[0]++;
      if (!CS.w[0])
	CS.w[1]++;
    }
  }
  // RU?
  if ((rnd_mode) == BID_ROUNDING_UP) {
    CS.w[0]++;
    if (!CS.w[0])
      CS.w[1]++;
  }

}
#endif
#endif

#ifdef BID_SET_STATUS_FLAGS
__set_status_flags (pfpsf, BID_INEXACT_EXCEPTION);
#endif
bid_get_BID128_fast (&res, 0, (exponent_q + DECIMAL_EXPONENT_BIAS_128) >> 1,
		 CS);
#ifdef UNCHANGED_BINARY_STATUS_FLAGS
// (void) fesetexceptflag (&binaryflags, BID_FE_ALL_FLAGS);
#endif
BID_RETURN (res);


}
