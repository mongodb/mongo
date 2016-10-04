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

static const BID_UINT64 bid_mult_factor[16] = {
  1ull, 10ull, 100ull, 1000ull,
  10000ull, 100000ull, 1000000ull, 10000000ull,
  100000000ull, 1000000000ull, 10000000000ull, 100000000000ull,
  1000000000000ull, 10000000000000ull,
  100000000000000ull, 1000000000000000ull
};

/*****************************************************************************
 *    BID64 non-computational functions:
 *         - bid64_isSigned
 *         - bid64_isNormal
 *         - bid64_isSubnormal
 *         - bid64_isFinite
 *         - bid64_isZero
 *         - bid64_isInf
 *         - bid64_isSignaling
 *         - bid64_isCanonical
 *         - bid64_isNaN
 *         - bid64_copy
 *         - bid64_negate
 *         - bid64_abs
 *         - bid64_copySign
 *         - bid64_class
 *         - bid64_sameQuantum
 *         - bid64_totalOrder
 *         - bid64_totalOrderMag
 *         - bid64_radix
 ****************************************************************************/

#if DECIMAL_CALL_BY_REFERENCE
void
bid64_isSigned (int *pres, BID_UINT64 * px _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
  BID_UINT64 x = *px;
#else
RES_WRAPFN_DFP(int, bid64_isSigned, 64)
int
bid64_isSigned (BID_UINT64 x _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
#endif
  int res;

  res = ((x & MASK_SIGN) == MASK_SIGN);
  BID_RETURN (res);
}

// return 1 iff x is not zero, nor NaN nor subnormal nor infinity
#if DECIMAL_CALL_BY_REFERENCE
void
bid64_isNormal (int *pres, BID_UINT64 * px _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
  BID_UINT64 x = *px;
#else
RES_WRAPFN_DFP(int, bid64_isNormal, 64)
int
bid64_isNormal (BID_UINT64 x _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
#endif
  int res;
  BID_UINT128 sig_x_prime;
  BID_UINT64 sig_x;
  unsigned int exp_x;

  if ((x & MASK_INF) == MASK_INF) {	// x is either INF or NaN
    res = 0;
  } else {
    // decode number into exponent and significand
    if ((x & MASK_STEERING_BITS) == MASK_STEERING_BITS) {
      sig_x = (x & MASK_BINARY_SIG2) | MASK_BINARY_OR2;
      // check for zero or non-canonical
      if (sig_x > 9999999999999999ull || sig_x == 0) {
	res = 0;	// zero or non-canonical
	BID_RETURN (res);
      }
      exp_x = (x & MASK_BINARY_EXPONENT2) >> 51;
    } else {
      sig_x = (x & MASK_BINARY_SIG1);
      if (sig_x == 0) {
	res = 0;	// zero
	BID_RETURN (res);
      }
      exp_x = (x & MASK_BINARY_EXPONENT1) >> 53;
    }
    // if exponent is less than -383, the number may be subnormal
    // if (exp_x - 398 = -383) the number may be subnormal
    if (exp_x < 15) {
      __mul_64x64_to_128MACH (sig_x_prime, sig_x, bid_mult_factor[exp_x]);
      if (sig_x_prime.w[1] == 0
	  && sig_x_prime.w[0] < 1000000000000000ull) {
	res = 0;	// subnormal
      } else {
	res = 1;	// normal
      }
    } else {
      res = 1;	// normal
    }
  }
  BID_RETURN (res);
}

// return 1 iff x is not zero, nor NaN nor normal nor infinity
#if DECIMAL_CALL_BY_REFERENCE
void
bid64_isSubnormal (int *pres,
		   BID_UINT64 * px _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
  BID_UINT64 x = *px;
#else
RES_WRAPFN_DFP(int, bid64_isSubnormal, 64)
int
bid64_isSubnormal (BID_UINT64 x _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
#endif
  int res;
  BID_UINT128 sig_x_prime;
  BID_UINT64 sig_x;
  unsigned int exp_x;

  if ((x & MASK_INF) == MASK_INF) {	// x is either INF or NaN
    res = 0;
  } else {
    // decode number into exponent and significand
    if ((x & MASK_STEERING_BITS) == MASK_STEERING_BITS) {
      sig_x = (x & MASK_BINARY_SIG2) | MASK_BINARY_OR2;
      // check for zero or non-canonical
      if (sig_x > 9999999999999999ull || sig_x == 0) {
	res = 0;	// zero or non-canonical
	BID_RETURN (res);
      }
      exp_x = (x & MASK_BINARY_EXPONENT2) >> 51;
    } else {
      sig_x = (x & MASK_BINARY_SIG1);
      if (sig_x == 0) {
	res = 0;	// zero
	BID_RETURN (res);
      }
      exp_x = (x & MASK_BINARY_EXPONENT1) >> 53;
    }
    // if exponent is less than -383, the number may be subnormal
    // if (exp_x - 398 = -383) the number may be subnormal
    if (exp_x < 15) {
      __mul_64x64_to_128MACH (sig_x_prime, sig_x, bid_mult_factor[exp_x]);
      if (sig_x_prime.w[1] == 0
	  && sig_x_prime.w[0] < 1000000000000000ull) {
	res = 1;	// subnormal
      } else {
	res = 0;	// normal
      }
    } else {
      res = 0;	// normal
    }
  }
  BID_RETURN (res);
}

//iff x is zero, subnormal or normal (not infinity or NaN)
#if DECIMAL_CALL_BY_REFERENCE
void
bid64_isFinite (int *pres, BID_UINT64 * px _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
  BID_UINT64 x = *px;
#else
RES_WRAPFN_DFP(int, bid64_isFinite, 64)
int
bid64_isFinite (BID_UINT64 x _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
#endif
  int res;

  res = ((x & MASK_INF) != MASK_INF);
  BID_RETURN (res);
}

#if DECIMAL_CALL_BY_REFERENCE
void
bid64_isZero (int *pres, BID_UINT64 * px _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
  BID_UINT64 x = *px;
#else
RES_WRAPFN_DFP(int, bid64_isZero, 64)
int
bid64_isZero (BID_UINT64 x _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
#endif
  int res;

  // if infinity or nan, return 0
  if ((x & MASK_INF) == MASK_INF) {
    res = 0;
  } else if ((x & MASK_STEERING_BITS) == MASK_STEERING_BITS) {
    // if steering bits are 11 (condition will be 0), then exponent is G[0:w+1]
    // => sig_x = (x & MASK_BINARY_SIG2) | MASK_BINARY_OR2;
    // if(sig_x > 9999999999999999ull) {return 1;}
    res =
      (((x & MASK_BINARY_SIG2) | MASK_BINARY_OR2) >
       9999999999999999ull);
  } else {
    res = ((x & MASK_BINARY_SIG1) == 0);
  }
  BID_RETURN (res);
}

#if DECIMAL_CALL_BY_REFERENCE
void
bid64_isInf (int *pres, BID_UINT64 * px _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
  BID_UINT64 x = *px;
#else
RES_WRAPFN_DFP(int, bid64_isInf, 64)
int
bid64_isInf (BID_UINT64 x _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
#endif
  int res;

  res = ((x & MASK_INF) == MASK_INF) && ((x & MASK_NAN) != MASK_NAN);
  BID_RETURN (res);
}

#if DECIMAL_CALL_BY_REFERENCE
void
bid64_isSignaling (int *pres,
		   BID_UINT64 * px _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
  BID_UINT64 x = *px;
#else
RES_WRAPFN_DFP(int, bid64_isSignaling, 64)
int
bid64_isSignaling (BID_UINT64 x _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
#endif
  int res;

  res = ((x & MASK_SNAN) == MASK_SNAN);
  BID_RETURN (res);
}

#if DECIMAL_CALL_BY_REFERENCE
void
bid64_isCanonical (int *pres,
		   BID_UINT64 * px _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
  BID_UINT64 x = *px;
#else
RES_WRAPFN_DFP(int, bid64_isCanonical, 64)
int
bid64_isCanonical (BID_UINT64 x _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
#endif
  int res;

  if ((x & MASK_NAN) == MASK_NAN) {	// NaN
    if (x & 0x01fc000000000000ull) {
      res = 0;
    } else if ((x & 0x0003ffffffffffffull) > 999999999999999ull) {	// payload
      res = 0;
    } else {
      res = 1;
    }
  } else if ((x & MASK_INF) == MASK_INF) {
    if (x & 0x03ffffffffffffffull) {
      res = 0;
    } else {
      res = 1;
    }
  } else if ((x & MASK_STEERING_BITS) == MASK_STEERING_BITS) {	// 54-bit coeff.
    res =
      (((x & MASK_BINARY_SIG2) | MASK_BINARY_OR2) <=
       9999999999999999ull);
  } else {	// 53-bit coeff.
    res = 1;
  }
  BID_RETURN (res);
}

#if DECIMAL_CALL_BY_REFERENCE
void
bid64_isNaN (int *pres, BID_UINT64 * px _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
  BID_UINT64 x = *px;
#else
RES_WRAPFN_DFP(int, bid64_isNaN, 64)
int
bid64_isNaN (BID_UINT64 x _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
#endif
  int res;

  res = ((x & MASK_NAN) == MASK_NAN);
  BID_RETURN (res);
}

// copies a floating-point operand x to destination y, with no change
#if DECIMAL_CALL_BY_REFERENCE
void
bid64_copy (BID_UINT64 * pres, BID_UINT64 * px _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
  BID_UINT64 x = *px;
#else
DFP_WRAPFN_DFP(64, bid64_copy, 64)
BID_UINT64
bid64_copy (BID_UINT64 x _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
#endif
  BID_UINT64 res;

  res = x;
  BID_RETURN (res);
}

// copies a floating-point operand x to destination y, reversing the sign
#if DECIMAL_CALL_BY_REFERENCE
void
bid64_negate (BID_UINT64 * pres,
	      BID_UINT64 * px _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
  BID_UINT64 x = *px;
#else
DFP_WRAPFN_DFP(64, bid64_negate, 64)
BID_UINT64
bid64_negate (BID_UINT64 x _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
#endif
  BID_UINT64 res;

  res = x ^ MASK_SIGN;
  BID_RETURN (res);
}

// copies a floating-point operand x to destination y, changing the sign to positive
#if DECIMAL_CALL_BY_REFERENCE
void
bid64_abs (BID_UINT64 * pres, BID_UINT64 * px _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
  BID_UINT64 x = *px;
#else
DFP_WRAPFN_DFP(64, bid64_abs, 64)
BID_UINT64
bid64_abs (BID_UINT64 x _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
#endif
  BID_UINT64 res;

  res = x & ~MASK_SIGN;
  BID_RETURN (res);
}

// copies operand x to destination in the same format as x, but 
// with the sign of y
#if DECIMAL_CALL_BY_REFERENCE
void
bid64_copySign (BID_UINT64 * pres, BID_UINT64 * px,
		BID_UINT64 * py _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
  BID_UINT64 x = *px;
  BID_UINT64 y = *py;
#else
DFP_WRAPFN_DFP_DFP(64, bid64_copySign, 64, 64)
BID_UINT64
bid64_copySign (BID_UINT64 x, BID_UINT64 y _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
#endif
  BID_UINT64 res;

  res = (x & ~MASK_SIGN) | (y & MASK_SIGN);
  BID_RETURN (res);
}

#if DECIMAL_CALL_BY_REFERENCE
void
bid64_class (int *pres, BID_UINT64 * px _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
  BID_UINT64 x = *px;
#else
RES_WRAPFN_DFP(int, bid64_class, 64)
int
bid64_class (BID_UINT64 x _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
#endif
  int res;
  BID_UINT128 sig_x_prime;
  BID_UINT64 sig_x;
  int exp_x;

  if ((x & MASK_NAN) == MASK_NAN) {
    // is the NaN signaling?
    if ((x & MASK_SNAN) == MASK_SNAN) {
      res = signalingNaN;
      BID_RETURN (res);
    }
    // if NaN and not signaling, must be quietNaN
    res = quietNaN;
    BID_RETURN (res);
  } else if ((x & MASK_INF) == MASK_INF) {
    // is the Infinity negative?
    if ((x & MASK_SIGN) == MASK_SIGN) {
      res = negativeInfinity;
    } else {
      // otherwise, must be positive infinity
      res = positiveInfinity;
    }
    BID_RETURN (res);
  } else if ((x & MASK_STEERING_BITS) == MASK_STEERING_BITS) {
    // decode number into exponent and significand
    sig_x = (x & MASK_BINARY_SIG2) | MASK_BINARY_OR2;
    // check for zero or non-canonical
    if (sig_x > 9999999999999999ull || sig_x == 0) {
      if ((x & MASK_SIGN) == MASK_SIGN) {
	res = negativeZero;
      } else {
	res = positiveZero;
      }
      BID_RETURN (res);
    }
    exp_x = (x & MASK_BINARY_EXPONENT2) >> 51;
  } else {
    sig_x = (x & MASK_BINARY_SIG1);
    if (sig_x == 0) {
      res =
	((x & MASK_SIGN) == MASK_SIGN) ? negativeZero : positiveZero;
      BID_RETURN (res);
    }
    exp_x = (x & MASK_BINARY_EXPONENT1) >> 53;
  }
  // if exponent is less than -383, number may be subnormal
  //  if (exp_x - 398 < -383)
  if (exp_x < 15) {	// sig_x *10^exp_x
    __mul_64x64_to_128MACH (sig_x_prime, sig_x, bid_mult_factor[exp_x]);
    if (sig_x_prime.w[1] == 0
	&& (sig_x_prime.w[0] < 1000000000000000ull)) {
      res =
	((x & MASK_SIGN) ==
	 MASK_SIGN) ? negativeSubnormal : positiveSubnormal;
      BID_RETURN (res);
    }
  }
  // otherwise, normal number, determine the sign
  res =
    ((x & MASK_SIGN) == MASK_SIGN) ? negativeNormal : positiveNormal;
  BID_RETURN (res);
}

// true if the exponents of x and y are the same, false otherwise.
// The special cases of sameQuantum (NaN, NaN) and sameQuantum (Inf, Inf) are 
// true.
// If exactly one operand is infinite or exactly one operand is NaN, then false
#if DECIMAL_CALL_BY_REFERENCE
void
bid64_sameQuantum (int *pres, BID_UINT64 * px,
		   BID_UINT64 * py _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
  BID_UINT64 x = *px;
  BID_UINT64 y = *py;
#else
RES_WRAPFN_DFP_DFP(int, bid64_sameQuantum, 64, 64)
int
bid64_sameQuantum (BID_UINT64 x, BID_UINT64 y _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
#endif
  int res;
  unsigned int exp_x, exp_y;

  // if both operands are NaN, return true; if just one is NaN, return false
  if ((x & MASK_NAN) == MASK_NAN || ((y & MASK_NAN) == MASK_NAN)) {
    res = ((x & MASK_NAN) == MASK_NAN && (y & MASK_NAN) == MASK_NAN);
    BID_RETURN (res);
  }
  // if both operands are INF, return true; if just one is INF, return false
  if ((x & MASK_INF) == MASK_INF || (y & MASK_INF) == MASK_INF) {
    res = ((x & MASK_INF) == MASK_INF && (y & MASK_INF) == MASK_INF);
    BID_RETURN (res);
  }
  // decode exponents for both numbers, and return true if they match
  if ((x & MASK_STEERING_BITS) == MASK_STEERING_BITS) {
    exp_x = (x & MASK_BINARY_EXPONENT2) >> 51;
  } else {
    exp_x = (x & MASK_BINARY_EXPONENT1) >> 53;
  }
  if ((y & MASK_STEERING_BITS) == MASK_STEERING_BITS) {
    exp_y = (y & MASK_BINARY_EXPONENT2) >> 51;
  } else {
    exp_y = (y & MASK_BINARY_EXPONENT1) >> 53;
  }
  res = (exp_x == exp_y);
  BID_RETURN (res);
}

#if DECIMAL_CALL_BY_REFERENCE
void
bid64_totalOrder (int *pres, BID_UINT64 * px,
		  BID_UINT64 * py _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
  BID_UINT64 x = *px;
  BID_UINT64 y = *py;
#else
RES_WRAPFN_DFP_DFP(int, bid64_totalOrder, 64, 64)
int
bid64_totalOrder (BID_UINT64 x, BID_UINT64 y _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
#endif
  int res;
  int exp_x, exp_y;
  BID_UINT64 sig_x, sig_y, pyld_y, pyld_x;
  BID_UINT128 sig_n_prime;
  char x_is_zero = 0, y_is_zero = 0;

  // NaN (CASE1)
  // if x and y are unordered numerically because either operand is NaN
  //    (1) totalOrder(-NaN, number) is true
  //    (2) totalOrder(number, +NaN) is true
  //    (3) if x and y are both NaN:
  //           i) negative sign bit < positive sign bit
  //           ii) signaling < quiet for +NaN, reverse for -NaN
  //           iii) lesser payload < greater payload for +NaN (reverse for -NaN)
  //           iv) else if bitwise identical (in canonical form), return 1
  if ((x & MASK_NAN) == MASK_NAN) {
    // if x is -NaN
    if ((x & MASK_SIGN) == MASK_SIGN) {
      // return true, unless y is -NaN also
      if ((y & MASK_NAN) != MASK_NAN || (y & MASK_SIGN) != MASK_SIGN) {
	res = 1;	// y is a number, return 1
	BID_RETURN (res);
      } else {	// if y and x are both -NaN
	// if x and y are both -sNaN or both -qNaN, we have to compare payloads
	// this xnor statement evaluates to true if both are sNaN or qNaN
	if (!
	    (((y & MASK_SNAN) == MASK_SNAN) ^ ((x & MASK_SNAN) ==
					       MASK_SNAN))) {
	  // it comes down to the payload.  we want to return true if x has a
	  // larger payload, or if the payloads are equal (canonical forms
	  // are bitwise identical)
	  pyld_y = y & 0x0003ffffffffffffull;
	  pyld_x = x & 0x0003ffffffffffffull;
	  if (pyld_y > 999999999999999ull || pyld_y == 0) {
	    // if y is zero, x must be less than or numerically equal
	    // y's payload is 0
	    res = 1;
	    BID_RETURN (res);
	  }
	  // if x is zero and y isn't, x has the smaller payload
	  // definitely (since we know y isn't 0 at this point)
	  if (pyld_x > 999999999999999ull || pyld_x == 0) {
	    // x's payload is 0
	    res = 0;
	    BID_RETURN (res);
	  }
	  res = (pyld_x >= pyld_y);
	  BID_RETURN (res);
	} else {
	  // either x = -sNaN and y = -qNaN or x = -qNaN and y = -sNaN
	  res = (y & MASK_SNAN) == MASK_SNAN;	// totalOrder(-qNaN, -sNaN) == 1
	  BID_RETURN (res);
	}
      }
    } else {	// x is +NaN
      // return false, unless y is +NaN also
      if ((y & MASK_NAN) != MASK_NAN || (y & MASK_SIGN) == MASK_SIGN) {
	res = 0;	// y is a number, return 1
	BID_RETURN (res);
      } else {
	// x and y are both +NaN; 
	// must investigate payload if both quiet or both signaling
	// this xnor statement will be true if both x and y are +qNaN or +sNaN
	if (!
	    (((y & MASK_SNAN) == MASK_SNAN) ^ ((x & MASK_SNAN) ==
					       MASK_SNAN))) {
	  // it comes down to the payload.  we want to return true if x has a
	  // smaller payload, or if the payloads are equal (canonical forms
	  // are bitwise identical)
	  pyld_y = y & 0x0003ffffffffffffull;
	  pyld_x = x & 0x0003ffffffffffffull;
	  // if x is zero and y isn't, x has the smaller 
	  // payload definitely (since we know y isn't 0 at this point)
	  if (pyld_x > 999999999999999ull || pyld_x == 0) {
	    res = 1;
	    BID_RETURN (res);
	  }
	  if (pyld_y > 999999999999999ull || pyld_y == 0) {
	    // if y is zero, x must be less than or numerically equal
	    res = 0;
	    BID_RETURN (res);
	  }
	  res = (pyld_x <= pyld_y);
	  BID_RETURN (res);
	} else {
	  // return true if y is +qNaN and x is +sNaN 
	  // (we know they're different bc of xor if_stmt above)
	  res = ((x & MASK_SNAN) == MASK_SNAN);
	  BID_RETURN (res);
	}
      }
    }
  } else if ((y & MASK_NAN) == MASK_NAN) {
    // x is certainly not NAN in this case.
    // return true if y is positive
    res = ((y & MASK_SIGN) != MASK_SIGN);
    BID_RETURN (res);
  }
  // SIMPLE (CASE2)
  // if all the bits are the same, these numbers are equal.
  if (x == y) {
    res = 1;
    BID_RETURN (res);
  }
  // OPPOSITE SIGNS (CASE 3)
  // if signs are opposite, return 1 if x is negative 
  // (if x<y, totalOrder is true)
  if (((x & MASK_SIGN) == MASK_SIGN) ^ ((y & MASK_SIGN) == MASK_SIGN)) {
    res = (x & MASK_SIGN) == MASK_SIGN;
    BID_RETURN (res);
  }
  // INFINITY (CASE4)
  if ((x & MASK_INF) == MASK_INF) {
    // if x==neg_inf, return (y == neg_inf)?1:0;
    if ((x & MASK_SIGN) == MASK_SIGN) {
      res = 1;
      BID_RETURN (res);
    } else {
      // x is positive infinity, only return1 if y 
      // is positive infinity as well
      // (we know y has same sign as x)
      res = ((y & MASK_INF) == MASK_INF);
      BID_RETURN (res);
    }
  } else if ((y & MASK_INF) == MASK_INF) {
    // x is finite, so:
    //    if y is +inf, x<y
    //    if y is -inf, x>y
    res = ((y & MASK_SIGN) != MASK_SIGN);
    BID_RETURN (res);
  }
  // if steering bits are 11 (condition will be 0), then exponent is G[0:w+1] =>
  if ((x & MASK_STEERING_BITS) == MASK_STEERING_BITS) {
    exp_x = (x & MASK_BINARY_EXPONENT2) >> 51;
    sig_x = (x & MASK_BINARY_SIG2) | MASK_BINARY_OR2;
    if (sig_x > 9999999999999999ull || sig_x == 0) {
      x_is_zero = 1;
    }
  } else {
    exp_x = (x & MASK_BINARY_EXPONENT1) >> 53;
    sig_x = (x & MASK_BINARY_SIG1);
    if (sig_x == 0) {
      x_is_zero = 1;
    }
  }

  // if steering bits are 11 (condition will be 0), then exponent is G[0:w+1] =>
  if ((y & MASK_STEERING_BITS) == MASK_STEERING_BITS) {
    exp_y = (y & MASK_BINARY_EXPONENT2) >> 51;
    sig_y = (y & MASK_BINARY_SIG2) | MASK_BINARY_OR2;
    if (sig_y > 9999999999999999ull || sig_y == 0) {
      y_is_zero = 1;
    }
  } else {
    exp_y = (y & MASK_BINARY_EXPONENT1) >> 53;
    sig_y = (y & MASK_BINARY_SIG1);
    if (sig_y == 0) {
      y_is_zero = 1;
    }
  }

  // ZERO (CASE 5)
  // if x and y represent the same entities, and 
  // both are negative , return true iff exp_x <= exp_y
  if (x_is_zero && y_is_zero) {
    if (!((x & MASK_SIGN) == MASK_SIGN) ^
	((y & MASK_SIGN) == MASK_SIGN)) {
      // if signs are the same:
      // totalOrder(x,y) iff exp_x >= exp_y for negative numbers
      // totalOrder(x,y) iff exp_x <= exp_y for positive numbers
      if (exp_x == exp_y) {
	res = 1;
	BID_RETURN (res);
      }
      res = (exp_x <= exp_y) ^ ((x & MASK_SIGN) == MASK_SIGN);
      BID_RETURN (res);
    } else {
      // signs are different.
      // totalOrder(-0, +0) is true
      // totalOrder(+0, -0) is false
      res = ((x & MASK_SIGN) == MASK_SIGN);
      BID_RETURN (res);
    }
  }
  // if x is zero and y isn't, clearly x has the smaller payload.
  if (x_is_zero) {
    res = ((y & MASK_SIGN) != MASK_SIGN);
    BID_RETURN (res);
  }
  // if y is zero, and x isn't, clearly y has the smaller payload.
  if (y_is_zero) {
    res = ((x & MASK_SIGN) == MASK_SIGN);
    BID_RETURN (res);
  }
  // REDUNDANT REPRESENTATIONS (CASE6)
  // if both components are either bigger or smaller, 
  // it is clear what needs to be done
  if (sig_x > sig_y && exp_x >= exp_y) {
    res = ((x & MASK_SIGN) == MASK_SIGN);
    BID_RETURN (res);
  }
  if (sig_x < sig_y && exp_x <= exp_y) {
    res = ((x & MASK_SIGN) != MASK_SIGN);
    BID_RETURN (res);
  }
  // if exp_x is 15 greater than exp_y, it is 
  // definitely larger, so no need for compensation
  if (exp_x - exp_y > 15) {
    // difference cannot be greater than 10^15
    res = ((x & MASK_SIGN) == MASK_SIGN);
    BID_RETURN (res);
  }
  // if exp_x is 15 less than exp_y, it is 
  // definitely smaller, no need for compensation
  if (exp_y - exp_x > 15) {
    res = ((x & MASK_SIGN) != MASK_SIGN);
    BID_RETURN (res);
  }
  // if |exp_x - exp_y| < 15, it comes down 
  // to the compensated significand
  if (exp_x > exp_y) {
    // otherwise adjust the x significand upwards
    __mul_64x64_to_128MACH (sig_n_prime, sig_x,
			    bid_mult_factor[exp_x - exp_y]);
    // if x and y represent the same entities, 
    // and both are negative, return true iff exp_x <= exp_y
    if (sig_n_prime.w[1] == 0 && (sig_n_prime.w[0] == sig_y)) {
      // case cannot occure, because all bits must 
      // be the same - would have been caught if (x==y)
      res = (exp_x <= exp_y) ^ ((x & MASK_SIGN) == MASK_SIGN);
      BID_RETURN (res);
    }
    // if positive, return 1 if adjusted x is smaller than y
    res = ((sig_n_prime.w[1] == 0)
	   && sig_n_prime.w[0] < sig_y) ^ ((x & MASK_SIGN) ==
					   MASK_SIGN);
    BID_RETURN (res);
  }
  // adjust the y significand upwards
  __mul_64x64_to_128MACH (sig_n_prime, sig_y,
			  bid_mult_factor[exp_y - exp_x]);

  // if x and y represent the same entities, 
  // and both are negative, return true iff exp_x <= exp_y
  if (sig_n_prime.w[1] == 0 && (sig_n_prime.w[0] == sig_x)) {
    // Cannot occur, because all bits must be the same. 
    // Case would have been caught if (x==y)
    res = (exp_x <= exp_y) ^ ((x & MASK_SIGN) == MASK_SIGN);
    BID_RETURN (res);
  }
  // values are not equal, for positive numbers return 1 
  // if x is less than y.  0 otherwise
  res = ((sig_n_prime.w[1] > 0)
	 || (sig_x < sig_n_prime.w[0])) ^ ((x & MASK_SIGN) ==
					   MASK_SIGN);
  BID_RETURN (res);
}

// totalOrderMag is TotalOrder(abs(x), abs(y))
#if DECIMAL_CALL_BY_REFERENCE
void
bid64_totalOrderMag (int *pres, BID_UINT64 * px,
		     BID_UINT64 * py _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
  BID_UINT64 x = *px;
  BID_UINT64 y = *py;
#else
RES_WRAPFN_DFP_DFP(int, bid64_totalOrderMag, 64, 64)
int
bid64_totalOrderMag (BID_UINT64 x,
		     BID_UINT64 y _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
#endif
  int res;
  int exp_x, exp_y;
  BID_UINT64 sig_x, sig_y, pyld_y, pyld_x;
  BID_UINT128 sig_n_prime;
  char x_is_zero = 0, y_is_zero = 0;

  // NaN (CASE 1)
  // if x and y are unordered numerically because either operand is NaN
  //    (1) totalOrder(number, +NaN) is true
  //    (2) if x and y are both NaN:
  //       i) signaling < quiet for +NaN
  //       ii) lesser payload < greater payload for +NaN
  //       iii) else if bitwise identical (in canonical form), return 1
  if ((x & MASK_NAN) == MASK_NAN) {
    // x is +NaN

    // return false, unless y is +NaN also
    if ((y & MASK_NAN) != MASK_NAN) {
      res = 0;	// y is a number, return 1
      BID_RETURN (res);

    } else {

      // x and y are both +NaN; 
      // must investigate payload if both quiet or both signaling
      // this xnor statement will be true if both x and y are +qNaN or +sNaN
      if (!
	  (((y & MASK_SNAN) == MASK_SNAN) ^ ((x & MASK_SNAN) ==
					     MASK_SNAN))) {
	// it comes down to the payload.  we want to return true if x has a
	// smaller payload, or if the payloads are equal (canonical forms
	// are bitwise identical)
	pyld_y = y & 0x0003ffffffffffffull;
	pyld_x = x & 0x0003ffffffffffffull;
	// if x is zero and y isn't, x has the smaller 
	// payload definitely (since we know y isn't 0 at this point)
	if (pyld_x > 999999999999999ull || pyld_x == 0) {
	  res = 1;
	  BID_RETURN (res);
	}

	if (pyld_y > 999999999999999ull || pyld_y == 0) {
	  // if y is zero, x must be less than or numerically equal
	  res = 0;
	  BID_RETURN (res);
	}
	res = (pyld_x <= pyld_y);
	BID_RETURN (res);

      } else {
	// return true if y is +qNaN and x is +sNaN 
	// (we know they're different bc of xor if_stmt above)
	res = ((x & MASK_SNAN) == MASK_SNAN);
	BID_RETURN (res);
      }
    }

  } else if ((y & MASK_NAN) == MASK_NAN) {
    // x is certainly not NAN in this case.
    // return true if y is positive
    res = 1;
    BID_RETURN (res);
  }
  // SIMPLE (CASE2)
  // if all the bits (except sign bit) are the same, 
  // these numbers are equal.
  if ((x & ~MASK_SIGN) == (y & ~MASK_SIGN)) {
    res = 1;
    BID_RETURN (res);
  }
  // INFINITY (CASE3)
  if ((x & MASK_INF) == MASK_INF) {
    // x is positive infinity, only return1 
    // if y is positive infinity as well
    res = ((y & MASK_INF) == MASK_INF);
    BID_RETURN (res);
  } else if ((y & MASK_INF) == MASK_INF) {
    // x is finite, so:
    //    if y is +inf, x<y
    res = 1;
    BID_RETURN (res);
  }
  // if steering bits are 11 (condition will be 0), 
  // then exponent is G[0:w+1] =>
  if ((x & MASK_STEERING_BITS) == MASK_STEERING_BITS) {
    exp_x = (x & MASK_BINARY_EXPONENT2) >> 51;
    sig_x = (x & MASK_BINARY_SIG2) | MASK_BINARY_OR2;
    if (sig_x > 9999999999999999ull || sig_x == 0) {
      x_is_zero = 1;
    }
  } else {
    exp_x = (x & MASK_BINARY_EXPONENT1) >> 53;
    sig_x = (x & MASK_BINARY_SIG1);
    if (sig_x == 0) {
      x_is_zero = 1;
    }
  }

  // if steering bits are 11 (condition will be 0), 
  // then exponent is G[0:w+1] =>
  if ((y & MASK_STEERING_BITS) == MASK_STEERING_BITS) {
    exp_y = (y & MASK_BINARY_EXPONENT2) >> 51;
    sig_y = (y & MASK_BINARY_SIG2) | MASK_BINARY_OR2;
    if (sig_y > 9999999999999999ull || sig_y == 0) {
      y_is_zero = 1;
    }
  } else {
    exp_y = (y & MASK_BINARY_EXPONENT1) >> 53;
    sig_y = (y & MASK_BINARY_SIG1);
    if (sig_y == 0) {
      y_is_zero = 1;
    }
  }

  // ZERO (CASE 5)
  // if x and y represent the same entities, 
  // and both are negative , return true iff exp_x <= exp_y
  if (x_is_zero && y_is_zero) {
    // totalOrder(x,y) iff exp_x <= exp_y for positive numbers
    res = (exp_x <= exp_y);
    BID_RETURN (res);
  }
  // if x is zero and y isn't, clearly x has the smaller payload.
  if (x_is_zero) {
    res = 1;
    BID_RETURN (res);
  }
  // if y is zero, and x isn't, clearly y has the smaller payload.
  if (y_is_zero) {
    res = 0;
    BID_RETURN (res);
  }
  // REDUNDANT REPRESENTATIONS (CASE6)
  // if both components are either bigger or smaller
  if (sig_x > sig_y && exp_x >= exp_y) {
    res = 0;
    BID_RETURN (res);
  }
  if (sig_x < sig_y && exp_x <= exp_y) {
    res = 1;
    BID_RETURN (res);
  }
  // if exp_x is 15 greater than exp_y, it is definitely 
  // larger, so no need for compensation
  if (exp_x - exp_y > 15) {
    res = 0;	// difference cannot be greater than 10^15
    BID_RETURN (res);
  }
  // if exp_x is 15 less than exp_y, it is definitely 
  // smaller, no need for compensation
  if (exp_y - exp_x > 15) {
    res = 1;
    BID_RETURN (res);
  }
  // if |exp_x - exp_y| <= 15, it comes down 
  // to the compensated significand
  if (exp_x > exp_y) {

    // otherwise adjust the x significand upwards
    __mul_64x64_to_128MACH (sig_n_prime, sig_x,
			    bid_mult_factor[exp_x - exp_y]);

    // if x and y represent the same entities, 
    // and both are negative, return true iff exp_x <= exp_y
    if (sig_n_prime.w[1] == 0 && (sig_n_prime.w[0] == sig_y)) {
      // case cannot occur, because all bits 
      // must be the same - would have been caught if (x==y)
      res = 0; // res = (exp_x <= exp_y); but exp_x > exp_y
      BID_RETURN (res);
    }
    // if positive, return 1 if adjusted x is smaller than y
    res = ((sig_n_prime.w[1] == 0) && sig_n_prime.w[0] < sig_y);
    BID_RETURN (res);
  } // from this point on -15 <= exp_x - exp_y <= 0
  // adjust the y significand upwards
  __mul_64x64_to_128MACH (sig_n_prime, sig_y,
			  bid_mult_factor[exp_y - exp_x]);

  // if x and y represent the same entities, 
  // and both are negative, return true iff exp_x <= exp_y
  if (sig_n_prime.w[1] == 0 && (sig_n_prime.w[0] == sig_x)) {
    res = 1; // res = (exp_x <= exp_y); but -15 <= exp_x - exp_y <= 0
    BID_RETURN (res);
  }
  // values are not equal, for positive numbers 
  // return 1 if x is less than y.  0 otherwise
  res = ((sig_n_prime.w[1] > 0) || (sig_x < sig_n_prime.w[0]));
  BID_RETURN (res);

}

#if DECIMAL_CALL_BY_REFERENCE
void
bid64_radix (int *pres, BID_UINT64 * px _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
  BID_UINT64 x = *px;
#else
RES_WRAPFN_DFP(int, bid64_radix, 64)
int
bid64_radix (BID_UINT64 x _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
#endif
  int res;
  if (x)	// dummy test
    res = 10;
  else
    res = 10;
  BID_RETURN (res);
}

#if DECIMAL_CALL_BY_REFERENCE
void bid64_inf (BID_UINT64 *pres) {
#else
BID_UINT64 bid64_inf (void) {
#endif
 
  BID_UINT64 res;
  res = 0x7800000000000000ull; // +inf
  BID_RETURN(res);
}

#if DECIMAL_CALL_BY_REFERENCE
void bid64_nan (BID_UINT64 *pres, const char *tagp) {
#else
DFP_WRAPFN_OTHERTYPE(64, bid64_nan, const char *)
BID_UINT64 bid64_nan (const char *tagp) {
#endif
 
  BID_UINT64 res, x;
#if !DECIMAL_GLOBAL_ROUNDING
  unsigned int rnd_mode = BID_ROUNDING_TO_NEAREST;
#endif
#if !DECIMAL_GLOBAL_EXCEPTION_FLAGS
  unsigned int fpsf;
  unsigned int *pfpsf = &fpsf;
#endif

  res = 0x7c00000000000000ull; // +QNaN
  if (!tagp) BID_RETURN(res);

#if DECIMAL_CALL_BY_REFERENCE
  bid64_from_string (&x, (char *)tagp _RND_MODE_ARG _EXC_FLAGS_ARG);
#else
  x = bid64_from_string ((char *)tagp _RND_MODE_ARG _EXC_FLAGS_ARG);
#endif
  x = x & 0x0003ffffffffffffull; // valid values fit in 50 bits
  res = res | x;
 
  BID_RETURN(res);
}
 
