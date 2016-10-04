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
 *    BID64 divide
 *****************************************************************************
 *
 *  Algorithm description:
 *
 *  if(coefficient_x<coefficient_y)
 *    p = number_digits(coefficient_y) - number_digits(coefficient_x)
 *    A = coefficient_x*10^p
 *    B = coefficient_y
 *    CA= A*10^(15+j), j=0 for A>=B, 1 otherwise
 *    Q = 0
 *  else
 *    get Q=(int)(coefficient_x/coefficient_y) 
 *        (based on double precision divide)
 *    check for exact divide case
 *    Let R = coefficient_x - Q*coefficient_y
 *    Let m=16-number_digits(Q)
 *    CA=R*10^m, Q=Q*10^m
 *    B = coefficient_y
 *  endif
 *    if (CA<2^64)
 *      Q += CA/B  (64-bit unsigned divide)
 *    else 
 *      get final Q using double precision divide, followed by 3 integer 
 *          iterations
 *    if exact result, eliminate trailing zeros
 *    check for underflow
 *    round coefficient to nearest
 *
 ****************************************************************************/

#define BID_FUNCTION_SETS_BINARY_FLAGS
#define BID_128RES
#include "bid_internal.h"
#include "bid_div_macros.h"

BID_EXTERN_C const BID_UINT32 bid_convert_table[5][128][2];
BID_EXTERN_C const BID_SINT8 bid_factors[][2];
BID_EXTERN_C const BID_UINT8 bid_packed_10000_zeros[];


BID_TYPE0_FUNCTION_ARGTYPE1_ARGTYPE2(BID_UINT64, bid64_div, BID_UINT64, x, BID_UINT64, y)

  BID_UINT128 CA, CT;
  BID_UINT64 sign_x, sign_y, coefficient_x, coefficient_y, A, B, QX, PD;
  BID_UINT64 A2, Q, Q2, B2, B4, B5, R, T, DU, res;
  BID_UINT64 valid_x, valid_y;
  BID_SINT64 D;
  int_double t_scale, tempq, temp_b;
  int_float tempx, tempy;
  double da, db, dq, da_h, da_l;
  int exponent_x, exponent_y, bin_expon_cx;
  int diff_expon, ed1, ed2, bin_index;
  int rmode, amount;
  int nzeros, i, j, k, d5;
  BID_UINT32 QX32, tdigit[3], digit, digit_h, digit_low;

  BID_OPT_SAVE_BINARY_FLAGS()

  valid_x = unpack_BID64 (&sign_x, &exponent_x, &coefficient_x, x);
  valid_y = unpack_BID64 (&sign_y, &exponent_y, &coefficient_y, y);

  // unpack arguments, check for NaN or Infinity
  if (!valid_x) {
    // x is Inf. or NaN
#ifdef BID_SET_STATUS_FLAGS
    if ((y & SNAN_MASK64) == SNAN_MASK64)	// y is sNaN
      __set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
#endif

    // test if x is NaN
    if ((x & NAN_MASK64) == NAN_MASK64) {
#ifdef BID_SET_STATUS_FLAGS
      if ((x & SNAN_MASK64) == SNAN_MASK64)	// sNaN
	__set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
#endif
      BID_RETURN_VAL (coefficient_x & QUIET_MASK64);
    }
    // x is Infinity?
    if ((x & INFINITY_MASK64) == INFINITY_MASK64) {
      // check if y is Inf or NaN
      if ((y & INFINITY_MASK64) == INFINITY_MASK64) {
	// y==Inf, return NaN 
	if ((y & NAN_MASK64) == INFINITY_MASK64) {	// Inf/Inf
#ifdef BID_SET_STATUS_FLAGS
	  __set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
#endif
	  BID_RETURN_VAL (NAN_MASK64);
	}
      } else {
	// otherwise return +/-Inf
	BID_RETURN_VAL (((x ^ y) & 0x8000000000000000ull) |
		    INFINITY_MASK64);
      }
    }
    // x==0
    if (((y & INFINITY_MASK64) != INFINITY_MASK64)
	&& !(coefficient_y)) {
      // y==0 , return NaN
#ifdef BID_SET_STATUS_FLAGS
      __set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
#endif
      BID_RETURN_VAL (NAN_MASK64);
    }
    if (((y & INFINITY_MASK64) != INFINITY_MASK64)) {
      if ((y & SPECIAL_ENCODING_MASK64) == SPECIAL_ENCODING_MASK64)
	exponent_y = ((BID_UINT32) (y >> 51)) & 0x3ff;
      else
	exponent_y = ((BID_UINT32) (y >> 53)) & 0x3ff;
      sign_y = y & 0x8000000000000000ull;

      exponent_x = exponent_x - exponent_y + DECIMAL_EXPONENT_BIAS;
      if (exponent_x > DECIMAL_MAX_EXPON_64)
	exponent_x = DECIMAL_MAX_EXPON_64;
      else if (exponent_x < 0)
	exponent_x = 0;
      BID_RETURN_VAL ((sign_x ^ sign_y) | (((BID_UINT64) exponent_x) << 53));
    }

  }
  if (!valid_y) {
    // y is Inf. or NaN

    // test if y is NaN
    if ((y & NAN_MASK64) == NAN_MASK64) {
#ifdef BID_SET_STATUS_FLAGS
      if ((y & SNAN_MASK64) == SNAN_MASK64)	// sNaN
	__set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
#endif
      BID_RETURN_VAL (coefficient_y & QUIET_MASK64);
    }
    // y is Infinity?
    if ((y & INFINITY_MASK64) == INFINITY_MASK64) {
      // return +/-0
      BID_RETURN_VAL (((x ^ y) & 0x8000000000000000ull));
    }
    // y is 0
#ifdef BID_SET_STATUS_FLAGS
    __set_status_flags (pfpsf, BID_ZERO_DIVIDE_EXCEPTION);
#endif
    BID_RETURN_VAL ((sign_x ^ sign_y) | INFINITY_MASK64);
  }
#ifdef UNCHANGED_BINARY_STATUS_FLAGS
  // (void) fegetexceptflag (&binaryflags, BID_FE_ALL_FLAGS);
#endif
  diff_expon = exponent_x - exponent_y + DECIMAL_EXPONENT_BIAS;

  if (coefficient_x < coefficient_y) {
    // get number of decimal digits for c_x, c_y

    //--- get number of bits in the coefficients of x and y ---
    tempx.d = (float) coefficient_x;
    tempy.d = (float) coefficient_y;
    bin_index = (tempy.i - tempx.i) >> 23;

    A = coefficient_x * bid_power10_index_binexp[bin_index];
    B = coefficient_y;

    temp_b.d = (double) B;

    // compare A, B
    DU = (A - B) >> 63;
    ed1 = 15 + (int) DU;
    ed2 = bid_estimate_decimal_digits[bin_index] + ed1;
    T = bid_power10_table_128[ed1].w[0];
    __mul_64x64_to_128 (CA, A, T);

    Q = 0;
    diff_expon = diff_expon - ed2;

    // adjust double precision db, to ensure that later A/B - (int)(da/db) > -1
    if (coefficient_y < 0x0020000000000000ull) {
      temp_b.i += 1;
      db = temp_b.d;
    } else
      db = (double) (B + 2 + (B & 1));

  } else {
    // get c_x/c_y

    //  set last bit before conversion to DP
    A2 = coefficient_x | 1;
    da = (double) A2;

    db = (double) coefficient_y;

    tempq.d = da / db;
    Q = (BID_UINT64) tempq.d;

    R = coefficient_x - coefficient_y * Q;

    // will use to get number of dec. digits of Q
    bin_expon_cx = (tempq.i >> 52) - 0x3ff;

    // R<0 ?
    D = ((BID_SINT64) R) >> 63;
    Q += D;
    R += (coefficient_y & D);

    // exact result ?
    if (((BID_SINT64) R) <= 0) {
      // can have R==-1 for coeff_y==1
      res =
	get_BID64 (sign_x ^ sign_y, diff_expon, (Q + R), rnd_mode,
		   pfpsf);
#ifdef UNCHANGED_BINARY_STATUS_FLAGS
      // (void) fesetexceptflag (&binaryflags, BID_FE_ALL_FLAGS);
#endif
      BID_RETURN_VAL (res);
    }
    // get decimal digits of Q
    DU = bid_power10_index_binexp[bin_expon_cx] - Q - 1;
    DU >>= 63;

    ed2 = 16 - bid_estimate_decimal_digits[bin_expon_cx] - (int) DU;

    T = bid_power10_table_128[ed2].w[0];
    __mul_64x64_to_128 (CA, R, T);
    B = coefficient_y;

    Q *= bid_power10_table_128[ed2].w[0];
    diff_expon -= ed2;

  }

  if (!CA.w[1]) {
    Q2 = CA.w[0] / B;
    B2 = B + B;
    B4 = B2 + B2;
    R = CA.w[0] - Q2 * B;
    Q += Q2;
  } else {

    // 2^64
    t_scale.i = 0x43f0000000000000ull;
    // convert CA to DP
    da_h = CA.w[1];
    da_l = CA.w[0];
    da = da_h * t_scale.d + da_l;

    // quotient
    dq = da / db;
    Q2 = (BID_UINT64) dq;

    // get w[0] remainder
    R = CA.w[0] - Q2 * B;

    // R<0 ?
    D = ((BID_SINT64) R) >> 63;
    Q2 += D;
    R += (B & D);

    // now R<6*B

    // quick divide

    // 4*B
    B2 = B + B;
    B4 = B2 + B2;

    R = R - B4;
    // R<0 ?
    D = ((BID_SINT64) R) >> 63;
    // restore R if negative
    R += (B4 & D);
    Q2 += ((~D) & 4);

    R = R - B2;
    // R<0 ?
    D = ((BID_SINT64) R) >> 63;
    // restore R if negative
    R += (B2 & D);
    Q2 += ((~D) & 2);

    R = R - B;
    // R<0 ?
    D = ((BID_SINT64) R) >> 63;
    // restore R if negative
    R += (B & D);
    Q2 += ((~D) & 1);

    Q += Q2;
  }

#ifdef BID_SET_STATUS_FLAGS
  if (R) {
    // set status flags
    __set_status_flags (pfpsf, BID_INEXACT_EXCEPTION);
  }
#ifndef LEAVE_TRAILING_ZEROS
  else
#endif
#else
#ifndef LEAVE_TRAILING_ZEROS
  if (!R)
#endif
#endif
#ifndef LEAVE_TRAILING_ZEROS
  {
    // eliminate trailing zeros

    // check whether CX, CY are short
    if ((coefficient_x <= 1024) && (coefficient_y <= 1024)) {
      i = (int) coefficient_y - 1;
      j = (int) coefficient_x - 1;
      // difference in powers of 2 bid_factors for Y and X
      nzeros = ed2 - bid_factors[i][0] + bid_factors[j][0];
      // difference in powers of 5 bid_factors
      d5 = ed2 - bid_factors[i][1] + bid_factors[j][1];
      if (d5 < nzeros)
	nzeros = d5;

      __mul_64x64_to_128 (CT, Q, bid_reciprocals10_64[nzeros]);

      // now get P/10^extra_digits: shift C64 right by M[extra_digits]-128
      amount = bid_short_recip_scale[nzeros];
      Q = CT.w[1] >> amount;

      diff_expon += nzeros;
    } else {
      tdigit[0] = Q & 0x3ffffff;
      tdigit[1] = 0;
      QX = Q >> 26;
      QX32 = QX;
      nzeros = 0;

      for (j = 0; QX32; j++, QX32 >>= 7) {
	k = (QX32 & 127);
	tdigit[0] += bid_convert_table[j][k][0];
	tdigit[1] += bid_convert_table[j][k][1];
	if (tdigit[0] >= 100000000) {
	  tdigit[0] -= 100000000;
	  tdigit[1]++;
	}
      }

      digit = tdigit[0];
      if (!digit && !tdigit[1])
	nzeros += 16;
      else {
	if (!digit) {
	  nzeros += 8;
	  digit = tdigit[1];
	}
	// decompose digit
	PD = (BID_UINT64) digit *0x068DB8BBull;
	digit_h = (BID_UINT32) (PD >> 40);
	digit_low = digit - digit_h * 10000;

	if (!digit_low)
	  nzeros += 4;
	else
	  digit_h = digit_low;

	if (!(digit_h & 1))
	  nzeros +=
	    3 & (BID_UINT32) (bid_packed_10000_zeros[digit_h >> 3] >>
			  (digit_h & 7));
      }

      if (nzeros) {
	__mul_64x64_to_128 (CT, Q, bid_reciprocals10_64[nzeros]);

	// now get P/10^extra_digits: shift C64 right by M[extra_digits]-128
	amount = bid_short_recip_scale[nzeros];
	Q = CT.w[1] >> amount;
      }
      diff_expon += nzeros;

    }
    if (diff_expon >= 0) {
      res =
	fast_get_BID64_check_OF (sign_x ^ sign_y, diff_expon, Q,
				 rnd_mode, pfpsf);
#ifdef UNCHANGED_BINARY_STATUS_FLAGS
      // (void) fesetexceptflag (&binaryflags, BID_FE_ALL_FLAGS);
#endif
      BID_RETURN_VAL (res);
    }
  }
#endif

  if (diff_expon >= 0) {
#ifdef IEEE_ROUND_NEAREST
    // round to nearest code
    // R*10
    R += R;
    R = (R << 2) + R;
    B5 = B4 + B;

    // compare 10*R to 5*B
    R = B5 - R;
    // correction for (R==0 && (Q&1))
    R -= (Q & 1);
    // R<0 ?
    D = ((BID_UINT64) R) >> 63;
    Q += D;
#else
#ifdef IEEE_ROUND_NEAREST_TIES_AWAY
    // round to nearest code
    // R*10
    R += R;
    R = (R << 2) + R;
    B5 = B4 + B;

    // compare 10*R to 5*B
    R = B5 - R;
    // correction for (R==0 && (Q&1))
    R -= (Q & 1);
    // R<0 ?
    D = ((BID_UINT64) R) >> 63;
    Q += D;
#else
    rmode = rnd_mode;
    if (sign_x ^ sign_y && (unsigned) (rmode - 1) < 2)
      rmode = 3 - rmode;
    switch (rmode) {
    case 0:	// round to nearest code
    case BID_ROUNDING_TIES_AWAY:
      // R*10
      R += R;
      R = (R << 2) + R;
      B5 = B4 + B;
      // compare 10*R to 5*B
      R = B5 - R;
      // correction for (R==0 && (Q&1))
      R -= ((Q | (rmode >> 2)) & 1);
      // R<0 ?
      D = ((BID_UINT64) R) >> 63;
      Q += D;
      break;
    case BID_ROUNDING_DOWN:
    case BID_ROUNDING_TO_ZERO:
      break;
    default:	// rounding up
      Q++;
      break;
    }
#endif
#endif

    res =
      fast_get_BID64_check_OF (sign_x ^ sign_y, diff_expon, Q, rnd_mode,
			       pfpsf);
#ifdef UNCHANGED_BINARY_STATUS_FLAGS
    // (void) fesetexceptflag (&binaryflags, BID_FE_ALL_FLAGS);
#endif
    BID_RETURN_VAL (res);
  } else {
    // UF occurs

#ifdef BID_SET_STATUS_FLAGS
    if ((diff_expon + 16 < 0)) {
      // set status flags
      __set_status_flags (pfpsf, BID_INEXACT_EXCEPTION);
    }
#endif
    rmode = rnd_mode;
    res =
      get_BID64_UF (sign_x ^ sign_y, diff_expon, Q, R, rmode, pfpsf);
#ifdef UNCHANGED_BINARY_STATUS_FLAGS
    // (void) fesetexceptflag (&binaryflags, BID_FE_ALL_FLAGS);
#endif
    BID_RETURN_VAL (res);

  }
}



BID_TYPE0_FUNCTION_ARGTYPE1_ARG128 (BID_UINT64, bid64dq_div, BID_UINT64, x, y)
     BID_UINT256 CA4 =
       { {0x0ull, 0x0ull, 0x0ull, 0x0ull} }, CA4r;
BID_UINT128 CX, CY, T128, CQ, CR, CA, TP128, Qh, Ql, Tmp;
BID_UINT64 sign_x, sign_y, carry64, D, Q_low, QX, valid_y, PD, res;
int_float fx, fy, f64;
BID_UINT32 QX32, tdigit[3], digit, digit_h, digit_low;
int exponent_x, exponent_y, bin_index, bin_expon, diff_expon, ed2,
  digits_q, amount;
int nzeros, i, j, k, d5, done = 0;
unsigned rmode;
  BID_OPT_SAVE_BINARY_FLAGS()

valid_y = unpack_BID128_value (&sign_y, &exponent_y, &CY, y);

	// unpack arguments, check for NaN or Infinity
CX.w[1] = 0;
if (!unpack_BID64 (&sign_x, &exponent_x, &CX.w[0], (x))) {
#ifdef BID_SET_STATUS_FLAGS
    if (((y.w[1] & SNAN_MASK64) == SNAN_MASK64) ||	// y is sNaN
		((x & SNAN_MASK64) == SNAN_MASK64))
      __set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
#endif
  // test if x is NaN
  if (((x) & 0x7c00000000000000ull) == 0x7c00000000000000ull) {
    res = CX.w[0];
    BID_RETURN_VAL (res & QUIET_MASK64);
  }
  // x is Infinity?
  if (((x) & 0x7800000000000000ull) == 0x7800000000000000ull) {
    // check if y is Inf.
    if (((y.w[1] & 0x7c00000000000000ull) == 0x7800000000000000ull))
      // return NaN 
    {
#ifdef BID_SET_STATUS_FLAGS
      __set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
#endif
      res = 0x7c00000000000000ull;
      BID_RETURN_VAL (res);
    }
	if (((y.w[1] & 0x7c00000000000000ull) != 0x7c00000000000000ull)) {
    // otherwise return +/-Inf
    res =
      (((x) ^ y.w[1]) & 0x8000000000000000ull) | 0x7800000000000000ull;
    BID_RETURN_VAL (res);
	}
  }
  // x is 0
  if ((y.w[1] & INFINITY_MASK64) != INFINITY_MASK64) {
    if ((!CY.w[0]) && !(CY.w[1] & 0x0001ffffffffffffull)) {
#ifdef BID_SET_STATUS_FLAGS
      __set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
#endif
      // x=y=0, return NaN
      res = 0x7c00000000000000ull;
      BID_RETURN_VAL (res);
    }
    // return 0
    res = ((x) ^ y.w[1]) & 0x8000000000000000ull;
    exponent_x = exponent_x - exponent_y + DECIMAL_EXPONENT_BIAS_128;
    if (exponent_x > DECIMAL_MAX_EXPON_64)
      exponent_x = DECIMAL_MAX_EXPON_64;
    else if (exponent_x < 0)
      exponent_x = 0;
    res |= (((BID_UINT64) exponent_x) << 53);
    BID_RETURN_VAL (res);
  }
}
exponent_x += (DECIMAL_EXPONENT_BIAS_128 - DECIMAL_EXPONENT_BIAS);
if (!valid_y) {
  // y is Inf. or NaN

  // test if y is NaN
  if ((y.w[1] & 0x7c00000000000000ull) == 0x7c00000000000000ull) {
#ifdef BID_SET_STATUS_FLAGS
    if ((y.w[1] & 0x7e00000000000000ull) == 0x7e00000000000000ull)	// sNaN
      __set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
#endif
    Tmp.w[1] = (CY.w[1] & 0x00003fffffffffffull);
    Tmp.w[0] = CY.w[0];
    TP128 = bid_reciprocals10_128[18];
    __mul_128x128_full (Qh, Ql, Tmp, TP128);
    amount = bid_recip_scale[18];
    __shr_128 (Tmp, Qh, amount);
    res = (CY.w[1] & 0xfc00000000000000ull) | Tmp.w[0];
    BID_RETURN_VAL (res);
  }
  // y is Infinity?
  if ((y.w[1] & 0x7800000000000000ull) == 0x7800000000000000ull) {
    // return +/-0
    res = sign_x ^ sign_y;
    BID_RETURN_VAL (res);
  }
  // y is 0, return +/-Inf
  res =
    (((x) ^ y.w[1]) & 0x8000000000000000ull) | 0x7800000000000000ull;
#ifdef BID_SET_STATUS_FLAGS
  __set_status_flags (pfpsf, BID_ZERO_DIVIDE_EXCEPTION);
#endif
  BID_RETURN_VAL (res);
}
#ifdef UNCHANGED_BINARY_STATUS_FLAGS
// (void) fegetexceptflag (&binaryflags, BID_FE_ALL_FLAGS);
#endif
diff_expon = exponent_x - exponent_y + DECIMAL_EXPONENT_BIAS;

if (__unsigned_compare_gt_128 (CY, CX)) {
  // CX < CY

  // 2^64
  f64.i = 0x5f800000;

  // fx ~ CX,   fy ~ CY
  fx.d = (float) CX.w[1] * f64.d + (float) CX.w[0];
  fy.d = (float) CY.w[1] * f64.d + (float) CY.w[0];
  // expon_cy - expon_cx
  bin_index = (fy.i - fx.i) >> 23;

// Code redundant as agreed Jul 25 2008. To be removed after verification
//  if (CX.w[1]) {
//    T = bid_power10_index_binexp_128[bin_index].w[0];
//    __mul_64x128_short (CA, T, CX);
//  } else {
    T128 = bid_power10_index_binexp_128[bin_index];
    __mul_64x128_short (CA, CX.w[0], T128);
//  }

  ed2 = 15;
  if (__unsigned_compare_gt_128 (CY, CA))
    ed2++;

  T128 = bid_power10_table_128[ed2];
  __mul_128x128_to_256 (CA4, CA, T128);

  ed2 += bid_estimate_decimal_digits[bin_index];
  CQ.w[0] = CQ.w[1] = 0;
  diff_expon = diff_expon - ed2;

} else {
  // get CQ = CX/CY
  bid___div_128_by_128 (&CQ, &CR, CX, CY);

  // get number of decimal digits in CQ
  // 2^64
  f64.i = 0x5f800000;
  fx.d = (float) CQ.w[1] * f64.d + (float) CQ.w[0];
  // binary expon. of CQ
  bin_expon = (fx.i - 0x3f800000) >> 23;

  digits_q = bid_estimate_decimal_digits[bin_expon];
  TP128.w[0] = bid_power10_index_binexp_128[bin_expon].w[0];
  TP128.w[1] = bid_power10_index_binexp_128[bin_expon].w[1];
  if (__unsigned_compare_ge_128 (CQ, TP128))
    digits_q++;

// Code redundant as agreed Jul 25 2008. To be removed after verification
//  if (digits_q <= 16) {
    if (!CR.w[1] && !CR.w[0]) {
      res = get_BID64 (sign_x ^ sign_y, diff_expon,
		       CQ.w[0], rnd_mode, pfpsf);
#ifdef UNCHANGED_BINARY_STATUS_FLAGS
      // (void) fesetexceptflag (&binaryflags, BID_FE_ALL_FLAGS);
#endif
      BID_RETURN_VAL (res);
    }

    ed2 = 16 - digits_q;
    T128.w[0] = bid_power10_table_128[ed2].w[0];
    __mul_64x128_to_192 (CA4, (T128.w[0]), CR);
    diff_expon = diff_expon - ed2;
    CQ.w[0] *= T128.w[0];
}
if (!done) {
  bid___div_256_by_128 (&CQ, &CA4, CY);
}

 

#ifdef BID_SET_STATUS_FLAGS
  if (CA4.w[0] || CA4.w[1]) {
    // set status flags
    __set_status_flags (pfpsf, BID_INEXACT_EXCEPTION);
  }
#ifndef LEAVE_TRAILING_ZEROS
  else
#endif
#else
#ifndef LEAVE_TRAILING_ZEROS
  if (!CA4.w[0] && !CA4.w[1])
#endif
#endif
#ifndef LEAVE_TRAILING_ZEROS
    // check whether result is exact
  {
    // check whether CX, CY are short
    if (!CX.w[1] && !CY.w[1] && (CX.w[0] <= 1024) && (CY.w[0] <= 1024)) {
      i = (int) CY.w[0] - 1;
      j = (int) CX.w[0] - 1;
      // difference in powers of 2 bid_factors for Y and X
      nzeros = ed2 - bid_factors[i][0] + bid_factors[j][0];
      // difference in powers of 5 bid_factors
      d5 = ed2 - bid_factors[i][1] + bid_factors[j][1];
      if (d5 < nzeros)
	nzeros = d5;
      // get P*(2^M[extra_digits])/10^extra_digits
      __mul_128x128_full (Qh, Ql, CQ, bid_reciprocals10_128[nzeros]);

      // now get P/10^extra_digits: shift Q_high right by M[extra_digits]-128
      amount = bid_recip_scale[nzeros];
      __shr_128_long (CQ, Qh, amount);

      diff_expon += nzeros;
    } else {
      // decompose Q as Qh*10^17 + Ql
      Q_low = CQ.w[0];

      {
	tdigit[0] = Q_low & 0x3ffffff;
	tdigit[1] = 0;
	QX = Q_low >> 26;
	QX32 = QX;
	nzeros = 0;

	for (j = 0; QX32; j++, QX32 >>= 7) {
	  k = (QX32 & 127);
	  tdigit[0] += bid_convert_table[j][k][0];
	  tdigit[1] += bid_convert_table[j][k][1];
	  if (tdigit[0] >= 100000000) {
	    tdigit[0] -= 100000000;
	    tdigit[1]++;
	  }
	}

	digit = tdigit[0];
	if (!digit && !tdigit[1])
	  nzeros += 16;
	else {
	  if (!digit) {
	    nzeros += 8;
	    digit = tdigit[1];
	  }
	  // decompose digit
	  PD = (BID_UINT64) digit *0x068DB8BBull;
	  digit_h = (BID_UINT32) (PD >> 40);
	  digit_low = digit - digit_h * 10000;

	  if (!digit_low)
	    nzeros += 4;
	  else
	    digit_h = digit_low;

	  if (!(digit_h & 1))
	    nzeros +=
	      3 & (BID_UINT32) (bid_packed_10000_zeros[digit_h >> 3] >>
			    (digit_h & 7));
	}

	if (nzeros) {
	  // get P*(2^M[extra_digits])/10^extra_digits
	  __mul_128x128_full (Qh, Ql, CQ, bid_reciprocals10_128[nzeros]);

	  // now get P/10^extra_digits: shift Q_high right by M[extra_digits]-128
	  amount = bid_recip_scale[nzeros];
	  __shr_128 (CQ, Qh, amount);
	}
	diff_expon += nzeros;

      }
    }
	if(diff_expon>=0){
    res =
      fast_get_BID64_check_OF (sign_x ^ sign_y, diff_expon, CQ.w[0],
			       rnd_mode, pfpsf);
#ifdef UNCHANGED_BINARY_STATUS_FLAGS
    // (void) fesetexceptflag (&binaryflags, BID_FE_ALL_FLAGS);
#endif
    BID_RETURN_VAL (res);
	}
  }
#endif

  if (diff_expon >= 0) {
#ifdef IEEE_ROUND_NEAREST
  // rounding
  // 2*CA4 - CY
  CA4r.w[1] = (CA4.w[1] + CA4.w[1]) | (CA4.w[0] >> 63);
  CA4r.w[0] = CA4.w[0] + CA4.w[0];
  __sub_borrow_out (CA4r.w[0], carry64, CA4r.w[0], CY.w[0]);
  CA4r.w[1] = CA4r.w[1] - CY.w[1] - carry64;

  D = (CA4r.w[1] | CA4r.w[0]) ? 1 : 0;
  carry64 = (1 + (((BID_SINT64) CA4r.w[1]) >> 63)) & ((CQ.w[0]) | D);

  CQ.w[0] += carry64;
#else
#ifdef IEEE_ROUND_NEAREST_TIES_AWAY
  // rounding
  // 2*CA4 - CY
  CA4r.w[1] = (CA4.w[1] + CA4.w[1]) | (CA4.w[0] >> 63);
  CA4r.w[0] = CA4.w[0] + CA4.w[0];
  __sub_borrow_out (CA4r.w[0], carry64, CA4r.w[0], CY.w[0]);
  CA4r.w[1] = CA4r.w[1] - CY.w[1] - carry64;

  D = (CA4r.w[1] | CA4r.w[0]) ? 0 : 1;
  carry64 = (1 + (((BID_SINT64) CA4r.w[1]) >> 63)) | D;

  CQ.w[0] += carry64;
  if (CQ.w[0] < carry64)
    CQ.w[1]++;
#else
  rmode = rnd_mode;
  if (sign_x ^ sign_y && (unsigned) (rmode - 1) < 2)
    rmode = 3 - rmode;
  switch (rmode) {
  case BID_ROUNDING_TO_NEAREST:	// round to nearest code
    // rounding
    // 2*CA4 - CY
    CA4r.w[1] = (CA4.w[1] + CA4.w[1]) | (CA4.w[0] >> 63);
    CA4r.w[0] = CA4.w[0] + CA4.w[0];
    __sub_borrow_out (CA4r.w[0], carry64, CA4r.w[0], CY.w[0]);
    CA4r.w[1] = CA4r.w[1] - CY.w[1] - carry64;
    D = (CA4r.w[1] | CA4r.w[0]) ? 1 : 0;
    carry64 = (1 + (((BID_SINT64) CA4r.w[1]) >> 63)) & ((CQ.w[0]) | D);
    CQ.w[0] += carry64;
    if (CQ.w[0] < carry64)
      CQ.w[1]++;
    break;
  case BID_ROUNDING_TIES_AWAY:
    // rounding
    // 2*CA4 - CY
    CA4r.w[1] = (CA4.w[1] + CA4.w[1]) | (CA4.w[0] >> 63);
    CA4r.w[0] = CA4.w[0] + CA4.w[0];
    __sub_borrow_out (CA4r.w[0], carry64, CA4r.w[0], CY.w[0]);
    CA4r.w[1] = CA4r.w[1] - CY.w[1] - carry64;
    D = (CA4r.w[1] | CA4r.w[0]) ? 0 : 1;
    carry64 = (1 + (((BID_SINT64) CA4r.w[1]) >> 63)) | D;
    CQ.w[0] += carry64;
    if (CQ.w[0] < carry64)
      CQ.w[1]++;
    break;
  case BID_ROUNDING_DOWN:
  case BID_ROUNDING_TO_ZERO:
    break;
  default:	// rounding up
    CQ.w[0]++;
    if (!CQ.w[0])
      CQ.w[1]++;
    break; 
  }
#endif
#endif
  
    res =
      fast_get_BID64_check_OF (sign_x ^ sign_y, diff_expon, CQ.w[0], rnd_mode,
			       pfpsf);
#ifdef UNCHANGED_BINARY_STATUS_FLAGS
    // (void) fesetexceptflag (&binaryflags, BID_FE_ALL_FLAGS);
#endif
    BID_RETURN_VAL (res);
  } else {
    // UF occurs

#ifdef BID_SET_STATUS_FLAGS
    if ((diff_expon + 16 < 0)) {
      // set status flags
      __set_status_flags (pfpsf, BID_INEXACT_EXCEPTION);
    }
#endif
    rmode = rnd_mode;
    res =
      get_BID64_UF (sign_x ^ sign_y, diff_expon, CQ.w[0], CA4.w[1] | CA4.w[0], rmode, pfpsf);
#ifdef UNCHANGED_BINARY_STATUS_FLAGS
    // (void) fesetexceptflag (&binaryflags, BID_FE_ALL_FLAGS);
#endif
    BID_RETURN_VAL (res);

  }

}


//#define LEAVE_TRAILING_ZEROS

BID_TYPE0_FUNCTION_ARG128_ARGTYPE2 (BID_UINT64, bid64qd_div, x, BID_UINT64, y)

     BID_UINT256 CA4 =
       { {0x0ull, 0x0ull, 0x0ull, 0x0ull} }, CA4r, P256, QB256;
BID_UINT128 CX, CY, T128, CQ, CQ2, CR, CA, TP128, Qh, Ql, Tmp;
BID_UINT64 sign_x, sign_y, carry64, D, Q_low, QX, PD, res, valid_y;
int_float fx, fy, f64;
BID_UINT32 QX32, tdigit[3], digit, digit_h, digit_low;
int exponent_x, exponent_y, bin_index, bin_expon, diff_expon, ed2,
  digits_q, amount;
int nzeros, i, j, k, d5, done = 0;
unsigned rmode;

  BID_OPT_SAVE_BINARY_FLAGS()

valid_y = unpack_BID64 (&sign_y, &exponent_y, &CY.w[0], (y));

	// unpack arguments, check for NaN or Infinity
if (!unpack_BID128_value (&sign_x, &exponent_x, &CX, x)) {
  // test if x is NaN
  if ((x.w[1] & 0x7c00000000000000ull) == 0x7c00000000000000ull) {
#ifdef BID_SET_STATUS_FLAGS
    if ((x.w[1] & 0x7e00000000000000ull) == 0x7e00000000000000ull ||	// sNaN
	(y & 0x7e00000000000000ull) == 0x7e00000000000000ull)
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
    // check if y is Inf.
    if (((y & 0x7c00000000000000ull) == 0x7800000000000000ull))
      // return NaN 
    {
#ifdef BID_SET_STATUS_FLAGS
      __set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
#endif
      res = 0x7c00000000000000ull;
      BID_RETURN_VAL (res);
    }
	if (((y & 0x7c00000000000000ull) != 0x7c00000000000000ull)) {
    // otherwise return +/-Inf
    res =
      ((x.w[1] ^ (y)) & 0x8000000000000000ull) | 0x7800000000000000ull;
    BID_RETURN_VAL (res);
	}
  }
  // x is 0
  if (((y & INFINITY_MASK64) != INFINITY_MASK64)) {
	  if (!CY.w[0]) {
#ifdef BID_SET_STATUS_FLAGS
    __set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
#endif
    // x=y=0, return NaN
    res = 0x7c00000000000000ull;
    BID_RETURN_VAL (res);
    }
    exponent_x =
      exponent_x - exponent_y - DECIMAL_EXPONENT_BIAS_128 +
      (DECIMAL_EXPONENT_BIAS << 1);
    if (exponent_x > DECIMAL_MAX_EXPON_64)
      exponent_x = DECIMAL_MAX_EXPON_64;
    else if (exponent_x < 0)
      exponent_x = 0;
    res = (sign_x ^ sign_y) | (((BID_UINT64) exponent_x) << 53);
    BID_RETURN_VAL (res);
  }
}
CY.w[1] = 0;
if (!valid_y) {
  // y is Inf. or NaN

  // test if y is NaN
  if ((y & NAN_MASK64) == NAN_MASK64) {
#ifdef BID_SET_STATUS_FLAGS
    if ((y & SNAN_MASK64) == SNAN_MASK64)	// sNaN
      __set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
#endif
    BID_RETURN_VAL (CY.w[0] & QUIET_MASK64);
  }
  // y is Infinity?
  if (((y) & 0x7800000000000000ull) == 0x7800000000000000ull) {
    // return +/-0
    res = sign_x ^ sign_y;
    BID_RETURN_VAL (res);
  }
  // y is 0, return +/-Inf
  res =
    ((x.w[1] ^ (y)) & 0x8000000000000000ull) | 0x7800000000000000ull;
#ifdef BID_SET_STATUS_FLAGS
  __set_status_flags (pfpsf, BID_ZERO_DIVIDE_EXCEPTION);
#endif
  BID_RETURN_VAL (res);
}
#ifdef UNCHANGED_BINARY_STATUS_FLAGS
// (void) fegetexceptflag (&binaryflags, BID_FE_ALL_FLAGS);
#endif
diff_expon =
  exponent_x - exponent_y - DECIMAL_EXPONENT_BIAS_128 +
  (DECIMAL_EXPONENT_BIAS << 1);

if (__unsigned_compare_gt_128 (CY, CX)) {
  // CX < CY

  // 2^64
  f64.i = 0x5f800000;

  // fx ~ CX,   fy ~ CY
  fx.d = (float) CX.w[1] * f64.d + (float) CX.w[0];
  fy.d = (float) CY.w[1] * f64.d + (float) CY.w[0];
  // expon_cy - expon_cx
  bin_index = (fy.i - fx.i) >> 23;

// Code redundant as agreed Jul 25 2008. To be removed after verification
//  if (CX.w[1]) {
//    T = bid_power10_index_binexp_128[bin_index].w[0];
//    __mul_64x128_short (CA, T, CX);
//  } else {
    T128 = bid_power10_index_binexp_128[bin_index];
    __mul_64x128_short (CA, CX.w[0], T128);
//  }

  ed2 = 15;
  if (__unsigned_compare_gt_128 (CY, CA))
    ed2++;

  T128 = bid_power10_table_128[ed2];
  __mul_128x128_to_256 (CA4, CA, T128);

  ed2 += bid_estimate_decimal_digits[bin_index];
  CQ.w[0] = CQ.w[1] = 0;
  diff_expon = diff_expon - ed2;

} else {
  // get CQ = CX/CY
  bid___div_128_by_128 (&CQ, &CR, CX, CY);
//printf("CQ=%016I64x,%016I64x, CR=%016I64x %016I64x, p=%I64x\n",CQ.w[1], CQ.w[0],CR.w[1],CR.w[0],power10_table_128[0].w[0]);
//printf("CX=%016I64x,%016I64x, CY=%016I64x %016I64x, p=%I64x\n",CX.w[1], CX.w[0],CY.w[1],CY.w[0],power10_table_128[0].w[0]);

  // get number of decimal digits in CQ
  // 2^64
  f64.i = 0x5f800000;
  fx.d = (float) CQ.w[1] * f64.d + (float) CQ.w[0];
  // binary expon. of CQ
  bin_expon = (fx.i - 0x3f800000) >> 23;

  digits_q = bid_estimate_decimal_digits[bin_expon];
  TP128.w[0] = bid_power10_index_binexp_128[bin_expon].w[0];
  TP128.w[1] = bid_power10_index_binexp_128[bin_expon].w[1];
  if (__unsigned_compare_ge_128 (CQ, TP128))
    digits_q++;

  if (digits_q <= 16) {
    if (!CR.w[1] && !CR.w[0]) {
      res = get_BID64 (sign_x ^ sign_y, diff_expon,
		       CQ.w[0], rnd_mode, pfpsf);
#ifdef UNCHANGED_BINARY_STATUS_FLAGS
      // (void) fesetexceptflag (&binaryflags, BID_FE_ALL_FLAGS);
#endif
      BID_RETURN_VAL (res);
    }

    ed2 = 16 - digits_q;
    T128.w[0] = bid_power10_table_128[ed2].w[0];
    __mul_64x128_to_192 (CA4, (T128.w[0]), CR);
    diff_expon = diff_expon - ed2;
    CQ.w[0] *= T128.w[0];
  } else {
    ed2 = digits_q - 16;
    diff_expon += ed2;
    T128 = bid_reciprocals10_128[ed2];
    __mul_128x128_to_256 (P256, CQ, T128);
    amount = bid_recip_scale[ed2];
    CQ.w[0] = (P256.w[2] >> amount) | (P256.w[3] << (64 - amount));
    CQ.w[1] = 0;
//printf("CQ=%016I64x,%016I64x, CR=%016I64x %016I64x, p=%I64x\n",CQ.w[1], CQ.w[0],CR.w[1],CR.w[0],power10_table_128[0].w[0]);

    __mul_64x64_to_128 (CQ2, CQ.w[0], (bid_power10_table_128[ed2].w[0])); 

    __mul_64x64_to_128 (QB256, CQ2.w[0], CY.w[0]);
    QB256.w[1] += CQ2.w[0] * CY.w[1] + CQ2.w[1] * CY.w[0];
//printf("CQ2=%016I64x,%016I64x, CB=%016I64x %016I64x, p=%I64x\n",CQ2.w[1], CQ2.w[0],QB256.w[1],QB256.w[0],power10_table_128[0].w[0]);

    CA4.w[1] = CX.w[1] - QB256.w[1];
    CA4.w[0] = CX.w[0] - QB256.w[0];
    if (CX.w[0] < QB256.w[0])
      CA4.w[1]--;
//printf("CA4_0=%016I64x,%016I64x, CY=%016I64x %016I64x, p=%I64x\n",CA4.w[1], CA4.w[0],CY.w[1],CY.w[0],power10_table_128[ed2].w[0]);
    /*if (CR.w[0] || CR.w[1])
      CA4.w[0] |= 1;*/
    done = 1;
//printf("CA4=%016I64x,%016I64x, CY=%016I64x %016I64x, p=%I64x\n",CA4.w[1], CA4.w[0],CY.w[1],CY.w[0],power10_table_128[ed2].w[0]);
	if(CA4.w[1]|CA4.w[0]) {
    __mul_64x128_low(CY, (bid_power10_table_128[ed2].w[0]),CY);
	}

  }

}

if (!done) {
  bid___div_256_by_128 (&CQ, &CA4, CY);
}

#ifdef BID_SET_STATUS_FLAGS
  if (CA4.w[0] || CA4.w[1]) {
    // set status flags
    __set_status_flags (pfpsf, BID_INEXACT_EXCEPTION);
  }
#ifndef LEAVE_TRAILING_ZEROS
  else 
#endif
#else
#ifndef LEAVE_TRAILING_ZEROS
  if (!CA4.w[0] && !CA4.w[1])
#endif
#endif
#ifndef LEAVE_TRAILING_ZEROS
    // check whether result is exact
  {
	  if(!done) {
    // check whether CX, CY are short
    if (!CX.w[1] && !CY.w[1] && (CX.w[0] <= 1024) && (CY.w[0] <= 1024)) {
      i = (int) CY.w[0] - 1;
      j = (int) CX.w[0] - 1;
      // difference in powers of 2 bid_factors for Y and X
      nzeros = ed2 - bid_factors[i][0] + bid_factors[j][0];
      // difference in powers of 5 bid_factors
      d5 = ed2 - bid_factors[i][1] + bid_factors[j][1];
      if (d5 < nzeros)
		nzeros = d5;
      // get P*(2^M[extra_digits])/10^extra_digits
      __mul_128x128_full (Qh, Ql, CQ, bid_reciprocals10_128[nzeros]);
      //__mul_128x128_to_256(P256, CQ, bid_reciprocals10_128[nzeros]);Qh.w[1]=P256.w[3];Qh.w[0]=P256.w[2];

      // now get P/10^extra_digits: shift Q_high right by M[extra_digits]-128
      amount = bid_recip_scale[nzeros];
      __shr_128_long (CQ, Qh, amount);

      diff_expon += nzeros;
    } else {
      // decompose Q as Qh*10^17 + Ql
      //T128 = bid_reciprocals10_128[17];
      Q_low = CQ.w[0];

      {
	tdigit[0] = Q_low & 0x3ffffff;
	tdigit[1] = 0;
	QX = Q_low >> 26;
	QX32 = QX;
	nzeros = 0;

	for (j = 0; QX32; j++, QX32 >>= 7) {
	  k = (QX32 & 127);
	  tdigit[0] += bid_convert_table[j][k][0];
	  tdigit[1] += bid_convert_table[j][k][1];
	  if (tdigit[0] >= 100000000) {
	    tdigit[0] -= 100000000;
	    tdigit[1]++;
	  }
	}

	digit = tdigit[0];
	if (!digit && !tdigit[1])
	  nzeros += 16;
	else {
	  if (!digit) {
	    nzeros += 8;
	    digit = tdigit[1];
	  }
	  // decompose digit
	  PD = (BID_UINT64) digit *0x068DB8BBull;
	  digit_h = (BID_UINT32) (PD >> 40);
	  digit_low = digit - digit_h * 10000;

	  if (!digit_low)
	    nzeros += 4;
	  else
	    digit_h = digit_low;

	  if (!(digit_h & 1))
	    nzeros +=
	      3 & (BID_UINT32) (bid_packed_10000_zeros[digit_h >> 3] >>
			    (digit_h & 7));
	}

	if (nzeros) {
	  // get P*(2^M[extra_digits])/10^extra_digits
	  __mul_128x128_full (Qh, Ql, CQ, bid_reciprocals10_128[nzeros]);

	  // now get P/10^extra_digits: shift Q_high right by M[extra_digits]-128
	  amount = bid_recip_scale[nzeros];
	  __shr_128 (CQ, Qh, amount);
	}
	diff_expon += nzeros;

      }
    }
	  }
	if(diff_expon>=0){
    res =
      fast_get_BID64_check_OF (sign_x ^ sign_y, diff_expon, CQ.w[0],
			       rnd_mode, pfpsf);
#ifdef UNCHANGED_BINARY_STATUS_FLAGS
    // (void) fesetexceptflag (&binaryflags, BID_FE_ALL_FLAGS);
#endif
    BID_RETURN_VAL (res);
	}
  }
#endif

  if (diff_expon >= 0) {
#ifdef IEEE_ROUND_NEAREST
  // rounding
  // 2*CA4 - CY
  CA4r.w[1] = (CA4.w[1] + CA4.w[1]) | (CA4.w[0] >> 63);
  CA4r.w[0] = CA4.w[0] + CA4.w[0];
  __sub_borrow_out (CA4r.w[0], carry64, CA4r.w[0], CY.w[0]);
  CA4r.w[1] = CA4r.w[1] - CY.w[1] - carry64;

  D = (CA4r.w[1] | CA4r.w[0]) ? 1 : 0;
  carry64 = (1 + (((BID_SINT64) CA4r.w[1]) >> 63)) & ((CQ.w[0]) | D);

  CQ.w[0] += carry64;
  //if(CQ.w[0]<carry64)
  //CQ.w[1] ++;
#else
#ifdef IEEE_ROUND_NEAREST_TIES_AWAY
  // rounding
  // 2*CA4 - CY
  CA4r.w[1] = (CA4.w[1] + CA4.w[1]) | (CA4.w[0] >> 63);
  CA4r.w[0] = CA4.w[0] + CA4.w[0];
  __sub_borrow_out (CA4r.w[0], carry64, CA4r.w[0], CY.w[0]);
  CA4r.w[1] = CA4r.w[1] - CY.w[1] - carry64;

  D = (CA4r.w[1] | CA4r.w[0]) ? 0 : 1;
  carry64 = (1 + (((BID_SINT64) CA4r.w[1]) >> 63)) | D;

  CQ.w[0] += carry64;
  if (CQ.w[0] < carry64)
    CQ.w[1]++;
#else
  rmode = rnd_mode;
  if (sign_x ^ sign_y && (unsigned) (rmode - 1) < 2)
    rmode = 3 - rmode;
  switch (rmode) {
  case BID_ROUNDING_TO_NEAREST:	// round to nearest code
    // rounding
    // 2*CA4 - CY
//printf("CA4=%016I64x,%016I64x\n",CA4.w[1], CA4.w[0]);
    CA4r.w[1] = (CA4.w[1] + CA4.w[1]) | (CA4.w[0] >> 63);
    CA4r.w[0] = CA4.w[0] + CA4.w[0];
    __sub_borrow_out (CA4r.w[0], carry64, CA4r.w[0], CY.w[0]);
    CA4r.w[1] = CA4r.w[1] - CY.w[1] - carry64;
    D = (CA4r.w[1] | CA4r.w[0]) ? 1 : 0;
    carry64 = (1 + (((BID_SINT64) CA4r.w[1]) >> 63)) & ((CQ.w[0]) | D);
  //printf("CQ=%016I64x, carry=%I64x\n",CQ.w[0], carry64);
    CQ.w[0] += carry64;
    if (CQ.w[0] < carry64)
      CQ.w[1]++;
    break;
  case BID_ROUNDING_TIES_AWAY:
    // rounding
    // 2*CA4 - CY
    CA4r.w[1] = (CA4.w[1] + CA4.w[1]) | (CA4.w[0] >> 63);
    CA4r.w[0] = CA4.w[0] + CA4.w[0];
    __sub_borrow_out (CA4r.w[0], carry64, CA4r.w[0], CY.w[0]);
    CA4r.w[1] = CA4r.w[1] - CY.w[1] - carry64;
    D = (CA4r.w[1] | CA4r.w[0]) ? 0 : 1;
    carry64 = (1 + (((BID_SINT64) CA4r.w[1]) >> 63)) | D;
    CQ.w[0] += carry64;
    if (CQ.w[0] < carry64)
      CQ.w[1]++;
    break;
  case BID_ROUNDING_DOWN:
  case BID_ROUNDING_TO_ZERO:
    break;
  default:	// rounding up
    CQ.w[0]++;
    if (!CQ.w[0])
      CQ.w[1]++;
    break;
  }
#endif
#endif
 
    res =
      fast_get_BID64_check_OF (sign_x ^ sign_y, diff_expon, CQ.w[0], rnd_mode,
			       pfpsf);
#ifdef UNCHANGED_BINARY_STATUS_FLAGS
    // (void) fesetexceptflag (&binaryflags, BID_FE_ALL_FLAGS);
#endif
    BID_RETURN_VAL (res);
  } else {
    // UF occurs

#ifdef BID_SET_STATUS_FLAGS
    if ((diff_expon + 16 < 0)) {
      // set status flags
      __set_status_flags (pfpsf, BID_INEXACT_EXCEPTION);
    }
#endif
    rmode = rnd_mode;
    res =
      get_BID64_UF (sign_x ^ sign_y, diff_expon, CQ.w[0], CA4.w[1] | CA4.w[0], rmode, pfpsf);
#ifdef UNCHANGED_BINARY_STATUS_FLAGS
    // (void) fesetexceptflag (&binaryflags, BID_FE_ALL_FLAGS);
#endif
    BID_RETURN_VAL (res);

  }

}

//#define LEAVE_TRAILING_ZEROS

BID_EXTERN_C const BID_UINT32 bid_convert_table[5][128][2];
BID_EXTERN_C const BID_SINT8 bid_factors[][2];
BID_EXTERN_C const BID_UINT8 bid_packed_10000_zeros[];


//BID_UINT64* bid64_div128x128(BID_UINT64 res, BID_UINT128 *px, BID_UINT128 *py, unsigned rnd_mode, unsigned *pfpsf) 

BID_TYPE0_FUNCTION_ARG128_ARG128 (BID_UINT64, bid64qq_div, x, y)
     BID_UINT256 CA4 =
       { {0x0ull, 0x0ull, 0x0ull, 0x0ull} }, CA4r, P256, QB256;
BID_UINT128 CX, CY, T128, CQ, CQ2, CR, CA, TP128, Qh, Ql, Tmp;
BID_UINT64 sign_x, sign_y, T, carry64, D, Q_low, QX, valid_y, PD, res;
int_float fx, fy, f64;
BID_UINT32 QX32, tdigit[3], digit, digit_h, digit_low;
int exponent_x, exponent_y, bin_index, bin_expon, diff_expon, ed2,
  digits_q, amount;
int nzeros, i, j, k, d5, done = 0;
unsigned rmode;

  BID_OPT_SAVE_BINARY_FLAGS()

valid_y = unpack_BID128_value (&sign_y, &exponent_y, &CY, y);

	// unpack arguments, check for NaN or Infinity
if (!unpack_BID128_value (&sign_x, &exponent_x, &CX, x)) {
  // test if x is NaN
  if ((x.w[1] & 0x7c00000000000000ull) == 0x7c00000000000000ull) {
#ifdef BID_SET_STATUS_FLAGS
    if ((x.w[1] & 0x7e00000000000000ull) == 0x7e00000000000000ull ||	// sNaN
	(y.w[1] & 0x7e00000000000000ull) == 0x7e00000000000000ull)
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
    // check if y is Inf.
    if (((y.w[1] & 0x7c00000000000000ull) == 0x7800000000000000ull))
      // return NaN 
    {
#ifdef BID_SET_STATUS_FLAGS
      __set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
#endif
      res = 0x7c00000000000000ull;
      BID_RETURN_VAL (res);
    }
	if (((y.w[1] & 0x7c00000000000000ull) != 0x7c00000000000000ull)) {
    // otherwise return +/-Inf
    res =
      ((x.w[1] ^ y.
	w[1]) & 0x8000000000000000ull) | 0x7800000000000000ull;
    BID_RETURN_VAL (res);
	}
  }
  // x is 0
  if (((y.w[1] & 0x7800000000000000ull) != 0x7800000000000000ull)) {
  if ((!CY.w[0]) && !(CY.w[1] & 0x0001ffffffffffffull)) {
#ifdef BID_SET_STATUS_FLAGS
    __set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
#endif
    // x=y=0, return NaN
    res = 0x7c00000000000000ull;
    BID_RETURN_VAL (res);
  }
  // return 0
  res = (x.w[1] ^ y.w[1]) & 0x8000000000000000ull;
  exponent_x = exponent_x - exponent_y + DECIMAL_EXPONENT_BIAS;
  if (exponent_x > DECIMAL_MAX_EXPON_64)
    exponent_x = DECIMAL_MAX_EXPON_64;
  else if (exponent_x < 0)
    exponent_x = 0;
  res |= (((BID_UINT64) exponent_x) << 53);
  BID_RETURN_VAL (res);
  }
}
if (!valid_y) {
  // y is Inf. or NaN

  // test if y is NaN
  if ((y.w[1] & 0x7c00000000000000ull) == 0x7c00000000000000ull) {
#ifdef BID_SET_STATUS_FLAGS
    if ((y.w[1] & 0x7e00000000000000ull) == 0x7e00000000000000ull)	// sNaN
      __set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
#endif
      Tmp.w[1] = (CY.w[1] & 0x00003fffffffffffull);
      Tmp.w[0] = CY.w[0];
      TP128 = bid_reciprocals10_128[18];
      __mul_128x128_full (Qh, Ql, Tmp, TP128);
      amount = bid_recip_scale[18];
      __shr_128 (Tmp, Qh, amount);
      res = (CY.w[1] & 0xfc00000000000000ull) | Tmp.w[0];
    BID_RETURN_VAL (res);
  }
  // y is Infinity?
  if ((y.w[1] & 0x7800000000000000ull) == 0x7800000000000000ull) {
    // return +/-0
    res = sign_x ^ sign_y;
    BID_RETURN_VAL (res);
  }
  // y is 0, return +/-Inf
  res =
    ((x.w[1] ^ y.w[1]) & 0x8000000000000000ull) | 0x7800000000000000ull;
#ifdef BID_SET_STATUS_FLAGS
  __set_status_flags (pfpsf, BID_ZERO_DIVIDE_EXCEPTION);
#endif
  BID_RETURN_VAL (res);
}
#ifdef UNCHANGED_BINARY_STATUS_FLAGS
// (void) fegetexceptflag (&binaryflags, BID_FE_ALL_FLAGS);
#endif
diff_expon = exponent_x - exponent_y + DECIMAL_EXPONENT_BIAS;

if (__unsigned_compare_gt_128 (CY, CX)) {
  // CX < CY

  // 2^64
  f64.i = 0x5f800000;

  // fx ~ CX,   fy ~ CY
  fx.d = (float) CX.w[1] * f64.d + (float) CX.w[0];
  fy.d = (float) CY.w[1] * f64.d + (float) CY.w[0];
  // expon_cy - expon_cx
  bin_index = (fy.i - fx.i) >> 23;

  if (CX.w[1]) {
    T = bid_power10_index_binexp_128[bin_index].w[0];
    __mul_64x128_short (CA, T, CX);
  } else {
    T128 = bid_power10_index_binexp_128[bin_index];
    __mul_64x128_short (CA, CX.w[0], T128);
  }

  ed2 = 15;
  if (__unsigned_compare_gt_128 (CY, CA))
    ed2++;

  T128 = bid_power10_table_128[ed2];
  __mul_128x128_to_256 (CA4, CA, T128);

  ed2 += bid_estimate_decimal_digits[bin_index];
  CQ.w[0] = CQ.w[1] = 0;
  diff_expon = diff_expon - ed2;

} else {
  // get CQ = CX/CY
  bid___div_128_by_128 (&CQ, &CR, CX, CY);

  // get number of decimal digits in CQ
  // 2^64
  f64.i = 0x5f800000;
  fx.d = (float) CQ.w[1] * f64.d + (float) CQ.w[0];
  // binary expon. of CQ
  bin_expon = (fx.i - 0x3f800000) >> 23;

  digits_q = bid_estimate_decimal_digits[bin_expon];
  TP128.w[0] = bid_power10_index_binexp_128[bin_expon].w[0];
  TP128.w[1] = bid_power10_index_binexp_128[bin_expon].w[1];
  if (__unsigned_compare_ge_128 (CQ, TP128))
    digits_q++;

  if (digits_q <= 16) {
    if (!CR.w[1] && !CR.w[0]) {
      res = get_BID64 (sign_x ^ sign_y, diff_expon,
		       CQ.w[0], rnd_mode, pfpsf);
#ifdef UNCHANGED_BINARY_STATUS_FLAGS
      // (void) fesetexceptflag (&binaryflags, BID_FE_ALL_FLAGS);
#endif
      BID_RETURN_VAL (res);
    }

    ed2 = 16 - digits_q;
    T128.w[0] = bid_power10_table_128[ed2].w[0];
    __mul_64x128_to_192 (CA4, (T128.w[0]), CR);
    diff_expon = diff_expon - ed2;
    CQ.w[0] *= T128.w[0];
  } else {
    ed2 = digits_q - 16;
    diff_expon += ed2;
    T128 = bid_reciprocals10_128[ed2];
    __mul_128x128_to_256 (P256, CQ, T128);
    amount = bid_recip_scale[ed2];
    CQ.w[0] = (P256.w[2] >> amount) | (P256.w[3] << (64 - amount));
    CQ.w[1] = 0;

    __mul_64x64_to_128 (CQ2, CQ.w[0], (bid_power10_table_128[ed2].w[0]));

    __mul_64x64_to_128 (QB256, CQ2.w[0], CY.w[0]);
    QB256.w[1] += CQ2.w[0] * CY.w[1] + CQ2.w[1] * CY.w[0];

    CA4.w[1] = CX.w[1] - QB256.w[1];
    CA4.w[0] = CX.w[0] - QB256.w[0];
    if (CX.w[0] < QB256.w[0])
      CA4.w[1]--;
    /*if (CR.w[0] || CR.w[1])
      CA4.w[0] |= 1;*/
    done = 1;
	if(CA4.w[1]|CA4.w[0]) {
    __mul_64x128_low(CY, (bid_power10_table_128[ed2].w[0]),CY);
	}
  }

}

if (!done) {
  bid___div_256_by_128 (&CQ, &CA4, CY);
}

 

#ifdef BID_SET_STATUS_FLAGS
  if (CA4.w[0] || CA4.w[1]) {
    // set status flags
    __set_status_flags (pfpsf, BID_INEXACT_EXCEPTION);
  }
#ifndef LEAVE_TRAILING_ZEROS
  else 
#endif
#else
#ifndef LEAVE_TRAILING_ZEROS
  if (!CA4.w[0] && !CA4.w[1])
#endif
#endif
#ifndef LEAVE_TRAILING_ZEROS
    // check whether result is exact
  {
	  if(!done) {
    // check whether CX, CY are short
    if (!CX.w[1] && !CY.w[1] && (CX.w[0] <= 1024) && (CY.w[0] <= 1024)) {
      i = (int) CY.w[0] - 1;
      j = (int) CX.w[0] - 1;
      // difference in powers of 2 bid_factors for Y and X
      nzeros = ed2 - bid_factors[i][0] + bid_factors[j][0];
      // difference in powers of 5 bid_factors
      d5 = ed2 - bid_factors[i][1] + bid_factors[j][1];
      if (d5 < nzeros)
	nzeros = d5;
      // get P*(2^M[extra_digits])/10^extra_digits
      __mul_128x128_full (Qh, Ql, CQ, bid_reciprocals10_128[nzeros]);
      //__mul_128x128_to_256(P256, CQ, bid_reciprocals10_128[nzeros]);Qh.w[1]=P256.w[3];Qh.w[0]=P256.w[2];

      // now get P/10^extra_digits: shift Q_high right by M[extra_digits]-128
      amount = bid_recip_scale[nzeros];
      __shr_128_long (CQ, Qh, amount);

      diff_expon += nzeros;
    } else {
      // decompose Q as Qh*10^17 + Ql
      //T128 = bid_reciprocals10_128[17];
      Q_low = CQ.w[0];

      {
	tdigit[0] = Q_low & 0x3ffffff;
	tdigit[1] = 0;
	QX = Q_low >> 26;
	QX32 = QX;
	nzeros = 0;

	for (j = 0; QX32; j++, QX32 >>= 7) {
	  k = (QX32 & 127);
	  tdigit[0] += bid_convert_table[j][k][0];
	  tdigit[1] += bid_convert_table[j][k][1];
	  if (tdigit[0] >= 100000000) {
	    tdigit[0] -= 100000000;
	    tdigit[1]++;
	  }
	}

	digit = tdigit[0];
	if (!digit && !tdigit[1])
	  nzeros += 16;
	else {
	  if (!digit) {
	    nzeros += 8;
	    digit = tdigit[1];
	  }
	  // decompose digit
	  PD = (BID_UINT64) digit *0x068DB8BBull;
	  digit_h = (BID_UINT32) (PD >> 40);
	  digit_low = digit - digit_h * 10000;

	  if (!digit_low)
	    nzeros += 4;
	  else
	    digit_h = digit_low;

	  if (!(digit_h & 1))
	    nzeros +=
	      3 & (BID_UINT32) (bid_packed_10000_zeros[digit_h >> 3] >>
			    (digit_h & 7));
	}

	if (nzeros) {
	  // get P*(2^M[extra_digits])/10^extra_digits
	  __mul_128x128_full (Qh, Ql, CQ, bid_reciprocals10_128[nzeros]);

	  // now get P/10^extra_digits: shift Q_high right by M[extra_digits]-128
	  amount = bid_recip_scale[nzeros];
	  __shr_128 (CQ, Qh, amount);
	}
	diff_expon += nzeros;

      }
    }
	  }
	if(diff_expon>=0){
    res =
      fast_get_BID64_check_OF (sign_x ^ sign_y, diff_expon, CQ.w[0],
			       rnd_mode, pfpsf);
#ifdef UNCHANGED_BINARY_STATUS_FLAGS
    // (void) fesetexceptflag (&binaryflags, BID_FE_ALL_FLAGS);
#endif
    BID_RETURN_VAL (res);
	}
  }
#endif

  if(diff_expon>=0) {

#ifdef IEEE_ROUND_NEAREST
  // rounding
  // 2*CA4 - CY
  CA4r.w[1] = (CA4.w[1] + CA4.w[1]) | (CA4.w[0] >> 63);
  CA4r.w[0] = CA4.w[0] + CA4.w[0];
  __sub_borrow_out (CA4r.w[0], carry64, CA4r.w[0], CY.w[0]);
  CA4r.w[1] = CA4r.w[1] - CY.w[1] - carry64;

  D = (CA4r.w[1] | CA4r.w[0]) ? 1 : 0;
  carry64 = (1 + (((BID_SINT64) CA4r.w[1]) >> 63)) & ((CQ.w[0]) | D);

  CQ.w[0] += carry64;
  //if(CQ.w[0]<carry64)
  //CQ.w[1] ++;
#else
#ifdef IEEE_ROUND_NEAREST_TIES_AWAY
  // rounding
  // 2*CA4 - CY
  CA4r.w[1] = (CA4.w[1] + CA4.w[1]) | (CA4.w[0] >> 63);
  CA4r.w[0] = CA4.w[0] + CA4.w[0];
  __sub_borrow_out (CA4r.w[0], carry64, CA4r.w[0], CY.w[0]);
  CA4r.w[1] = CA4r.w[1] - CY.w[1] - carry64;

  D = (CA4r.w[1] | CA4r.w[0]) ? 0 : 1;
  carry64 = (1 + (((BID_SINT64) CA4r.w[1]) >> 63)) | D;

  CQ.w[0] += carry64;
  if (CQ.w[0] < carry64)
    CQ.w[1]++;
#else
  rmode = rnd_mode;
  if (sign_x ^ sign_y && (unsigned) (rmode - 1) < 2)
    rmode = 3 - rmode;
  switch (rmode) {
  case BID_ROUNDING_TO_NEAREST:	// round to nearest code
    // rounding
    // 2*CA4 - CY
    CA4r.w[1] = (CA4.w[1] + CA4.w[1]) | (CA4.w[0] >> 63);
    CA4r.w[0] = CA4.w[0] + CA4.w[0];
    __sub_borrow_out (CA4r.w[0], carry64, CA4r.w[0], CY.w[0]);
    CA4r.w[1] = CA4r.w[1] - CY.w[1] - carry64;
    D = (CA4r.w[1] | CA4r.w[0]) ? 1 : 0;
    carry64 = (1 + (((BID_SINT64) CA4r.w[1]) >> 63)) & ((CQ.w[0]) | D);
    CQ.w[0] += carry64;
    if (CQ.w[0] < carry64)
      CQ.w[1]++;
    break;
  case BID_ROUNDING_TIES_AWAY:
    // rounding
    // 2*CA4 - CY
    CA4r.w[1] = (CA4.w[1] + CA4.w[1]) | (CA4.w[0] >> 63);
    CA4r.w[0] = CA4.w[0] + CA4.w[0];
    __sub_borrow_out (CA4r.w[0], carry64, CA4r.w[0], CY.w[0]);
    CA4r.w[1] = CA4r.w[1] - CY.w[1] - carry64;
    D = (CA4r.w[1] | CA4r.w[0]) ? 0 : 1;
    carry64 = (1 + (((BID_SINT64) CA4r.w[1]) >> 63)) | D;
    CQ.w[0] += carry64;
    if (CQ.w[0] < carry64)
      CQ.w[1]++;
    break;
  case BID_ROUNDING_DOWN:
  case BID_ROUNDING_TO_ZERO:
    break;
  default:	// rounding up
    CQ.w[0]++;
    if (!CQ.w[0])
      CQ.w[1]++;
    break;
  }
#endif
#endif

  
    res =
      fast_get_BID64_check_OF (sign_x ^ sign_y, diff_expon, CQ.w[0], rnd_mode,
			       pfpsf);
#ifdef UNCHANGED_BINARY_STATUS_FLAGS
    // (void) fesetexceptflag (&binaryflags, BID_FE_ALL_FLAGS);
#endif
    BID_RETURN_VAL (res);
  } else {
    // UF occurs

#ifdef BID_SET_STATUS_FLAGS
    if ((diff_expon + 16 < 0)) {
      // set status flags
      __set_status_flags (pfpsf, BID_INEXACT_EXCEPTION);
    }
#endif
    rmode = rnd_mode;
    res =
      get_BID64_UF (sign_x ^ sign_y, diff_expon, CQ.w[0], CA4.w[1] | CA4.w[0], rmode, pfpsf);
#ifdef UNCHANGED_BINARY_STATUS_FLAGS
    // (void) fesetexceptflag (&binaryflags, BID_FE_ALL_FLAGS);
#endif
    BID_RETURN_VAL (res);

  }

}
