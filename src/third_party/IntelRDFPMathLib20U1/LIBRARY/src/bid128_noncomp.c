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

/*****************************************************************************
 *
 *    BID128 non-computational functions:
 *         - bid128_isSigned
 *         - bid128_isNormal
 *         - bid128_isSubnormal
 *         - bid128_isFinite
 *         - bid128_isZero
 *         - bid128_isInf
 *         - bid128_isSignaling
 *         - bid128_isCanonical
 *         - bid128_isNaN
 *         - bid128_copy
 *         - bid128_negate
 *         - bid128_abs
 *         - bid128_copySign
 *         - bid128_class
 *         - bid128_totalOrder
 *         - bid128_totalOrderMag
 *         - bid128_sameQuantum
 *         - bid128_radix
 ****************************************************************************/

#if DECIMAL_CALL_BY_REFERENCE
void
bid128_isSigned (int *pres,
		 BID_UINT128 * px _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
  BID_UINT128 x = *px;
#else
RES_WRAPFN_DFP(int, bid128_isSigned, 128)
int
bid128_isSigned (BID_UINT128 x _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
#endif
  int res;

  res = ((x.w[BID_HIGH_128W] & MASK_SIGN) == MASK_SIGN);
  BID_RETURN (res);
}

// return 1 iff x is not zero, nor NaN nor subnormal nor infinity
#if DECIMAL_CALL_BY_REFERENCE
void
bid128_isNormal (int *pres,
		 BID_UINT128 * px _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
  BID_UINT128 x = *px;
#else
RES_WRAPFN_DFP(int, bid128_isNormal, 128)
int
bid128_isNormal (BID_UINT128 x _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
#endif
  int res;
  BID_UINT64 x_exp, C1_hi, C1_lo;
  BID_UI64DOUBLE tmp1;
  int exp, q, x_nr_bits;

  BID_SWAP128 (x);
  // test for special values - infinity or NaN
  if ((x.w[1] & MASK_SPECIAL) == MASK_SPECIAL) {
    // x is special
    res = 0;
    BID_RETURN (res);
  }
  // unpack x 
  x_exp = x.w[1] & MASK_EXP;	// biased and shifted left 49 bit positions
  C1_hi = x.w[1] & MASK_COEFF;
  C1_lo = x.w[0];
  // test for zero
  if (C1_hi == 0 && C1_lo == 0) {
    res = 0;
    BID_RETURN (res);
  }
  // test for non-canonical values of the argument x
  if ((((C1_hi > 0x0001ed09bead87c0ull)
	|| ((C1_hi == 0x0001ed09bead87c0ull)
	    && (C1_lo > 0x378d8e63ffffffffull)))
       && ((x.w[1] & 0x6000000000000000ull) != 0x6000000000000000ull))
      || ((x.w[1] & 0x6000000000000000ull) == 0x6000000000000000ull)) {
    res = 0;
    BID_RETURN (res);
  }
  // x is subnormal or normal
  // determine the number of digits q in the significand
  // q = nr. of decimal digits in x
  // determine first the nr. of bits in x
  if (C1_hi == 0) {
    if (C1_lo >= 0x0020000000000000ull) {	// x >= 2^53
      // split the 64-bit value in two 32-bit halves to avoid rounding errors
	tmp1.d = (double) (C1_lo >> 32);	// exact conversion
	x_nr_bits =
	  33 + ((((unsigned int) (tmp1.ui64 >> 52)) & 0x7ff) - 0x3ff);
    } else {	// if x < 2^53
      tmp1.d = (double) C1_lo;	// exact conversion
      x_nr_bits =
	1 + ((((unsigned int) (tmp1.ui64 >> 52)) & 0x7ff) - 0x3ff);
    }
  } else {	// C1_hi != 0 => nr. bits = 64 + nr_bits (C1_hi)
    tmp1.d = (double) C1_hi;	// exact conversion
    x_nr_bits =
      65 + ((((unsigned int) (tmp1.ui64 >> 52)) & 0x7ff) - 0x3ff);
  }
  q = bid_nr_digits[x_nr_bits - 1].digits;
  if (q == 0) {
    q = bid_nr_digits[x_nr_bits - 1].digits1;
    if (C1_hi > bid_nr_digits[x_nr_bits - 1].threshold_hi ||
	(C1_hi == bid_nr_digits[x_nr_bits - 1].threshold_hi &&
	 C1_lo >= bid_nr_digits[x_nr_bits - 1].threshold_lo))
      q++;
  }
  exp = (int) (x_exp >> 49) - 6176;
  // test for subnormal values of x
  if (exp + q <= -6143) {
    res = 0;
    BID_RETURN (res);
  } else {
    res = 1;
    BID_RETURN (res);
  }
}

// return 1 iff x is not zero, nor NaN nor normal nor infinity
#if DECIMAL_CALL_BY_REFERENCE
void
bid128_isSubnormal (int *pres,
		    BID_UINT128 * px _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
  BID_UINT128 x = *px;
#else
RES_WRAPFN_DFP(int, bid128_isSubnormal, 128)
int
bid128_isSubnormal (BID_UINT128 x _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
#endif
  int res;
  BID_UINT64 x_exp, C1_hi, C1_lo;
  BID_UI64DOUBLE tmp1;
  int exp, q, x_nr_bits;

  BID_SWAP128 (x);
  // test for special values - infinity or NaN
  if ((x.w[1] & MASK_SPECIAL) == MASK_SPECIAL) {
    // x is special
    res = 0;
    BID_RETURN (res);
  }
  // unpack x 
  x_exp = x.w[1] & MASK_EXP;	// biased and shifted left 49 bit positions
  C1_hi = x.w[1] & MASK_COEFF;
  C1_lo = x.w[0];
  // test for zero
  if (C1_hi == 0 && C1_lo == 0) {
    res = 0;
    BID_RETURN (res);
  }
  // test for non-canonical values of the argument x
  if ((((C1_hi > 0x0001ed09bead87c0ull)
	|| ((C1_hi == 0x0001ed09bead87c0ull)
	    && (C1_lo > 0x378d8e63ffffffffull)))
       && ((x.w[1] & 0x6000000000000000ull) != 0x6000000000000000ull))
      || ((x.w[1] & 0x6000000000000000ull) == 0x6000000000000000ull)) {
    res = 0;
    BID_RETURN (res);
  }
  // x is subnormal or normal
  // determine the number of digits q in the significand
  // q = nr. of decimal digits in x
  // determine first the nr. of bits in x
  if (C1_hi == 0) {
    if (C1_lo >= 0x0020000000000000ull) {	// x >= 2^53
      // split the 64-bit value in two 32-bit halves to avoid rounding errors
	tmp1.d = (double) (C1_lo >> 32);	// exact conversion
	x_nr_bits =
	  33 + ((((unsigned int) (tmp1.ui64 >> 52)) & 0x7ff) - 0x3ff);
    } else {	// if x < 2^53
      tmp1.d = (double) C1_lo;	// exact conversion
      x_nr_bits =
	1 + ((((unsigned int) (tmp1.ui64 >> 52)) & 0x7ff) - 0x3ff);
    }
  } else {	// C1_hi != 0 => nr. bits = 64 + nr_bits (C1_hi)
    tmp1.d = (double) C1_hi;	// exact conversion
    x_nr_bits =
      65 + ((((unsigned int) (tmp1.ui64 >> 52)) & 0x7ff) - 0x3ff);
  }
  q = bid_nr_digits[x_nr_bits - 1].digits;
  if (q == 0) {
    q = bid_nr_digits[x_nr_bits - 1].digits1;
    if (C1_hi > bid_nr_digits[x_nr_bits - 1].threshold_hi ||
	(C1_hi == bid_nr_digits[x_nr_bits - 1].threshold_hi &&
	 C1_lo >= bid_nr_digits[x_nr_bits - 1].threshold_lo))
      q++;
  }
  exp = (int) (x_exp >> 49) - 6176;
  // test for subnormal values of x
  if (exp + q <= -6143) {
    res = 1;
  } else {
    res = 0;
  }
  BID_RETURN (res);
}

#if DECIMAL_CALL_BY_REFERENCE
void
bid128_isFinite (int *pres,
		 BID_UINT128 * px _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
  BID_UINT128 x = *px;
#else
RES_WRAPFN_DFP(int, bid128_isFinite, 128)
int
bid128_isFinite (BID_UINT128 x _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
#endif
  int res;
  res = ((x.w[BID_HIGH_128W] & MASK_INF) != MASK_INF);
  BID_RETURN (res);
}

#if DECIMAL_CALL_BY_REFERENCE
void
bid128_isZero (int *pres, BID_UINT128 * px _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
  BID_UINT128 x = *px;
#else
RES_WRAPFN_DFP(int, bid128_isZero, 128)
int
bid128_isZero (BID_UINT128 x _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
#endif
  int res;
  BID_UINT128 sig_x;

  BID_SWAP128 (x);
  if ((x.w[1] & MASK_INF) == MASK_INF) {
    res = 0;
    BID_RETURN (res);
  }
  sig_x.w[1] = x.w[1] & 0x0001ffffffffffffull;
  sig_x.w[0] = x.w[0];
  if ((sig_x.w[1] > 0x0001ed09bead87c0ull) ||	// significand is non-canonical
      ((sig_x.w[1] == 0x0001ed09bead87c0ull) && (sig_x.w[0] > 0x378d8e63ffffffffull)) ||	// significand is non-canonical
      ((x.w[1] & 0x6000000000000000ull) == 0x6000000000000000ull) ||
	// significand is non-canonical
      (sig_x.w[1] == 0 && sig_x.w[0] == 0)) {	// significand is 0
    res = 1;
    BID_RETURN (res);
  }
  res = 0;
  BID_RETURN (res);
}

#if DECIMAL_CALL_BY_REFERENCE
void
bid128_isInf (int *pres, BID_UINT128 * px _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
  BID_UINT128 x = *px;
#else
RES_WRAPFN_DFP(int, bid128_isInf, 128)
int
bid128_isInf (BID_UINT128 x _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
#endif
  int res;
  res = ((x.w[BID_HIGH_128W] & MASK_INF) == MASK_INF)
    && ((x.w[BID_HIGH_128W] & MASK_NAN) != MASK_NAN);
  BID_RETURN (res);
}

#if DECIMAL_CALL_BY_REFERENCE
void
bid128_isSignaling (int *pres,
		    BID_UINT128 * px _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
  BID_UINT128 x = *px;
#else
RES_WRAPFN_DFP(int, bid128_isSignaling, 128)
int
bid128_isSignaling (BID_UINT128 x _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
#endif
  int res;

  res = ((x.w[BID_HIGH_128W] & MASK_SNAN) == MASK_SNAN);
  BID_RETURN (res);
}

// return 1 iff x is a canonical number ,infinity, or NaN.
#if DECIMAL_CALL_BY_REFERENCE
void
bid128_isCanonical (int *pres,
		    BID_UINT128 * px _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
  BID_UINT128 x = *px;
#else
RES_WRAPFN_DFP(int, bid128_isCanonical, 128)
int
bid128_isCanonical (BID_UINT128 x _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
#endif
  int res;
  BID_UINT128 sig_x;

  BID_SWAP128 (x);
  if ((x.w[1] & MASK_NAN) == MASK_NAN) {	// NaN
    if (x.w[1] & 0x01ffc00000000000ull) {
      res = 0;
      BID_RETURN (res);
    }
    sig_x.w[1] = x.w[1] & 0x00003fffffffffffull;	// 46 bits
    sig_x.w[0] = x.w[0];	// 64 bits
    // payload must be < 10^33 = 0x0000314dc6448d93_38c15b0a00000000
    if (sig_x.w[1] < 0x0000314dc6448d93ull
	|| (sig_x.w[1] == 0x0000314dc6448d93ull
	    && sig_x.w[0] < 0x38c15b0a00000000ull)) {
      res = 1;
    } else {
      res = 0;
    }
    BID_RETURN (res);
  } else if ((x.w[1] & MASK_INF) == MASK_INF) {	// infinity
    if ((x.w[1] & 0x03ffffffffffffffull) || x.w[0]) {
      res = 0;
    } else {
      res = 1;
    }
    BID_RETURN (res);
  }
  // not NaN or infinity; extract significand to ensure it is canonical
  sig_x.w[1] = x.w[1] & 0x0001ffffffffffffull;
  sig_x.w[0] = x.w[0];
  // a canonical number has a coefficient < 10^34 
  //    (0x0001ed09_bead87c0_378d8e64_00000000)
  if ((sig_x.w[1] > 0x0001ed09bead87c0ull) ||	// significand is non-canonical
      ((sig_x.w[1] == 0x0001ed09bead87c0ull) && (sig_x.w[0] > 0x378d8e63ffffffffull)) ||	// significand is non-canonical
      ((x.w[1] & 0x6000000000000000ull) == 0x6000000000000000ull)) {
    res = 0;
  } else {
    res = 1;
  }
  BID_RETURN (res);
}

#if DECIMAL_CALL_BY_REFERENCE
void
bid128_isNaN (int *pres, BID_UINT128 * px _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
  BID_UINT128 x = *px;
#else
RES_WRAPFN_DFP(int, bid128_isNaN, 128)
int
bid128_isNaN (BID_UINT128 x _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
#endif
  int res;

  res = ((x.w[BID_HIGH_128W] & MASK_NAN) == MASK_NAN);
  BID_RETURN (res);
}

// copies a floating-point operand x to destination y, with no change
#if DECIMAL_CALL_BY_REFERENCE
void
bid128_copy (BID_UINT128 * pres,
	     BID_UINT128 * px _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
  BID_UINT128 x = *px;
#else
DFP_WRAPFN_DFP(128, bid128_copy, 128)
BID_UINT128
bid128_copy (BID_UINT128 x _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
#endif
  BID_UINT128 res;

  res = x;
  BID_RETURN (res);
}

// copies a floating-point operand x to destination y, reversing the sign
#if DECIMAL_CALL_BY_REFERENCE
void
bid128_negate (BID_UINT128 * pres,
	       BID_UINT128 * px _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
  BID_UINT128 x = *px;
#else
DFP_WRAPFN_DFP(128, bid128_negate, 128)
BID_UINT128
bid128_negate (BID_UINT128 x _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
#endif
  BID_UINT128 res;

  x.w[BID_HIGH_128W] ^= MASK_SIGN;
  res = x;
  BID_RETURN (res);
}

// copies a floating-point operand x to destination y, changing the sign to positive
#if DECIMAL_CALL_BY_REFERENCE
void
bid128_abs (BID_UINT128 * pres,
	    BID_UINT128 * px _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
  BID_UINT128 x = *px;
#else
DFP_WRAPFN_DFP(128, bid128_abs, 128)
BID_UINT128
bid128_abs (BID_UINT128 x _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
#endif
  BID_UINT128 res;

  x.w[BID_HIGH_128W] &= ~MASK_SIGN;
  res = x;
  BID_RETURN (res);
}

// copies operand x to destination in the same format as x, but with the sign of y
#if DECIMAL_CALL_BY_REFERENCE
void
bid128_copySign (BID_UINT128 * pres, BID_UINT128 * px,
		 BID_UINT128 * py _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
  BID_UINT128 x = *px;
  BID_UINT128 y = *py;
#else
DFP_WRAPFN_DFP_DFP(128, bid128_copySign, 128, 128)
BID_UINT128
bid128_copySign (BID_UINT128 x, BID_UINT128 y _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
#endif
  BID_UINT128 res;

  x.w[BID_HIGH_128W] =
    (x.w[BID_HIGH_128W] & ~MASK_SIGN) | (y.w[BID_HIGH_128W] & MASK_SIGN);
  res = x;
  BID_RETURN (res);
}

#if DECIMAL_CALL_BY_REFERENCE
void
bid128_class (int *pres, BID_UINT128 * px _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
  BID_UINT128 x = *px;
#else
RES_WRAPFN_DFP(int, bid128_class, 128)
int
bid128_class (BID_UINT128 x _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
#endif
  int res;
  BID_UINT256 sig_x_prime256;
  BID_UINT192 sig_x_prime192;
  BID_UINT128 sig_x;
  int exp_x;

  BID_SWAP128 (x);
  if ((x.w[1] & MASK_NAN) == MASK_NAN) {
    if ((x.w[1] & MASK_SNAN) == MASK_SNAN) {
      res = signalingNaN;
    } else {
      res = quietNaN;
    }
    BID_RETURN (res);
  }
  if ((x.w[1] & MASK_INF) == MASK_INF) {
    if ((x.w[1] & MASK_SIGN) == MASK_SIGN) {
      res = negativeInfinity;
    } else {
      res = positiveInfinity;
    }
    BID_RETURN (res);
  }
  // decode number into exponent and significand
  sig_x.w[1] = x.w[1] & 0x0001ffffffffffffull;
  sig_x.w[0] = x.w[0];
  // check for zero or non-canonical
  if ((sig_x.w[1] > 0x0001ed09bead87c0ull)
      || ((sig_x.w[1] == 0x0001ed09bead87c0ull)
	  && (sig_x.w[0] > 0x378d8e63ffffffffull))
      || ((x.w[1] & 0x6000000000000000ull) == 0x6000000000000000ull)
      || ((sig_x.w[1] == 0) && (sig_x.w[0] == 0))) {
    if ((x.w[1] & MASK_SIGN) == MASK_SIGN) {
      res = negativeZero;
    } else {
      res = positiveZero;
    }
    BID_RETURN (res);
  }
  exp_x = (x.w[1] >> 49) & 0x000000000003fffull;
  // if exponent is less than -6176, the number may be subnormal 
  // (less than the smallest normal value)
  //  the smallest normal value is 1 x 10^-6143 = 10^33 x 10^-6176
  //  if (exp_x - 6176 < -6143)
  if (exp_x < 33) {	// sig_x * 10^exp_x
    if (exp_x > 19) {
      __mul_128x128_to_256 (sig_x_prime256, sig_x,
			    bid_ten2k128[exp_x - 20]);
      // 10^33 = 0x0000314dc6448d93_38c15b0a00000000
      if ((sig_x_prime256.w[3] == 0) && (sig_x_prime256.w[2] == 0)
	  && ((sig_x_prime256.w[1] < 0x0000314dc6448d93ull)
	      || ((sig_x_prime256.w[1] == 0x0000314dc6448d93ull)
		  && (sig_x_prime256.w[0] < 0x38c15b0a00000000ull)))) {
	res = ((x.w[1] & MASK_SIGN) == MASK_SIGN) ? negativeSubnormal :
	  positiveSubnormal;
	BID_RETURN (res);
      }
    } else {
      __mul_64x128_to_192 (sig_x_prime192, bid_ten2k64[exp_x], sig_x);
      // 10^33 = 0x0000314dc6448d93_38c15b0a00000000
      if ((sig_x_prime192.w[2] == 0)
	  && ((sig_x_prime192.w[1] < 0x0000314dc6448d93ull)
	      || ((sig_x_prime192.w[1] == 0x0000314dc6448d93ull)
		  && (sig_x_prime192.w[0] < 0x38c15b0a00000000ull)))) {
	res = ((x.w[1] & MASK_SIGN) == MASK_SIGN) ? negativeSubnormal :
	  positiveSubnormal;
	BID_RETURN (res);
      }
    }
  }
  // otherwise, normal number, determine the sign
  res =
    ((x.w[1] & MASK_SIGN) ==
     MASK_SIGN) ? negativeNormal : positiveNormal;
  BID_RETURN (res);
}

// true if the exponents of x and y are the same, false otherwise.
// The special cases of sameQuantum(NaN, NaN) and sameQuantum(Inf, Inf) are true
// If exactly one operand is infinite or exactly one operand is NaN, then false
#if DECIMAL_CALL_BY_REFERENCE
void
bid128_sameQuantum (int *pres, BID_UINT128 * px,
		    BID_UINT128 * py _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
  BID_UINT128 x = *px;
  BID_UINT128 y = *py;
#else
RES_WRAPFN_DFP_DFP(int, bid128_sameQuantum, 128, 128)
int
bid128_sameQuantum (BID_UINT128 x,
		    BID_UINT128 y _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
#endif
  int res;
  BID_UINT64 x_exp, y_exp;

  BID_SWAP128 (x);
  BID_SWAP128 (y);
  // if both operands are NaN, return true
  if ((x.w[1] & MASK_NAN) == MASK_NAN
      || ((y.w[1] & MASK_NAN) == MASK_NAN)) {
    res = ((x.w[1] & MASK_NAN) == MASK_NAN
	   && (y.w[1] & MASK_NAN) == MASK_NAN);
    BID_RETURN (res);
  }
  // if both operands are INF, return true
  if ((x.w[1] & MASK_INF) == MASK_INF
      || (y.w[1] & MASK_INF) == MASK_INF) {
    res = ((x.w[1] & MASK_INF) == MASK_INF)
      && ((y.w[1] & MASK_INF) == MASK_INF);
    BID_RETURN (res);
  }
  // decode exponents for both numbers, and return true if they match
  if ((x.w[1] & 0x6000000000000000ull) == 0x6000000000000000ull) {	// G0_G1=11
    x_exp = (x.w[1] << 2) & MASK_EXP;	// biased and shifted left 49 bits
  } else {	// G0_G1 != 11
    x_exp = x.w[1] & MASK_EXP;	// biased and shifted left 49 bits
  }
  if ((y.w[1] & 0x6000000000000000ull) == 0x6000000000000000ull) {	// G0_G1=11
    y_exp = (y.w[1] << 2) & MASK_EXP;	// biased and shifted left 49 bits
  } else {	// G0_G1 != 11
    y_exp = y.w[1] & MASK_EXP;	// biased and shifted left 49 bits
  }
  res = (x_exp == y_exp);
  BID_RETURN (res);
}

#if DECIMAL_CALL_BY_REFERENCE
void
bid128_totalOrder (int *pres, BID_UINT128 * px,
		   BID_UINT128 * py _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
  BID_UINT128 x = *px;
  BID_UINT128 y = *py;
#else
RES_WRAPFN_DFP_DFP(int, bid128_totalOrder, 128, 128)
int
bid128_totalOrder (BID_UINT128 x,
		   BID_UINT128 y _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
#endif
  int res;
  int exp_x, exp_y;
  BID_UINT128 sig_x, sig_y, pyld_y, pyld_x;
  BID_UINT192 sig_n_prime192;
  BID_UINT256 sig_n_prime256;
  char x_is_zero = 0, y_is_zero = 0;

  BID_SWAP128 (x);
  BID_SWAP128 (y);
  // NaN (CASE 1)
  // if x and y are unordered numerically because either operand is NaN
  //    (1) totalOrder(-NaN, number) is true
  //    (2) totalOrder(number, +NaN) is true
  //    (3) if x and y are both NaN:
  //       i) negative sign bit < positive sign bit
  //       ii) signaling < quiet for +NaN, reverse for -NaN
  //       iii) lesser payload < greater payload for +NaN (reverse for -NaN)
  //       iv) else if bitwise identical (in canonical form), return 1
  if ((x.w[1] & MASK_NAN) == MASK_NAN) {
    // if x is -NaN
    if ((x.w[1] & MASK_SIGN) == MASK_SIGN) {
      // return true, unless y is -NaN also
      if ((y.w[1] & MASK_NAN) != MASK_NAN
	  || (y.w[1] & MASK_SIGN) != MASK_SIGN) {
	res = 1;	// y is a number, return 1
	BID_RETURN (res);
      } else {	// if y and x are both -NaN
	pyld_x.w[1] = x.w[1] & 0x00003fffffffffffull;
	pyld_x.w[0] = x.w[0];
	pyld_y.w[1] = y.w[1] & 0x00003fffffffffffull;
	pyld_y.w[0] = y.w[0];
	if ((pyld_x.w[1] > 0x0000314dc6448d93ull)
	    || ((pyld_x.w[1] == 0x0000314dc6448d93ull)
		&& (pyld_x.w[0] > 0x38c15b09ffffffffull))) {
	  pyld_x.w[1] = 0;
	  pyld_x.w[0] = 0;
	}
	if ((pyld_y.w[1] > 0x0000314dc6448d93ull)
	    || ((pyld_y.w[1] == 0x0000314dc6448d93ull)
		&& (pyld_y.w[0] > 0x38c15b09ffffffffull))) {
	  pyld_y.w[1] = 0;
	  pyld_y.w[0] = 0;
	}
	// if x and y are both -SNaN or both -QNaN, we have to compare payloads
	// this statement evaluates to true if both are SNaN or QNaN
	if (!
	    (((y.w[1] & MASK_SNAN) == MASK_SNAN) ^
	     ((x.w[1] & MASK_SNAN) == MASK_SNAN))) {
	  // it comes down to the payload.  we want to return true if x has a
	  // larger payload, or if the payloads are equal (canonical forms
	  // are bitwise identical)
	  if ((pyld_x.w[1] > pyld_y.w[1]) ||
	      ((pyld_x.w[1] == pyld_y.w[1])
	       && (pyld_x.w[0] >= pyld_y.w[0])))
	    res = 1;
	  else
	    res = 0;
	  BID_RETURN (res);
	} else {
	  // either x = -SNaN and y = -QNaN or x = -QNaN and y = -SNaN
	  res = ((y.w[1] & MASK_SNAN) == MASK_SNAN);
	  // totalOrder (-QNaN, -SNaN) == 1
	  BID_RETURN (res);
	}
      }
    } else {	// x is +NaN
      // return false, unless y is +NaN also
      if ((y.w[1] & MASK_NAN) != MASK_NAN
	  || (y.w[1] & MASK_SIGN) == MASK_SIGN) {
	res = 0;	// y is a number, return 1
	BID_RETURN (res);
      } else {
	// x and y are both +NaN; 
	pyld_x.w[1] = x.w[1] & 0x00003fffffffffffull;
	pyld_x.w[0] = x.w[0];
	pyld_y.w[1] = y.w[1] & 0x00003fffffffffffull;
	pyld_y.w[0] = y.w[0];
	if ((pyld_x.w[1] > 0x0000314dc6448d93ull)
	    || ((pyld_x.w[1] == 0x0000314dc6448d93ull)
		&& (pyld_x.w[0] > 0x38c15b09ffffffffull))) {
	  pyld_x.w[1] = 0;
	  pyld_x.w[0] = 0;
	}
	if ((pyld_y.w[1] > 0x0000314dc6448d93ull)
	    || ((pyld_y.w[1] == 0x0000314dc6448d93ull)
		&& (pyld_y.w[0] > 0x38c15b09ffffffffull))) {
	  pyld_y.w[1] = 0;
	  pyld_y.w[0] = 0;
	}
	// if x and y are both +SNaN or both +QNaN, we have to compare payloads
	// this statement evaluates to true if both are SNaN or QNaN
	if (!
	    (((y.w[1] & MASK_SNAN) == MASK_SNAN) ^
	     ((x.w[1] & MASK_SNAN) == MASK_SNAN))) {
	  // it comes down to the payload.  we want to return true if x has a
	  // smaller payload, or if the payloads are equal (canonical forms
	  // are bitwise identical)
	  if ((pyld_x.w[1] < pyld_y.w[1]) ||
	      ((pyld_x.w[1] == pyld_y.w[1])
	       && (pyld_x.w[0] <= pyld_y.w[0])))
	    res = 1;
	  else
	    res = 0;
	  BID_RETURN (res);
	} else {
	  // either x = SNaN and y = QNaN or x = QNaN and y = SNaN
	  res = ((x.w[1] & MASK_SNAN) == MASK_SNAN);
	  // totalOrder (-QNaN, -SNaN) == 1
	  BID_RETURN (res);
	}
      }
    }
  } else if ((y.w[1] & MASK_NAN) == MASK_NAN) {
    // x is certainly not NAN in this case.
    // return true if y is positive
    res = ((y.w[1] & MASK_SIGN) != MASK_SIGN);
    BID_RETURN (res);
  }
  // SIMPLE (CASE 2)
  // if all the bits are the same, the numbers are equal.
  if ((x.w[1] == y.w[1]) && (x.w[0] == y.w[0])) {
    res = 1;
    BID_RETURN (res);
  }
  // OPPOSITE SIGNS (CASE 3)
  // if signs are opposite, return 1 if x is negative 
  // (if x < y, totalOrder is true)
  if (((x.w[1] & MASK_SIGN) == MASK_SIGN) ^ ((y.w[1] & MASK_SIGN) ==
					     MASK_SIGN)) {
    res = ((x.w[1] & MASK_SIGN) == MASK_SIGN);
    BID_RETURN (res);
  }
  // INFINITY (CASE 4)
  if ((x.w[1] & MASK_INF) == MASK_INF) {
    // if x == neg_inf, return (y == neg_inf);
    if ((x.w[1] & MASK_SIGN) == MASK_SIGN) {
      res = 1;
      BID_RETURN (res);
    } else {
      // x is positive infinity, only return1 if y is positive infinity as well
      res = ((y.w[1] & MASK_INF) == MASK_INF);
      BID_RETURN (res);
      // && (y & MASK_SIGN) != MASK_SIGN); (we know y has same sign as x)
    }
  } else if ((y.w[1] & MASK_INF) == MASK_INF) {
    // x is finite, so:
    //    if y is +inf, x<y
    //    if y is -inf, x>y
    res = ((y.w[1] & MASK_SIGN) != MASK_SIGN);
    BID_RETURN (res);
  }
  // CONVERT x
  sig_x.w[1] = x.w[1] & 0x0001ffffffffffffull;
  sig_x.w[0] = x.w[0];
  exp_x = (x.w[1] >> 49) & 0x000000000003fffull;

  // CHECK IF x IS CANONICAL
  // 9999999999999999999999999999999999 (decimal) = 
  //     1ed09_bead87c0_378d8e63_ffffffff(hexadecimal)
  // [0, 10^34) is the 754 supported canonical range.  
  // If the value exceeds that, it is interpreted as 0.
  if ((((sig_x.w[1] > 0x0001ed09bead87c0ull) ||
	((sig_x.w[1] == 0x0001ed09bead87c0ull) &&
	 (sig_x.w[0] > 0x378d8e63ffffffffull))) &&
       ((x.w[1] & 0x6000000000000000ull) != 0x6000000000000000ull)) ||
      ((x.w[1] & 0x6000000000000000ull) == 0x6000000000000000ull) ||
      ((sig_x.w[1] == 0) && (sig_x.w[0] == 0))) {
    x_is_zero = 1;
    // check for the case where the exponent is shifted right by 2 bits!
    if ((x.w[1] & 0x6000000000000000ull) == 0x6000000000000000ull) {
      exp_x = (x.w[1] >> 47) & 0x000000000003fffull;
    }
  }
  // CONVERT y
  exp_y = (y.w[1] >> 49) & 0x0000000000003fffull;
  sig_y.w[1] = y.w[1] & 0x0001ffffffffffffull;
  sig_y.w[0] = y.w[0];

  // CHECK IF y IS CANONICAL
  // 9999999999999999999999999999999999(decimal) = 
  //     1ed09_bead87c0_378d8e63_ffffffff(hexadecimal)
  // [0, 10^34) is the 754 supported canonical range.  
  // If the value exceeds that, it is interpreted as 0.
  if ((((sig_y.w[1] > 0x0001ed09bead87c0ull) ||
	((sig_y.w[1] == 0x0001ed09bead87c0ull) &&
	 (sig_y.w[0] > 0x378d8e63ffffffffull))) &&
       ((y.w[1] & 0x6000000000000000ull) != 0x6000000000000000ull)) ||
      ((y.w[1] & 0x6000000000000000ull) == 0x6000000000000000ull) ||
      ((sig_y.w[1] == 0) && (sig_y.w[0] == 0))) {
    y_is_zero = 1;
    // check for the case where the exponent is shifted right by 2 bits!
    if ((y.w[1] & 0x6000000000000000ull) == 0x6000000000000000ull) {
      exp_y = (y.w[1] >> 47) & 0x000000000003fffull;
    }
  }
  // ZERO (CASE 5)
  // if x and y represent the same entities, and both are negative 
  // return true iff exp_x <= exp_y
  if (x_is_zero && y_is_zero) {
    // we know that signs must be the same because we would have caught it 
    // in case3 if signs were different
    // totalOrder(x,y) iff exp_x >= exp_y for negative numbers
    // totalOrder(x,y) iff exp_x <= exp_y for positive numbers
    if (exp_x == exp_y) {
      res = 1;
      BID_RETURN (res);
    }
    res = ((exp_x <= exp_y) ^ ((x.w[1] & MASK_SIGN) == MASK_SIGN));
    BID_RETURN (res);
  }
  // if x is zero and y isn't, clearly x has the smaller payload
  if (x_is_zero) {
    res = ((y.w[1] & MASK_SIGN) != MASK_SIGN);
    BID_RETURN (res);
  }
  // if y is zero, and x isn't, clearly y has the smaller payload
  if (y_is_zero) {
    res = ((x.w[1] & MASK_SIGN) == MASK_SIGN);
    BID_RETURN (res);
  }
  // REDUNDANT REPRESENTATIONS (CASE 6)
  // if both components are either bigger or smaller
  if (((sig_x.w[1] > sig_y.w[1])
       || (sig_x.w[1] == sig_y.w[1] && sig_x.w[0] > sig_y.w[0]))
      && exp_x >= exp_y) {
    res = ((x.w[1] & MASK_SIGN) == MASK_SIGN);
    BID_RETURN (res);
  }
  if (((sig_x.w[1] < sig_y.w[1])
       || (sig_x.w[1] == sig_y.w[1] && sig_x.w[0] < sig_y.w[0]))
      && exp_x <= exp_y) {
    res = ((x.w[1] & MASK_SIGN) != MASK_SIGN);
    BID_RETURN (res);
  }
  // if |exp_x - exp_y| < 33, it comes down to the compensated significand
  if (exp_x > exp_y) {
    // if exp_x is 33 greater than exp_y, it is definitely larger, 
    // so no need for compensation
    if (exp_x - exp_y > 33) {
      res = ((x.w[1] & MASK_SIGN) == MASK_SIGN);
      BID_RETURN (res);
      // difference cannot be greater than 10^33
    }
    // otherwise adjust the x significand upwards
    if (exp_x - exp_y > 19) {
      __mul_128x128_to_256 (sig_n_prime256, sig_x,
			    bid_ten2k128[exp_x - exp_y - 20]);
      // the compensated significands are equal (ie "x and y represent the same
      // entities") return 1 if (negative && expx > expy) || 
      // (positive && expx < expy)
      if ((sig_n_prime256.w[3] == 0) && (sig_n_prime256.w[2] == 0)
	  && (sig_n_prime256.w[1] == sig_y.w[1])
	  && (sig_n_prime256.w[0] == sig_y.w[0])) {
	// the case exp_x == exp_y  cannot occur, because all bits must be 
	// the same - would have been caught if (x == y)
	res = ((exp_x <= exp_y) ^ ((x.w[1] & MASK_SIGN) == MASK_SIGN));
	BID_RETURN (res);
      }
      // if positive, return 1 if adjusted x is smaller than y
      res = (((sig_n_prime256.w[3] == 0) && (sig_n_prime256.w[2] == 0)
	      && ((sig_n_prime256.w[1] < sig_y.w[1])
		  || (sig_n_prime256.w[1] == sig_y.w[1]
		      && sig_n_prime256.w[0] <
		      sig_y.w[0]))) ^ ((x.w[1] & MASK_SIGN) ==
				       MASK_SIGN));
      BID_RETURN (res);
    }
    __mul_64x128_to_192 (sig_n_prime192, bid_ten2k64[exp_x - exp_y], sig_x);
    // if positive, return whichever significand is larger 
    // (converse if negative)
    if ((sig_n_prime192.w[2] == 0) && sig_n_prime192.w[1] == sig_y.w[1]
	&& (sig_n_prime192.w[0] == sig_y.w[0])) {
      res = ((exp_x <= exp_y) ^ ((x.w[1] & MASK_SIGN) == MASK_SIGN));
      BID_RETURN (res);
    }
    res = (((sig_n_prime192.w[2] == 0)
	    && ((sig_n_prime192.w[1] < sig_y.w[1])
		|| (sig_n_prime192.w[1] == sig_y.w[1]
		    && sig_n_prime192.w[0] <
		    sig_y.w[0]))) ^ ((x.w[1] & MASK_SIGN) ==
				     MASK_SIGN));
    BID_RETURN (res);
  }
  // if exp_x is 33 less than exp_y, it is definitely smaller, 
  // no need for compensation
  if (exp_y - exp_x > 33) {
    res = ((x.w[1] & MASK_SIGN) != MASK_SIGN);
    BID_RETURN (res);
  }
  if (exp_y - exp_x > 19) {
    // adjust the y significand upwards
    __mul_128x128_to_256 (sig_n_prime256, sig_y,
			  bid_ten2k128[exp_y - exp_x - 20]);
    // if x and y represent the same entities and both are negative
    // return true iff exp_x <= exp_y
    if ((sig_n_prime256.w[3] == 0) && (sig_n_prime256.w[2] == 0)
	&& (sig_n_prime256.w[1] == sig_x.w[1])
	&& (sig_n_prime256.w[0] == sig_x.w[0])) {
      res = (exp_x <= exp_y) ^ ((x.w[1] & MASK_SIGN) == MASK_SIGN);
      BID_RETURN (res);
    }
    // values are not equal, for positive numbers return 1 if x is less than y
    // and 0 otherwise
    res = (((sig_n_prime256.w[3] != 0) ||
	    // if upper128 bits of compensated y are non-zero, y is bigger
	    (sig_n_prime256.w[2] != 0) ||
	    // if upper128 bits of compensated y are non-zero, y is bigger
	    (sig_n_prime256.w[1] > sig_x.w[1]) ||
	    // if compensated y is bigger, y is bigger
	    (sig_n_prime256.w[1] == sig_x.w[1]
	     && sig_n_prime256.w[0] >
	     sig_x.w[0])) ^ ((x.w[1] & MASK_SIGN) == MASK_SIGN));
    BID_RETURN (res);
  }
  __mul_64x128_to_192 (sig_n_prime192, bid_ten2k64[exp_y - exp_x], sig_y);
  if ((sig_n_prime192.w[2] == 0) && (sig_n_prime192.w[1] == sig_x.w[1])
      && (sig_n_prime192.w[0] == sig_x.w[0])) {
    res = (exp_x <= exp_y) ^ ((x.w[1] & MASK_SIGN) == MASK_SIGN);
    BID_RETURN (res);
  }
  res = (((sig_n_prime192.w[2] != 0) ||
	  // if upper128 bits of compensated y are non-zero, y is bigger
	  (sig_n_prime192.w[1] > sig_x.w[1]) ||
	  // if compensated y is bigger, y is bigger
	  (sig_n_prime192.w[1] == sig_x.w[1]
	   && sig_n_prime192.w[0] >
	   sig_x.w[0])) ^ ((x.w[1] & MASK_SIGN) == MASK_SIGN));
  BID_RETURN (res);
}

#if DECIMAL_CALL_BY_REFERENCE
void
bid128_totalOrderMag (int *pres, BID_UINT128 * px,
		      BID_UINT128 * py _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
  BID_UINT128 x = *px;
  BID_UINT128 y = *py;
#else
RES_WRAPFN_DFP_DFP(int, bid128_totalOrderMag, 128, 128)
int
bid128_totalOrderMag (BID_UINT128 x,
		      BID_UINT128 y _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
#endif
  int res;
  int exp_x, exp_y;
  BID_UINT128 sig_x, sig_y, pyld_y, pyld_x;
  BID_UINT192 sig_n_prime192;
  BID_UINT256 sig_n_prime256;
  char x_is_zero = 0, y_is_zero = 0;

  BID_SWAP128 (x);
  BID_SWAP128 (y);
  x.w[1] = x.w[1] & 0x7fffffffffffffffull;
  y.w[1] = y.w[1] & 0x7fffffffffffffffull;

  // NaN (CASE 1)
  // if x and y are unordered numerically because either operand is NaN
  //    (1) totalOrder(number, +NaN) is true
  //    (2) if x and y are both NaN:
  //       i) signaling < quiet for +NaN
  //       ii) lesser payload < greater payload for +NaN
  //       iii) else if bitwise identical (in canonical form), return 1
  if ((x.w[1] & MASK_NAN) == MASK_NAN) {
    // x is +NaN
    // return false, unless y is +NaN also
    if ((y.w[1] & MASK_NAN) != MASK_NAN) {
      res = 0;	// y is a number, return 0
      BID_RETURN (res);
    } else {
      // x and y are both +NaN; 
      pyld_x.w[1] = x.w[1] & 0x00003fffffffffffull;
      pyld_x.w[0] = x.w[0];
      pyld_y.w[1] = y.w[1] & 0x00003fffffffffffull;
      pyld_y.w[0] = y.w[0];
      if ((pyld_x.w[1] > 0x0000314dc6448d93ull)
	  || ((pyld_x.w[1] == 0x0000314dc6448d93ull)
	      && (pyld_x.w[0] > 0x38c15b09ffffffffull))) {
	pyld_x.w[1] = 0;
	pyld_x.w[0] = 0;
      }
      if ((pyld_y.w[1] > 0x0000314dc6448d93ull)
	  || ((pyld_y.w[1] == 0x0000314dc6448d93ull)
	      && (pyld_y.w[0] > 0x38c15b09ffffffffull))) {
	pyld_y.w[1] = 0;
	pyld_y.w[0] = 0;
      }
      // if x and y are both +SNaN or both +QNaN, we have to compare payloads
      // this statement evaluates to true if both are SNaN or QNaN
      if (!
	  (((y.w[1] & MASK_SNAN) == MASK_SNAN) ^
	   ((x.w[1] & MASK_SNAN) == MASK_SNAN))) {
	// it comes down to the payload.  we want to return true if x has a
	// smaller payload, or if the payloads are equal (canonical forms
	// are bitwise identical)
	if ((pyld_x.w[1] < pyld_y.w[1]) ||
	    ((pyld_x.w[1] == pyld_y.w[1])
	     && (pyld_x.w[0] <= pyld_y.w[0]))) {
	  res = 1;
	} else {
	  res = 0;
	}
	BID_RETURN (res);
      } else {
	// either x = SNaN and y = QNaN or x = QNaN and y = SNaN
	res = ((x.w[1] & MASK_SNAN) == MASK_SNAN);
	// totalOrder (-QNaN, -SNaN) == 1
	BID_RETURN (res);
      }
    }
  } else if ((y.w[1] & MASK_NAN) == MASK_NAN) {
    // x is certainly not NAN in this case.
    // return true because y is positive
    res = 1;
    BID_RETURN (res);
  }
  // SIMPLE (CASE 2)
  // if all the bits are the same, the numbers are equal.
  if ((x.w[1] == y.w[1]) && (x.w[0] == y.w[0])) {
    res = 1;
    BID_RETURN (res);
  }
  // INFINITY (CASE 3)
  if ((x.w[1] & MASK_INF) == MASK_INF) {
    // x is positive infinity, only return 1 if y is positive infinity as well
    res = ((y.w[1] & MASK_INF) == MASK_INF);
    BID_RETURN (res);
    // (we know y has same sign as x)
  } else if ((y.w[1] & MASK_INF) == MASK_INF) {
    // x is finite, so:
    //    since y is +inf, x<y
    res = 1;
    BID_RETURN (res);
  } else {
    ;	// continue
  }

  // CONVERT x
  sig_x.w[1] = x.w[1] & 0x0001ffffffffffffull;
  sig_x.w[0] = x.w[0];
  exp_x = (x.w[1] >> 49) & 0x000000000003fffull;

  // CHECK IF x IS CANONICAL
  // 9999999999999999999999999999999999 (decimal) = 
  //     1ed09_bead87c0_378d8e63_ffffffff(hexadecimal)
  // [0, 10^34) is the 754 supported canonical range.  
  // If the value exceeds that, it is interpreted as 0.
  if ((((sig_x.w[1] > 0x0001ed09bead87c0ull) ||
	((sig_x.w[1] == 0x0001ed09bead87c0ull) &&
	 (sig_x.w[0] > 0x378d8e63ffffffffull))) &&
       ((x.w[1] & 0x6000000000000000ull) != 0x6000000000000000ull)) ||
      ((x.w[1] & 0x6000000000000000ull) == 0x6000000000000000ull) ||
      ((sig_x.w[1] == 0) && (sig_x.w[0] == 0))) {
    x_is_zero = 1;
    // check for the case where the exponent is shifted right by 2 bits!
    if ((x.w[1] & 0x6000000000000000ull) == 0x6000000000000000ull) {
      exp_x = (x.w[1] >> 47) & 0x000000000003fffull;
    }
  }
  // CONVERT y
  exp_y = (y.w[1] >> 49) & 0x0000000000003fffull;
  sig_y.w[1] = y.w[1] & 0x0001ffffffffffffull;
  sig_y.w[0] = y.w[0];

  // CHECK IF y IS CANONICAL
  // 9999999999999999999999999999999999(decimal) = 
  //     1ed09_bead87c0_378d8e63_ffffffff(hexadecimal)
  // [0, 10^34) is the 754 supported canonical range.  
  // If the value exceeds that, it is interpreted as 0.
  if ((((sig_y.w[1] > 0x0001ed09bead87c0ull) ||
	((sig_y.w[1] == 0x0001ed09bead87c0ull) &&
	 (sig_y.w[0] > 0x378d8e63ffffffffull))) &&
       ((y.w[1] & 0x6000000000000000ull) != 0x6000000000000000ull)) ||
      ((y.w[1] & 0x6000000000000000ull) == 0x6000000000000000ull) ||
      ((sig_y.w[1] == 0) && (sig_y.w[0] == 0))) {
    y_is_zero = 1;
    // check for the case where the exponent is shifted right by 2 bits!
    if ((y.w[1] & 0x6000000000000000ull) == 0x6000000000000000ull) {
      exp_y = (y.w[1] >> 47) & 0x000000000003fffull;
    }
  }
  // ZERO (CASE 4)
  if (x_is_zero && y_is_zero) {
    // we know that signs must be the same because we would have caught it 
    // in case3 if signs were different
    // totalOrder(x,y) iff exp_x <= exp_y for positive numbers
    if (exp_x == exp_y) {
      res = 1;
      BID_RETURN (res);
    }
    res = (exp_x <= exp_y);
    BID_RETURN (res);
  }
  // if x is zero and y isn't, clearly x has the smaller payload
  if (x_is_zero) {
    res = 1;
    BID_RETURN (res);
  }
  // if y is zero, and x isn't, clearly y has the smaller payload
  if (y_is_zero) {
    res = 0;
    BID_RETURN (res);
  }
  // REDUNDANT REPRESENTATIONS (CASE 5)
  // if both components are either bigger or smaller
  if (((sig_x.w[1] > sig_y.w[1])
       || (sig_x.w[1] == sig_y.w[1] && sig_x.w[0] > sig_y.w[0]))
      && exp_x >= exp_y) {
    res = 0;
    BID_RETURN (res);
  }
  if (((sig_x.w[1] < sig_y.w[1])
       || (sig_x.w[1] == sig_y.w[1] && sig_x.w[0] < sig_y.w[0]))
      && exp_x <= exp_y) {
    res = 1;
    BID_RETURN (res);
  }
  // if |exp_x - exp_y| < 33, it comes down to the compensated significand
  if (exp_x > exp_y) {
    // if exp_x is 33 greater than exp_y, it is definitely larger, 
    // so no need for compensation
    if (exp_x - exp_y > 33) {
      res = 0;	// difference cannot be greater than 10^33
      BID_RETURN (res);
    }
    // otherwise adjust the x significand upwards
    if (exp_x - exp_y > 19) {
      __mul_128x128_to_256 (sig_n_prime256, sig_x,
			    bid_ten2k128[exp_x - exp_y - 20]);
      // the compensated significands are equal (ie "x and y represent the same
      // entities") return 1 if (negative && expx > expy) || 
      // (positive && expx < expy)
      if ((sig_n_prime256.w[3] == 0) && (sig_n_prime256.w[2] == 0)
	  && (sig_n_prime256.w[1] == sig_y.w[1])
	  && (sig_n_prime256.w[0] == sig_y.w[0])) {
	// the case (exp_x == exp_y) cannot occur, because all bits must be 
	// the same - would have been caught if (x == y)
	res = 0; // res = (exp_x <= exp_y); but exp_x > exp_y in this case
	BID_RETURN (res);
      }
      // since positive, return 1 if adjusted x is smaller than y
      res = ((sig_n_prime256.w[3] == 0) && (sig_n_prime256.w[2] == 0)
	     && ((sig_n_prime256.w[1] < sig_y.w[1])
		 || (sig_n_prime256.w[1] == sig_y.w[1]
		     && sig_n_prime256.w[0] < sig_y.w[0])));
      BID_RETURN (res);
    }
    __mul_64x128_to_192 (sig_n_prime192, bid_ten2k64[exp_x - exp_y], sig_x);
    // if positive, return whichever significand is larger 
    // (converse if negative)
    if ((sig_n_prime192.w[2] == 0) && sig_n_prime192.w[1] == sig_y.w[1]
	&& (sig_n_prime192.w[0] == sig_y.w[0])) {
      res = 0; // res = (exp_x <= exp_y); but exp_x > exp_y in this case
      BID_RETURN (res);
    }
    res = ((sig_n_prime192.w[2] == 0)
	   && ((sig_n_prime192.w[1] < sig_y.w[1])
	       || (sig_n_prime192.w[1] == sig_y.w[1]
		   && sig_n_prime192.w[0] < sig_y.w[0])));
    BID_RETURN (res);
  }
  // if exp_x is 33 less than exp_y, it is definitely smaller, 
  // no need for compensation
  if (exp_y - exp_x > 33) {
    res = 1;
    BID_RETURN (res);
  } // from this point on 0 <= exp_y - exp_x <= 32
  if (exp_y - exp_x > 19) {
    // adjust the y significand upwards
    __mul_128x128_to_256 (sig_n_prime256, sig_y,
			  bid_ten2k128[exp_y - exp_x - 20]);
    if ((sig_n_prime256.w[3] == 0) && (sig_n_prime256.w[2] == 0)
	&& (sig_n_prime256.w[1] == sig_x.w[1])
	&& (sig_n_prime256.w[0] == sig_x.w[0])) {
      res = 1; // res = (exp_x <= exp_y); but 0 <= exp_y - exp_x <= 32 in this case
      BID_RETURN (res);
    }
    // values are not equal, for positive numbers return 1 if x is less than y
    // and 0 otherwise
    res = ((sig_n_prime256.w[3] != 0) ||
	   // if upper128 bits of compensated y are non-zero, y is bigger
	   (sig_n_prime256.w[2] != 0) ||
	   // if upper128 bits of compensated y are non-zero, y is bigger
	   (sig_n_prime256.w[1] > sig_x.w[1]) ||
	   // if compensated y is bigger, y is bigger
	   (sig_n_prime256.w[1] == sig_x.w[1]
	    && sig_n_prime256.w[0] > sig_x.w[0]));
    BID_RETURN (res);
  } // from this point on 0 <= exp_y - exp_x <= 19
  __mul_64x128_to_192 (sig_n_prime192, bid_ten2k64[exp_y - exp_x], sig_y);
  if ((sig_n_prime192.w[2] == 0) && (sig_n_prime192.w[1] == sig_x.w[1])
      && (sig_n_prime192.w[0] == sig_x.w[0])) {
    res = 1; // res = (exp_x <= exp_y); but 0 <= exp_y - exp_x <= 19 in this case
    BID_RETURN (res);
  }
  res = ((sig_n_prime192.w[2] != 0) ||
	 // if upper128 bits of compensated y are non-zero, y is bigger
	 (sig_n_prime192.w[1] > sig_x.w[1]) ||
	 // if compensated y is bigger, y is bigger
	 (sig_n_prime192.w[1] == sig_x.w[1]
	  && sig_n_prime192.w[0] > sig_x.w[0]));
  BID_RETURN (res);
}

#if DECIMAL_CALL_BY_REFERENCE
void
bid128_radix (int *pres, BID_UINT128 * px _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
  BID_UINT128 x = *px;
#else
RES_WRAPFN_DFP(int, bid128_radix, 128)
int
bid128_radix (BID_UINT128 x _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
#endif
  int res;
  if (x.w[BID_LOW_128W])	// dummy test
    res = 10;
  else
    res = 10;
  BID_RETURN (res);
}

#if DECIMAL_CALL_BY_REFERENCE
void bid128_inf (BID_UINT128 *pres) { 
#else
BID_UINT128 bid128_inf (void) {
#endif
 
  BID_UINT128 res;
  res.w[BID_HIGH_128W] = 0x7800000000000000ull; // +inf
  res.w[BID_LOW_128W] = 0x0000000000000000ull;
  BID_RETURN(res);
}

#if DECIMAL_CALL_BY_REFERENCE
void bid128_nan (BID_UINT128 *pres, const char *tagp) {
#else
DFP_WRAPFN_OTHERTYPE(128, bid128_nan, const char *)
BID_UINT128 bid128_nan (const char *tagp) {
#endif
 
  BID_UINT128 res, x;
#if !DECIMAL_GLOBAL_ROUNDING
  unsigned int rnd_mode = BID_ROUNDING_TO_NEAREST;
#endif
#if !DECIMAL_GLOBAL_EXCEPTION_FLAGS
  unsigned int fpsf;
  unsigned int *pfpsf = &fpsf;
#endif

  res.w[BID_HIGH_128W] = 0x7c00000000000000ull; // +QNaN
  res.w[BID_LOW_128W] = 0x0000000000000000ull;
  if (!tagp) BID_RETURN(res);

#if DECIMAL_CALL_BY_REFERENCE
  bid128_from_string (&x, (char *)tagp _RND_MODE_ARG _EXC_FLAGS_ARG);
#else
  x = bid128_from_string ((char *)tagp _RND_MODE_ARG _EXC_FLAGS_ARG);
#endif
  x.w[BID_HIGH_128W] = x.w[BID_HIGH_128W] & 0x0000cfffffffffffull; // valid values fit in 110 bits=46+64
  res.w[BID_HIGH_128W] = res.w[BID_HIGH_128W] | x.w[BID_HIGH_128W];
  res.w[BID_LOW_128W] = x.w[BID_LOW_128W];

  BID_RETURN(res);
}













