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
#include "bid_div_macros.h"

BID_EXTERN_C const BID_UINT32 bid_convert_table[5][128][2];
BID_EXTERN_C const BID_SINT8 bid_factors[][2];
BID_EXTERN_C const BID_UINT8 bid_packed_10000_zeros[];

BID128_FUNCTION_ARG2 (bid128_div, x, y)

     BID_UINT256 CA4, CA4r, P256;
     BID_UINT128 CX, CY, T128, CQ, CR, CA, TP128, Qh, Ql, res;
     BID_UINT64 sign_x, sign_y, T, carry64, D, Q_high, Q_low, QX, PD,
       valid_y;
     int_float fx, fy, f64;
     BID_UINT32 QX32, tdigit[3], digit, digit_h, digit_low;
     int exponent_x, exponent_y, bin_index, bin_expon, diff_expon, ed2,
       digits_q, amount;
     int nzeros, i, j, k, d5;
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
  res.w[1] = (CX.w[1]) & QUIET_MASK64;
  res.w[0] = CX.w[0];
  BID_RETURN (res);
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
    res.w[1] = 0x7c00000000000000ull;
    res.w[0] = 0;
    BID_RETURN (res);
  }
  // y is NaN?
  if (((y.w[1] & 0x7c00000000000000ull) != 0x7c00000000000000ull))
    // return NaN 
  {
    // return +/-Inf
    res.w[1] = ((x.w[1] ^ y.w[1]) & 0x8000000000000000ull) |
      0x7800000000000000ull;
    res.w[0] = 0;
    BID_RETURN (res);
  }
}
    // x is 0
if ((y.w[1] & 0x7800000000000000ull) < 0x7800000000000000ull) {
  if ((!CY.w[0]) && !(CY.w[1] & 0x0001ffffffffffffull)) {
#ifdef BID_SET_STATUS_FLAGS
    __set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
#endif
    // x=y=0, return NaN
    res.w[1] = 0x7c00000000000000ull;
    res.w[0] = 0;
    BID_RETURN (res);
  }
  // return 0
  res.w[1] = (x.w[1] ^ y.w[1]) & 0x8000000000000000ull;
  exponent_x = exponent_x - exponent_y + DECIMAL_EXPONENT_BIAS_128;
  if (exponent_x > DECIMAL_MAX_EXPON_128)
    exponent_x = DECIMAL_MAX_EXPON_128;
  else if (exponent_x < 0)
    exponent_x = 0;
  res.w[1] |= (((BID_UINT64) exponent_x) << 49);
  res.w[0] = 0;
  BID_RETURN (res);
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
    res.w[1] = CY.w[1] & QUIET_MASK64;
    res.w[0] = CY.w[0];
    BID_RETURN (res);
  }
  // y is Infinity?
  if ((y.w[1] & 0x7800000000000000ull) == 0x7800000000000000ull) {
    // return +/-0
    res.w[1] = sign_x ^ sign_y;
    res.w[0] = 0;
    BID_RETURN (res);
  }
  // y is 0, return +/-Inf
#ifdef BID_SET_STATUS_FLAGS
  __set_status_flags (pfpsf, BID_ZERO_DIVIDE_EXCEPTION);
#endif
  res.w[1] =
    ((x.w[1] ^ y.w[1]) & 0x8000000000000000ull) | 0x7800000000000000ull;
  res.w[0] = 0;
  BID_RETURN (res);
}
#ifdef UNCHANGED_BINARY_STATUS_FLAGS
// (void) fegetexceptflag (&binaryflags, BID_FE_ALL_FLAGS);
#endif
diff_expon = exponent_x - exponent_y + DECIMAL_EXPONENT_BIAS_128;

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

  ed2 = 33;
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

  if (!CR.w[1] && !CR.w[0]) {
    bid_get_BID128 (&res, sign_x ^ sign_y, diff_expon, CQ, &rnd_mode,
		pfpsf);
#ifdef UNCHANGED_BINARY_STATUS_FLAGS
    // (void) fesetexceptflag (&binaryflags, BID_FE_ALL_FLAGS);
#endif
    BID_RETURN (res);
  }
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

  ed2 = 34 - digits_q;
  T128.w[0] = bid_power10_table_128[ed2].w[0];
  T128.w[1] = bid_power10_table_128[ed2].w[1];
  __mul_128x128_to_256 (CA4, CR, T128);
  diff_expon = diff_expon - ed2;
  __mul_128x128_low (CQ, CQ, T128);

}

bid___div_256_by_128 (&CQ, &CA4, CY);

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
    //T128 = bid_reciprocals10_128[17];
    T128.w[0] = 0x44909befeb9fad49ull;
    T128.w[1] = 0x000b877aa3236a4bull;
    __mul_128x128_to_256 (P256, CQ, T128);
    //amount = bid_recip_scale[17];
    Q_high = (P256.w[2] >> 44) | (P256.w[3] << (64 - 44));
    Q_low = CQ.w[0] - Q_high * 100000000000000000ull;

    if (!Q_low) {
      diff_expon += 17;

      tdigit[0] = Q_high & 0x3ffffff;
      tdigit[1] = 0;
      QX = Q_high >> 26;
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

      if (tdigit[1] >= 100000000) {
	tdigit[1] -= 100000000;
	if (tdigit[1] >= 100000000)
	  tdigit[1] -= 100000000;
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
	__mul_64x64_to_128 (CQ, Q_high, bid_reciprocals10_64[nzeros]);

	// now get P/10^extra_digits: shift C64 right by M[extra_digits]-64
	amount = bid_short_recip_scale[nzeros];
	CQ.w[0] = CQ.w[1] >> amount;
      } else
	CQ.w[0] = Q_high;
      CQ.w[1] = 0;

      diff_expon += nzeros;
    } else {
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

      if (tdigit[1] >= 100000000) {
	tdigit[1] -= 100000000;
	if (tdigit[1] >= 100000000)
	  tdigit[1] -= 100000000;
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

	//now get P/10^extra_digits: shift Q_high right by M[extra_digits]-128
	amount = bid_recip_scale[nzeros];
	__shr_128 (CQ, Qh, amount);
      }
      diff_expon += nzeros;

    }
  }
  bid_get_BID128 (&res, sign_x ^ sign_y, diff_expon, CQ, &rnd_mode, pfpsf);
#ifdef UNCHANGED_BINARY_STATUS_FLAGS
  // (void) fesetexceptflag (&binaryflags, BID_FE_ALL_FLAGS);
#endif
  BID_RETURN (res);
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
  if (CQ.w[0] < carry64)
    CQ.w[1]++;
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

} else {
#ifdef BID_SET_STATUS_FLAGS
  if (CA4.w[0] || CA4.w[1]) {
    // set status flags
    __set_status_flags (pfpsf, BID_INEXACT_EXCEPTION);
  }
#endif

  bid_handle_UF_128_rem (&res, sign_x ^ sign_y, diff_expon, CQ,
		     CA4.w[1] | CA4.w[0], &rnd_mode, pfpsf);
#ifdef UNCHANGED_BINARY_STATUS_FLAGS
  // (void) fesetexceptflag (&binaryflags, BID_FE_ALL_FLAGS);
#endif
  BID_RETURN (res);

}

bid_get_BID128 (&res, sign_x ^ sign_y, diff_expon, CQ, &rnd_mode, pfpsf);
#ifdef UNCHANGED_BINARY_STATUS_FLAGS
// (void) fesetexceptflag (&binaryflags, BID_FE_ALL_FLAGS);
#endif
BID_RETURN (res);
}


//#define LEAVE_TRAILING_ZEROS

BID_TYPE0_FUNCTION_ARGTYPE1_ARGTYPE2 (BID_UINT128, bid128dd_div, BID_UINT64, x,
				  BID_UINT64, y)

     BID_UINT256 CA4, CA4r, P256;
     BID_UINT128 CX, CY, T128, CQ, CR, CA, TP128, Qh, Ql, res;
     BID_UINT64 sign_x, sign_y, carry64, D, Q_high, Q_low, QX, PD,
       valid_y;
     int_float fx, fy, f64;
     BID_UINT32 QX32, tdigit[3], digit, digit_h, digit_low;
     int exponent_x, exponent_y, bin_index, bin_expon, diff_expon, ed2,
       digits_q, amount;
     int nzeros, i, j, k, d5;
     unsigned rmode;

  BID_OPT_SAVE_BINARY_FLAGS()

valid_y = unpack_BID64 (&sign_y, &exponent_y, &CY.w[0], y);

	// unpack arguments, check for NaN or Infinity
CX.w[1] = 0;
if (!unpack_BID64 (&sign_x, &exponent_x, &CX.w[0], (x))) {
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
  res.w[0] = (CX.w[0] & 0x0003ffffffffffffull);
  __mul_64x64_to_128 (res, res.w[0], bid_power10_table_128[18].w[0]);
  res.w[1] |= ((CX.w[0]) & 0xfc00000000000000ull);
  BID_RETURN (res);
}
	   // x is Infinity?
if (((x) & 0x7800000000000000ull) == 0x7800000000000000ull) {
  // check if y is Inf.
  if ((((y) & 0x7c00000000000000ull) == 0x7800000000000000ull))
    // return NaN 
  {
#ifdef BID_SET_STATUS_FLAGS
  __set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
#endif
  res.w[1] = 0x7c00000000000000ull;
  res.w[0] = 0;
    BID_RETURN (res);
  }
  if ((((y) & 0x7c00000000000000ull) != 0x7c00000000000000ull)) {
  // otherwise return +/-Inf
  res.w[1] =
    (((x) ^ (y)) & 0x8000000000000000ull) | 0x7800000000000000ull;
  res.w[0] = 0;
  BID_RETURN (res);
  }
}
	   // x is 0
if ((((y) & 0x7800000000000000ull) != 0x7800000000000000ull)) {
    if(!CY.w[0]) {
#ifdef BID_SET_STATUS_FLAGS
  __set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
#endif
  // x=y=0, return NaN
  res.w[1] = 0x7c00000000000000ull;
  res.w[0] = 0;
  BID_RETURN (res);
}
	   // return 0
res.w[1] = ((x) ^ (y)) & 0x8000000000000000ull;
if (((y) & 0x6000000000000000ull) == 0x6000000000000000ull)
  exponent_y = ((BID_UINT32) ((y) >> 51)) & 0x3ff;
else
  exponent_y = ((BID_UINT32) ((y) >> 53)) & 0x3ff;
exponent_x = exponent_x - exponent_y + DECIMAL_EXPONENT_BIAS_128;
res.w[1] |= (((BID_UINT64) exponent_x) << 49);
res.w[0] = 0;
BID_RETURN (res);
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
  res.w[0] = (CY.w[0] & 0x0003ffffffffffffull);
  __mul_64x64_to_128 (res, res.w[0], bid_power10_table_128[18].w[0]);
  res.w[1] |= ((CY.w[0]) & 0xfc00000000000000ull);
    BID_RETURN (res);
  }
  // y is Infinity?
  if (((y) & 0x7800000000000000ull) == 0x7800000000000000ull) {
    // return +/-0
    res.w[1] = sign_x ^ sign_y;
    res.w[0] = 0;
    BID_RETURN (res);
  }
  // y is 0, return +/-Inf
  res.w[1] =
    (((x) ^ (y)) & 0x8000000000000000ull) | 0x7800000000000000ull;
  res.w[0] = 0;
#ifdef BID_SET_STATUS_FLAGS
  __set_status_flags (pfpsf, BID_ZERO_DIVIDE_EXCEPTION);
#endif
  BID_RETURN (res);
}
#ifdef UNCHANGED_BINARY_STATUS_FLAGS
// (void) fegetexceptflag (&binaryflags, BID_FE_ALL_FLAGS);
#endif
diff_expon = exponent_x - exponent_y + DECIMAL_EXPONENT_BIAS_128;

if (__unsigned_compare_gt_128 (CY, CX)) {
  // CX < CY

  // 2^64
  f64.i = 0x5f800000;

  // fx ~ CX,   fy ~ CY
  fx.d = (float) CX.w[1] * f64.d + (float) CX.w[0];
  fy.d = (float) CY.w[1] * f64.d + (float) CY.w[0];
  // expon_cy - expon_cx
  bin_index = (fy.i - fx.i) >> 23;

  T128 = bid_power10_index_binexp_128[bin_index];
  __mul_64x128_short (CA, CX.w[0], T128);

  ed2 = 33;
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

  if (!CR.w[1] && !CR.w[0]) {
    bid_get_BID128 (&res, sign_x ^ sign_y, diff_expon, CQ, &rnd_mode,
		pfpsf);
#ifdef UNCHANGED_BINARY_STATUS_FLAGS
    // (void) fesetexceptflag (&binaryflags, BID_FE_ALL_FLAGS);
#endif
    BID_RETURN (res);
  }
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

  ed2 = 34 - digits_q;
  T128.w[0] = bid_power10_table_128[ed2].w[0];
  T128.w[1] = bid_power10_table_128[ed2].w[1];
  __mul_128x128_to_256 (CA4, CR, T128);
  diff_expon = diff_expon - ed2;
  __mul_128x128_low (CQ, CQ, T128);

}

bid___div_256_by_128 (&CQ, &CA4, CY);


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
      //__mul_128x128_to_256(P256, CQ, bid_reciprocals10_128[nzeros]);Qh.w[1]=P256.w[3];Qh.w[0]=P256.w[2];

      // now get P/10^extra_digits: shift Q_high right by M[extra_digits]-128
      amount = bid_recip_scale[nzeros];
      __shr_128_long (CQ, Qh, amount);

      diff_expon += nzeros;
    } else {
      // decompose Q as Qh*10^17 + Ql
      //T128 = bid_reciprocals10_128[17];
      T128.w[0] = 0x44909befeb9fad49ull;
      T128.w[1] = 0x000b877aa3236a4bull;
      __mul_128x128_to_256 (P256, CQ, T128);
      //amount = bid_recip_scale[17];
      Q_high = (P256.w[2] >> 44) | (P256.w[3] << (64 - 44));
      Q_low = CQ.w[0] - Q_high * 100000000000000000ull;

      if (!Q_low) {
	diff_expon += 17;

	tdigit[0] = Q_high & 0x3ffffff;
	tdigit[1] = 0;
	QX = Q_high >> 26;
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


	if (tdigit[1] >= 100000000) {
	  tdigit[1] -= 100000000;
	  if (tdigit[1] >= 100000000)
	    tdigit[1] -= 100000000;
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
	  __mul_64x64_to_128 (CQ, Q_high, bid_reciprocals10_64[nzeros]);

	  // now get P/10^extra_digits: shift C64 right by M[extra_digits]-64
	  amount = bid_short_recip_scale[nzeros];
	  CQ.w[0] = CQ.w[1] >> amount;
	} else
	  CQ.w[0] = Q_high;
	CQ.w[1] = 0;

	diff_expon += nzeros;
      } else {
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

	if (tdigit[1] >= 100000000) {
	  tdigit[1] -= 100000000;
	  if (tdigit[1] >= 100000000)
	    tdigit[1] -= 100000000;
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
    bid_get_BID128(&res, sign_x ^ sign_y, diff_expon, CQ, &rnd_mode,pfpsf);
#ifdef UNCHANGED_BINARY_STATUS_FLAGS
    // (void) fesetexceptflag (&binaryflags, BID_FE_ALL_FLAGS);
#endif
    BID_RETURN (res);
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
  if (CQ.w[0] < carry64)
    CQ.w[1]++;
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

} else {
#ifdef BID_SET_STATUS_FLAGS
  if (CA4.w[0] || CA4.w[1]) {
    // set status flags
    __set_status_flags (pfpsf, BID_INEXACT_EXCEPTION);
  }
#endif
  bid_handle_UF_128_rem (&res, sign_x ^ sign_y, diff_expon, CQ,
		     CA4.w[1] | CA4.w[0], &rnd_mode, pfpsf);
#ifdef UNCHANGED_BINARY_STATUS_FLAGS
  // (void) fesetexceptflag (&binaryflags, BID_FE_ALL_FLAGS);
#endif
  BID_RETURN (res);

}

bid_get_BID128 (&res, sign_x ^ sign_y, diff_expon, CQ, &rnd_mode, pfpsf);
#ifdef UNCHANGED_BINARY_STATUS_FLAGS
// (void) fesetexceptflag (&binaryflags, BID_FE_ALL_FLAGS);
#endif
BID_RETURN (res);
}


BID128_FUNCTION_ARGTYPE1_ARG128 (bid128dq_div, BID_UINT64, x, y)
     BID_UINT256 CA4, CA4r, P256;
     BID_UINT128 CX, CY, T128, CQ, CR, CA, TP128, Qh, Ql, res;
     BID_UINT64 sign_x, sign_y, carry64, D, Q_high, Q_low, QX, valid_y,
       PD;
     int_float fx, fy, f64;
     BID_UINT32 QX32, tdigit[3], digit, digit_h, digit_low;
     int exponent_x, exponent_y, bin_index, bin_expon, diff_expon, ed2,
       digits_q, amount;
     int nzeros, i, j, k, d5;
     unsigned rmode;

  BID_OPT_SAVE_BINARY_FLAGS()

valid_y = unpack_BID128_value (&sign_y, &exponent_y, &CY, y);

	// unpack arguments, check for NaN or Infinity
CX.w[1] = 0;
if (!unpack_BID64 (&sign_x, &exponent_x, &CX.w[0], x)) {
#ifdef BID_SET_STATUS_FLAGS
if ((y.w[1] & SNAN_MASK64) == SNAN_MASK64)	// y is sNaN
  __set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
#endif

    // test if x is NaN
if ((x & NAN_MASK64) == NAN_MASK64) {
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
  // check if y is Inf.
  if (((y.w[1] & 0x7c00000000000000ull) == 0x7800000000000000ull))
    // return NaN 
  {
#ifdef BID_SET_STATUS_FLAGS
    __set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
#endif
    res.w[1] = 0x7c00000000000000ull;
    res.w[0] = 0;
    BID_RETURN (res);
  }
  if (((y.w[1] & 0x7c00000000000000ull) != 0x7c00000000000000ull)) {
  // otherwise return +/-Inf
  res.w[1] =
    ((x ^ y.w[1]) & 0x8000000000000000ull) | 0x7800000000000000ull;
  res.w[0] = 0;
  BID_RETURN (res);
  }
}
	   // x is 0
if ((y.w[1] & INFINITY_MASK64) != INFINITY_MASK64) {
  if ((!CY.w[0]) && !(CY.w[1] & 0x0001ffffffffffffull)) {
#ifdef BID_SET_STATUS_FLAGS
    __set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
#endif
    // x=y=0, return NaN
    res.w[1] = 0x7c00000000000000ull;
    res.w[0] = 0;
    BID_RETURN (res);
  }
  // return 0
  res.w[1] = (x ^ y.w[1]) & 0x8000000000000000ull;
  exponent_x = exponent_x - exponent_y + (DECIMAL_EXPONENT_BIAS_128<<1) - DECIMAL_EXPONENT_BIAS;
  if (exponent_x > DECIMAL_MAX_EXPON_128)
    exponent_x = DECIMAL_MAX_EXPON_128;
  else if (exponent_x < 0)
    exponent_x = 0;
  res.w[1] |= (((BID_UINT64) exponent_x) << 49);
  res.w[0] = 0;
  BID_RETURN (res);
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
    res.w[1] = CY.w[1] & QUIET_MASK64;
    res.w[0] = CY.w[0];
    BID_RETURN (res);
  }
  // y is Infinity?
  if ((y.w[1] & 0x7800000000000000ull) == 0x7800000000000000ull) {
    // return +/-0
    res.w[1] = sign_x ^ sign_y;
    res.w[0] = 0;
    BID_RETURN (res);
  }
  // y is 0, return +/-Inf
  res.w[1] =
    ((x ^ y.w[1]) & 0x8000000000000000ull) | 0x7800000000000000ull;
  res.w[0] = 0;
#ifdef BID_SET_STATUS_FLAGS
  __set_status_flags (pfpsf, BID_ZERO_DIVIDE_EXCEPTION);
#endif
  BID_RETURN (res);
}
#ifdef UNCHANGED_BINARY_STATUS_FLAGS
// (void) fegetexceptflag (&binaryflags, BID_FE_ALL_FLAGS);
#endif
diff_expon = exponent_x - exponent_y + DECIMAL_EXPONENT_BIAS_128;

if (__unsigned_compare_gt_128 (CY, CX)) {
  // CX < CY

  // 2^64
  f64.i = 0x5f800000;

  // fx ~ CX,   fy ~ CY
  fx.d = (float) CX.w[1] * f64.d + (float) CX.w[0];
  fy.d = (float) CY.w[1] * f64.d + (float) CY.w[0];
  // expon_cy - expon_cx
  bin_index = (fy.i - fx.i) >> 23;

  T128 = bid_power10_index_binexp_128[bin_index];
  __mul_64x128_short (CA, CX.w[0], T128);

  ed2 = 33;
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

  if (!CR.w[1] && !CR.w[0]) {
    bid_get_BID128 (&res, sign_x ^ sign_y, diff_expon, CQ, &rnd_mode,
		pfpsf);
#ifdef UNCHANGED_BINARY_STATUS_FLAGS
    // (void) fesetexceptflag (&binaryflags, BID_FE_ALL_FLAGS);
#endif
    BID_RETURN (res);
  }
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

  ed2 = 34 - digits_q;
  T128.w[0] = bid_power10_table_128[ed2].w[0];
  T128.w[1] = bid_power10_table_128[ed2].w[1];
  __mul_128x128_to_256 (CA4, CR, T128);
  diff_expon = diff_expon - ed2;
  __mul_128x128_low (CQ, CQ, T128);

}

bid___div_256_by_128 (&CQ, &CA4, CY);

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
    //printf("ed2=%d,nz=%d,a=%d,CQ="BID_LX16","BID_LX16", RH="BID_LX16", RL="BID_LX16"\n",ed2,nzeros,amount,CQ.w[1],CQ.w[0],reciprocals10_128[nzeros].w[1],reciprocals10_128[nzeros].w[0]);fflush(stdout);
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
      T128.w[0] = 0x44909befeb9fad49ull;
      T128.w[1] = 0x000b877aa3236a4bull;
      __mul_128x128_to_256 (P256, CQ, T128);
      //amount = bid_recip_scale[17];
      Q_high = (P256.w[2] >> 44) | (P256.w[3] << (64 - 44));
      Q_low = CQ.w[0] - Q_high * 100000000000000000ull;

      if (!Q_low) {
	diff_expon += 17;

	tdigit[0] = Q_high & 0x3ffffff;
	tdigit[1] = 0;
	QX = Q_high >> 26;
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


	if (tdigit[1] >= 100000000) {
	  tdigit[1] -= 100000000;
	  if (tdigit[1] >= 100000000)
	    tdigit[1] -= 100000000;
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
	  //printf("i=%d, nz=%d, digit=%d (%d, %016I64x %016I64x)\n",i,nzeros,digit_h,digit,PD,digit_h);fflush(stdout);
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
	  __mul_64x64_to_128 (CQ, Q_high, bid_reciprocals10_64[nzeros]);

	  // now get P/10^extra_digits: shift C64 right by M[extra_digits]-64
	  amount = bid_short_recip_scale[nzeros];
	  CQ.w[0] = CQ.w[1] >> amount;
	} else
	  CQ.w[0] = Q_high;
	CQ.w[1] = 0;

	diff_expon += nzeros;
      } else {
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

	if (tdigit[1] >= 100000000) {
	  tdigit[1] -= 100000000;
	  if (tdigit[1] >= 100000000)
	    tdigit[1] -= 100000000;
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
	  //printf("i=%d, nz=%d, digit=%d (%d, %016I64x %016I64x)\n",i,nzeros,digit_h,digit,PD,digit_h);fflush(stdout);
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
    bid_get_BID128 (&res, sign_x ^ sign_y, diff_expon, CQ, &rnd_mode,
		pfpsf);
#ifdef UNCHANGED_BINARY_STATUS_FLAGS
    // (void) fesetexceptflag (&binaryflags, BID_FE_ALL_FLAGS);
#endif
    BID_RETURN (res);
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
  if (CQ.w[0] < carry64)
    CQ.w[1]++;
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

} else {
#ifdef BID_SET_STATUS_FLAGS
  if (CA4.w[0] || CA4.w[1]) {
    // set status flags
    __set_status_flags (pfpsf, BID_INEXACT_EXCEPTION);
  }
#endif
  bid_handle_UF_128_rem (&res, sign_x ^ sign_y, diff_expon, CQ,
		     CA4.w[1] | CA4.w[0], &rnd_mode, pfpsf);
#ifdef UNCHANGED_BINARY_STATUS_FLAGS
  // (void) fesetexceptflag (&binaryflags, BID_FE_ALL_FLAGS);
#endif
  BID_RETURN (res);
}

bid_get_BID128 (&res, sign_x ^ sign_y, diff_expon, CQ, &rnd_mode, pfpsf);
#ifdef UNCHANGED_BINARY_STATUS_FLAGS
// (void) fesetexceptflag (&binaryflags, BID_FE_ALL_FLAGS);
#endif
BID_RETURN (res);

}


BID128_FUNCTION_ARG128_ARGTYPE2 (bid128qd_div, x, BID_UINT64, y)
     BID_UINT256 CA4, CA4r, P256;
     BID_UINT128 CX, CY, T128, CQ, CR, CA, TP128, Qh, Ql, res;
     BID_UINT64 sign_x, sign_y, carry64, D, Q_high, Q_low, QX, PD,
       valid_y;
     int_float fx, fy, f64;
     BID_UINT32 QX32, tdigit[3], digit, digit_h, digit_low;
     int exponent_x, exponent_y, bin_index, bin_expon, diff_expon, ed2,
       digits_q, amount;
     int nzeros, i, j, k, d5, rmode;

  BID_OPT_SAVE_BINARY_FLAGS()

valid_y = unpack_BID64 (&sign_y, &exponent_y, &CY.w[0], y);
	// unpack arguments, check for NaN or Infinity
if (!unpack_BID128_value (&sign_x, &exponent_x, &CX, x)) {
    // test if x is NaN
if ((x.w[1] & 0x7c00000000000000ull) == 0x7c00000000000000ull) {
#ifdef BID_SET_STATUS_FLAGS
  if ((x.w[1] & 0x7e00000000000000ull) == 0x7e00000000000000ull ||	// sNaN
      (y & 0x7e00000000000000ull) == 0x7e00000000000000ull)
    __set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
#endif
  res.w[1] = (CX.w[1]) & QUIET_MASK64;
  res.w[0] = CX.w[0];
  BID_RETURN (res);
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
    res.w[1] = 0x7c00000000000000ull;
    res.w[0] = 0;
    BID_RETURN (res);
  }
  // y is NaN?
  if (((y & 0x7c00000000000000ull) != 0x7c00000000000000ull))
    // return NaN 
  {
    // return +/-Inf
    res.w[1] = ((x.w[1] ^ y) & 0x8000000000000000ull) |
      0x7800000000000000ull;
    res.w[0] = 0;
    BID_RETURN (res);
  }
}
    // x is 0
if ((y & 0x7800000000000000ull) < 0x7800000000000000ull) {
	if (!CY.w[0]) {
#ifdef BID_SET_STATUS_FLAGS
    __set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
#endif
    // x=y=0, return NaN
    res.w[1] = 0x7c00000000000000ull;
    res.w[0] = 0;
    BID_RETURN (res);
  }
  // return 0
  res.w[1] = (x.w[1] ^ y) & 0x8000000000000000ull;
  exponent_x = exponent_x - exponent_y + DECIMAL_EXPONENT_BIAS;
  if (exponent_x > DECIMAL_MAX_EXPON_128)
    exponent_x = DECIMAL_MAX_EXPON_128;
  else if (exponent_x < 0)
    exponent_x = 0;
  res.w[1] |= (((BID_UINT64) exponent_x) << 49);
  res.w[0] = 0;
  BID_RETURN (res);
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
  res.w[0] = (CY.w[0] & 0x0003ffffffffffffull);
  __mul_64x64_to_128 (res, res.w[0], bid_power10_table_128[18].w[0]);
  res.w[1] |= ((CY.w[0]) & 0xfc00000000000000ull);
    BID_RETURN (res);
  }
  // y is Infinity?
  if ((y & INFINITY_MASK64) == INFINITY_MASK64) {
    // return +/-0
    res.w[1] = ((x.w[1] ^ y) & 0x8000000000000000ull);
    res.w[0] = 0;
    BID_RETURN (res);
  }
  // y is 0
#ifdef BID_SET_STATUS_FLAGS
  __set_status_flags (pfpsf, BID_ZERO_DIVIDE_EXCEPTION);
#endif
  res.w[1] = (sign_x ^ sign_y) | INFINITY_MASK64;
  res.w[0] = 0;
  BID_RETURN (res);
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

  T128 = bid_power10_index_binexp_128[bin_index];
  __mul_64x128_short (CA, CX.w[0], T128);

  ed2 = 33;
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

  if (!CR.w[1] && !CR.w[0]) {
    bid_get_BID128 (&res, sign_x ^ sign_y, diff_expon, CQ, &rnd_mode,
		pfpsf);
#ifdef UNCHANGED_BINARY_STATUS_FLAGS
    // (void) fesetexceptflag (&binaryflags, BID_FE_ALL_FLAGS);
#endif
    BID_RETURN (res);
  }
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

  ed2 = 34 - digits_q;
  T128.w[0] = bid_power10_table_128[ed2].w[0];
  T128.w[1] = bid_power10_table_128[ed2].w[1];
  __mul_128x128_to_256 (CA4, CR, T128);
  diff_expon = diff_expon - ed2;
  __mul_128x128_low (CQ, CQ, T128);

}

bid___div_256_by_128 (&CQ, &CA4, CY);


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
      //__mul_128x128_to_256(P256, CQ, bid_reciprocals10_128[nzeros]);Qh.w[1]=P256.w[3];Qh.w[0]=P256.w[2];

      // now get P/10^extra_digits: shift Q_high right by M[extra_digits]-128
      amount = bid_recip_scale[nzeros];
      __shr_128_long (CQ, Qh, amount);

      diff_expon += nzeros;
    } else {
      // decompose Q as Qh*10^17 + Ql
      //T128 = bid_reciprocals10_128[17];
      T128.w[0] = 0x44909befeb9fad49ull;
      T128.w[1] = 0x000b877aa3236a4bull;
      __mul_128x128_to_256 (P256, CQ, T128);
      //amount = bid_recip_scale[17];
      Q_high = (P256.w[2] >> 44) | (P256.w[3] << (64 - 44));
      Q_low = CQ.w[0] - Q_high * 100000000000000000ull;

      if (!Q_low) {
	diff_expon += 17;

	tdigit[0] = Q_high & 0x3ffffff;
	tdigit[1] = 0;
	QX = Q_high >> 26;
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


	if (tdigit[1] >= 100000000) {
	  tdigit[1] -= 100000000;
	  if (tdigit[1] >= 100000000)
	    tdigit[1] -= 100000000;
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
	  __mul_64x64_to_128 (CQ, Q_high, bid_reciprocals10_64[nzeros]);

	  // now get P/10^extra_digits: shift C64 right by M[extra_digits]-64
	  amount = bid_short_recip_scale[nzeros];
	  CQ.w[0] = CQ.w[1] >> amount;
	} else
	  CQ.w[0] = Q_high;
	CQ.w[1] = 0;

	diff_expon += nzeros;
      } else {
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

	if (tdigit[1] >= 100000000) {
	  tdigit[1] -= 100000000;
	  if (tdigit[1] >= 100000000)
	    tdigit[1] -= 100000000;
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
    bid_get_BID128 (&res, sign_x ^ sign_y, diff_expon, CQ, &rnd_mode,pfpsf);
#ifdef UNCHANGED_BINARY_STATUS_FLAGS
    // (void) fesetexceptflag (&binaryflags, BID_FE_ALL_FLAGS);
#endif
    BID_RETURN (res);
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
  if (CQ.w[0] < carry64)
    CQ.w[1]++;
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

} else {
#ifdef BID_SET_STATUS_FLAGS
  if (CA4.w[0] || CA4.w[1]) {
    // set status flags
    __set_status_flags (pfpsf, BID_INEXACT_EXCEPTION);
  }
#endif
  bid_handle_UF_128_rem (&res, sign_x ^ sign_y, diff_expon, CQ,
		     CA4.w[1] | CA4.w[0], &rnd_mode, pfpsf);
#ifdef UNCHANGED_BINARY_STATUS_FLAGS
  // (void) fesetexceptflag (&binaryflags, BID_FE_ALL_FLAGS);
#endif
  BID_RETURN (res);

}

bid_get_BID128 (&res, sign_x ^ sign_y, diff_expon, CQ, &rnd_mode, pfpsf);
#ifdef UNCHANGED_BINARY_STATUS_FLAGS
// (void) fesetexceptflag (&binaryflags, BID_FE_ALL_FLAGS);
#endif
BID_RETURN (res);

}
