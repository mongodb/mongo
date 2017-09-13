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

static const BID_UINT32 bid_mult_factor[7] = {
  1, 10, 100, 1000, 10000, 100000, 1000000
};

/*****************************************************************************
 *    BID32 non-computational functions:
 *         - bid32_isSigned
 *         - bid32_isNormal
 *         - bid32_isSubnormal
 *         - bid32_isFinite
 *         - bid32_isZero
 *         - bid32_isInf
 *         - bid32_isSignaling
 *         - bid32_isCanonical
 *         - bid32_isNaN
 *         - bid32_copy
 *         - bid32_negate
 *         - bid32_abs
 *         - bid32_copySign
 *         - bid32_class
 *         - bid32_sameQuantum
 *         - bid32_totalOrder
 *         - bid32_totalOrderMag
 *         - bid32_radix
 ****************************************************************************/

#if DECIMAL_CALL_BY_REFERENCE
void
bid32_isSigned (int *pres, BID_UINT32 * px _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
  BID_UINT32 x = *px;
#else
RES_WRAPFN_DFP(int, bid32_isSigned, 32)
int
bid32_isSigned (BID_UINT32 x _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
#endif
  int res;

  res = ((x & MASK_SIGN32) == MASK_SIGN32);
  BID_RETURN (res);
}

// return 1 iff x is not zero, nor NaN nor subnormal nor infinity
#if DECIMAL_CALL_BY_REFERENCE
void
bid32_isNormal (int *pres, BID_UINT32 * px _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
  BID_UINT32 x = *px;
#else
RES_WRAPFN_DFP(int, bid32_isNormal, 32)
int
bid32_isNormal (BID_UINT32 x _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
#endif
  int res;
  BID_UINT64 sig_x_prime;
  BID_UINT32 sig_x;
  unsigned int exp_x;

  if ((x & MASK_INF32) == MASK_INF32) {	// x is either INF or NaN
    res = 0;
  } else {
    // decode number into exponent and significand
    if ((x & MASK_STEERING_BITS32) == MASK_STEERING_BITS32) {
      sig_x = (x & MASK_BINARY_SIG2_32) | MASK_BINARY_OR2_32;
      // check for zero or non-canonical
      if (sig_x > 9999999 || sig_x == 0) {
	res = 0; // zero or non-canonical
	BID_RETURN (res);
      }
      exp_x = (x & MASK_BINARY_EXPONENT2_32) >> 21;
    } else {
      sig_x = (x & MASK_BINARY_SIG1_32);
      if (sig_x == 0) {
	res = 0; // zero
	BID_RETURN (res);
      }
      exp_x = (x & MASK_BINARY_EXPONENT1_32) >> 23;
    }
    // if exponent is less than -95, the number may be subnormal
    // if (exp_x - 101 = -95) the number may be subnormal
    if (exp_x < 6) {
      sig_x_prime = (BID_UINT64)sig_x * (BID_UINT64)bid_mult_factor[exp_x];
      if (sig_x_prime < 1000000ull) {
	res = 0; // subnormal
      } else {
	res = 1; // normal
      }
    } else {
      res = 1; // normal
    }
  }
  BID_RETURN (res);
}

// return 1 iff x is not zero, NaN, normal, or infinity
#if DECIMAL_CALL_BY_REFERENCE
void
bid32_isSubnormal (int *pres, BID_UINT32 * px 
    _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
  BID_UINT32 x = *px;
#else
RES_WRAPFN_DFP(int, bid32_isSubnormal, 32)
int
bid32_isSubnormal (BID_UINT32 x _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
#endif
  int res;
  BID_UINT64 sig_x_prime;
  BID_UINT32 sig_x;
  unsigned int exp_x;

  if ((x & MASK_INF32) == MASK_INF32) {	// x is either INF or NaN
    res = 0;
  } else {
    // decode number into exponent and significand
    if ((x & MASK_STEERING_BITS32) == MASK_STEERING_BITS32) {
      sig_x = (x & MASK_BINARY_SIG2_32) | MASK_BINARY_OR2_32;
      // check for zero or non-canonical
      if (sig_x > 9999999 || sig_x == 0) {
	res = 0; // zero or non-canonical
	BID_RETURN (res);
      }
      exp_x = (x & MASK_BINARY_EXPONENT2_32) >> 21;
    } else {
      sig_x = (x & MASK_BINARY_SIG1_32);
      if (sig_x == 0) {
	res = 0; // zero
	BID_RETURN (res);
      }
      exp_x = (x & MASK_BINARY_EXPONENT1_32) >> 23;
    }
    // if exponent is less than -95, the number may be subnormal
    // if (exp_x - 101 = -95) the number may be subnormal
    if (exp_x < 6) {
      sig_x_prime = (BID_UINT64)sig_x * (BID_UINT64)bid_mult_factor[exp_x];
      if (sig_x_prime < 1000000ull) {
        res = 1; // subnormal
      } else {
        res = 0; // normal
      }
    } else {
      res = 0; // normal
    }
  }
  BID_RETURN (res);

}

//iff x is zero, subnormal or normal (not infinity or NaN)
#if DECIMAL_CALL_BY_REFERENCE
void
bid32_isFinite (int *pres, BID_UINT32 * px _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
  BID_UINT32 x = *px;
#else
RES_WRAPFN_DFP(int, bid32_isFinite, 32)
int
bid32_isFinite (BID_UINT32 x _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
#endif
  int res;

  res = ((x & MASK_INF32) != MASK_INF32);
  BID_RETURN (res);
}

#if DECIMAL_CALL_BY_REFERENCE
void
bid32_isZero (int *pres, BID_UINT32 * px _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
  BID_UINT32 x = *px;
#else
RES_WRAPFN_DFP(int, bid32_isZero, 32)
int
bid32_isZero (BID_UINT32 x _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
#endif
  int res;

  // if infinity or nan, return 0
  if ((x & MASK_INF32) == MASK_INF32) {
    res = 0;
  } else if ((x & MASK_STEERING_BITS32) == MASK_STEERING_BITS32) {
    // if steering bits are 11 (condition will be 0), then exponent is G[0:w+1]
    // => sig_x = (x & MASK_BINARY_SIG2_32) | MASK_BINARY_OR2_32;
    // if(sig_x > 9999999) {return 1;}
    res =
      (((x & MASK_BINARY_SIG2_32) | MASK_BINARY_OR2_32) >
       9999999);
  } else {
    res = ((x & MASK_BINARY_SIG1_32) == 0);
  }
  BID_RETURN (res);
}

#if DECIMAL_CALL_BY_REFERENCE
void
bid32_isInf (int *pres, BID_UINT32 * px _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
  BID_UINT32 x = *px;
#else
RES_WRAPFN_DFP(int, bid32_isInf, 32)
int
bid32_isInf (BID_UINT32 x _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
#endif
  int res;

  res = ((x & MASK_INF32) == MASK_INF32) && ((x & MASK_NAN32) != MASK_NAN32);
  BID_RETURN (res);
}

#if DECIMAL_CALL_BY_REFERENCE
void
bid32_isSignaling (int *pres, BID_UINT32 * px _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
  BID_UINT32 x = *px;
#else
RES_WRAPFN_DFP(int, bid32_isSignaling, 32)
int
bid32_isSignaling (BID_UINT32 x _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
#endif
  int res;

  res = ((x & MASK_SNAN32) == MASK_SNAN32);
  BID_RETURN (res);
}

#if DECIMAL_CALL_BY_REFERENCE
void
bid32_isCanonical (int *pres, BID_UINT32 * px _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
  BID_UINT32 x = *px;
#else
RES_WRAPFN_DFP(int, bid32_isCanonical, 32)
int
bid32_isCanonical (BID_UINT32 x _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
#endif
  int res;

  if ((x & MASK_NAN32) == MASK_NAN32) {	// NaN
    if (x & 0x01f00000) {
      res = 0;
    } else if ((x & 0x000fffff) > 999999) { // payload
      res = 0;
    } else {
      res = 1;
    }
  } else if ((x & MASK_INF32) == MASK_INF32) {
    if (x & 0x03ffffff) {
      res = 0;
    } else {
      res = 1;
    }
  } else if ((x & MASK_STEERING_BITS32) == MASK_STEERING_BITS32) { // 24-bit
    res = (((x & MASK_BINARY_SIG2_32) | MASK_BINARY_OR2_32) <= 9999999);
  } else { // 23-bit coeff.
    res = 1;
  }
  BID_RETURN (res);
}

#if DECIMAL_CALL_BY_REFERENCE
void
bid32_isNaN (int *pres, BID_UINT32 * px _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
  BID_UINT32 x = *px;
#else
RES_WRAPFN_DFP(int, bid32_isNaN, 32)
int
bid32_isNaN (BID_UINT32 x _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
#endif
  int res;

  res = ((x & MASK_NAN32) == MASK_NAN32);
  BID_RETURN (res);
}

// copies a floating-point operand x to destination y, with no change
#if DECIMAL_CALL_BY_REFERENCE
void
bid32_copy (BID_UINT32 * pres, BID_UINT32 * px _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
  BID_UINT32 x = *px;
#else
DFP_WRAPFN_DFP(32, bid32_copy, 32)
BID_UINT32
bid32_copy (BID_UINT32 x _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
#endif
  BID_UINT32 res;

  res = x;
  BID_RETURN (res);
}

// copies a floating-point operand x to destination y, reversing the sign
#if DECIMAL_CALL_BY_REFERENCE
void
bid32_negate (BID_UINT32 * pres, BID_UINT32 * px _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
  BID_UINT32 x = *px;
#else
DFP_WRAPFN_DFP(32, bid32_negate, 32)
BID_UINT32
bid32_negate (BID_UINT32 x _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
#endif
  BID_UINT32 res;

  res = x ^ MASK_SIGN32;
  BID_RETURN (res);
}

// copies a floating-point operand x to destination y, changing the sign to positive
#if DECIMAL_CALL_BY_REFERENCE
void
bid32_abs (BID_UINT32 * pres, BID_UINT32 * px _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
  BID_UINT32 x = *px;
#else
DFP_WRAPFN_DFP(32, bid32_abs, 32)
BID_UINT32
bid32_abs (BID_UINT32 x _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
#endif
  BID_UINT32 res;

  res = x & ~MASK_SIGN32;
  BID_RETURN (res);
}

// copies operand x to destination in the same format as x, but 
// with the sign of y
DFP_WRAPFN_DFP_DFP(32, bid32_copySign, 32, 32);   

#if DECIMAL_CALL_BY_REFERENCE
void
bid32_copySign (BID_UINT32 * pres, BID_UINT32 * px,
		BID_UINT32 * py _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
  BID_UINT32 x = *px;
  BID_UINT32 y = *py;
#else
BID_UINT32
bid32_copySign (BID_UINT32 x, BID_UINT32 y _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
#endif
  BID_UINT32 res;

  res = (x & ~MASK_SIGN32) | (y & MASK_SIGN32);
  BID_RETURN (res);
}

#if DECIMAL_CALL_BY_REFERENCE
void
bid32_class (int *pres, BID_UINT32 * px _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
  BID_UINT32 x = *px;
#else
RES_WRAPFN_DFP(int, bid32_class, 32)
int
bid32_class (BID_UINT32 x _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
#endif
  int res;
  BID_UINT64 sig_x_prime;
  BID_UINT32 sig_x;
  int exp_x;

  if ((x & MASK_NAN32) == MASK_NAN32) {
    // is the NaN signaling?
    if ((x & MASK_SNAN32) == MASK_SNAN32) {
      res = signalingNaN;
      BID_RETURN (res);
    }
    // if NaN and not signaling, must be quietNaN
    res = quietNaN;
    BID_RETURN (res);
  } else if ((x & MASK_INF32) == MASK_INF32) {
    // is the Infinity negative?
    if ((x & MASK_SIGN32) == MASK_SIGN32) {
      res = negativeInfinity;
    } else {
      // otherwise, must be positive infinity
      res = positiveInfinity;
    }
    BID_RETURN (res);
  } else if ((x & MASK_STEERING_BITS32) == MASK_STEERING_BITS32) {
    // decode number into exponent and significand
    sig_x = (x & MASK_BINARY_SIG2_32) | MASK_BINARY_OR2_32;
    // check for zero or non-canonical
    if (sig_x > 9999999 || sig_x == 0) {
      if ((x & MASK_SIGN32) == MASK_SIGN32) {
	res = negativeZero;
      } else {
	res = positiveZero;
      }
      BID_RETURN (res);
    }
    exp_x = (x & MASK_BINARY_EXPONENT2_32) >> 21;
  } else {
    sig_x = (x & MASK_BINARY_SIG1_32);
    if (sig_x == 0) {
      res = ((x & MASK_SIGN32) == MASK_SIGN32) ? negativeZero : positiveZero;
      BID_RETURN (res);
    }
    exp_x = (x & MASK_BINARY_EXPONENT1_32) >> 23;
  }
  // if exponent is less than -95, number may be subnormal
  //  if (exp_x - 101 < -95)
  if (exp_x < 6) { // sig_x *10^exp_x
    sig_x_prime = (BID_UINT64)sig_x * (BID_UINT64)bid_mult_factor[exp_x];
    if (sig_x_prime < 1000000ull) {
      res = ((x & MASK_SIGN32) ==
          MASK_SIGN32) ? negativeSubnormal : positiveSubnormal;
      BID_RETURN (res);
    }
  }
  // otherwise, normal number, determine the sign
  res = ((x & MASK_SIGN32) == MASK_SIGN32) ? negativeNormal : positiveNormal;
  BID_RETURN (res);
}

// true if the exponents of x and y are the same, false otherwise.
// The special cases of sameQuantum (NaN, NaN) and sameQuantum (Inf, Inf) are 
// true.
// If exactly one operand is infinite or exactly one operand is NaN, then false
#if DECIMAL_CALL_BY_REFERENCE
void
bid32_sameQuantum (int *pres, BID_UINT32 * px, BID_UINT32 * py 
    _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
  BID_UINT32 x = *px;
  BID_UINT32 y = *py;
#else
RES_WRAPFN_DFP_DFP(int, bid32_sameQuantum, 32, 32)
int
bid32_sameQuantum (BID_UINT32 x, BID_UINT32 y _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
#endif
  int res;
  unsigned int exp_x, exp_y;

  // if both operands are NaN, return true; if just one is NaN, return false
  if ((x & MASK_NAN32) == MASK_NAN32 || ((y & MASK_NAN32) == MASK_NAN32)) {
    res = ((x & MASK_NAN32) == MASK_NAN32 && (y & MASK_NAN32) == MASK_NAN32);
    BID_RETURN (res);
  }
  // if both operands are INF, return true; if just one is INF, return false
  if ((x & MASK_INF32) == MASK_INF32 || (y & MASK_INF32) == MASK_INF32) {
    res = ((x & MASK_INF32) == MASK_INF32 && (y & MASK_INF32) == MASK_INF32);
    BID_RETURN (res);
  }
  // decode exponents for both numbers, and return true if they match
  if ((x & MASK_STEERING_BITS32) == MASK_STEERING_BITS32) {
    exp_x = (x & MASK_BINARY_EXPONENT2_32) >> 21;
  } else {
    exp_x = (x & MASK_BINARY_EXPONENT1_32) >> 23;
  }
  if ((y & MASK_STEERING_BITS32) == MASK_STEERING_BITS32) {
    exp_y = (y & MASK_BINARY_EXPONENT2_32) >> 21;
  } else {
    exp_y = (y & MASK_BINARY_EXPONENT1_32) >> 23;
  }
  res = (exp_x == exp_y);
  BID_RETURN (res);
}

#if DECIMAL_CALL_BY_REFERENCE
void
bid32_totalOrder (int *pres, BID_UINT32 * px, BID_UINT32 * py 
    _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
  BID_UINT32 x = *px;
  BID_UINT32 y = *py;
#else
RES_WRAPFN_DFP_DFP(int, bid32_totalOrder, 32, 32)
int
bid32_totalOrder (BID_UINT32 x, BID_UINT32 y _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
#endif
  int res;
  int exp_x, exp_y;
  BID_UINT32 sig_x, sig_y, pyld_y, pyld_x;
  BID_UINT64 sig_n_prime;
  char x_is_zero = 0, y_is_zero = 0;

  // NaN (CASE1)
  // if x and y are unordered numerically because either operand is NaN
  //    (1) totalOrder(-NaN, number) is true
  //    (2) totalOrder(number, +NaN) is true
  //    (3) if x and y are both NaN:
  //           i) negative sign bit < positive sign bit
  //           ii) signaling < quiet for +NaN, reverse for -NaN
  //           iii) lesser payload < greater payload for +NaN(reverse for -NaN)
  //           iv) else if bitwise identical (in canonical form), return 1
  if ((x & MASK_NAN32) == MASK_NAN32) {
    // if x is -NaN
    if ((x & MASK_SIGN32) == MASK_SIGN32) {
      // return true, unless y is -NaN also
      if ((y & MASK_NAN32) != MASK_NAN32 || (y & MASK_SIGN32) != MASK_SIGN32) {
	res = 1; // y is a number, return 1
	BID_RETURN (res);
      } else { // if y and x are both -NaN
	// if x and y are both -sNaN or both -qNaN, we have to compare payloads
	// this xnor statement evaluates to true if both are sNaN or qNaN
	if (!(((y & MASK_SNAN32) == MASK_SNAN32) ^ 
            ((x & MASK_SNAN32) == MASK_SNAN32))) {
	  // it comes down to the payload.  we want to return true if x has a
	  // larger payload, or if the payloads are equal (canonical forms
	  // are bitwise identical)
	  pyld_y = y & 0x000fffff;
	  pyld_x = x & 0x000fffff;
	  if (pyld_y > 999999 || pyld_y == 0) {
	    // if y is zero, x must be less than or numerically equal
	    // y's payload is 0
	    res = 1;
	    BID_RETURN (res);
	  }
	  // if x is zero and y isn't, x has the smaller payload
	  // definitely (since we know y isn't 0 at this point)
	  if (pyld_x > 999999 || pyld_x == 0) {
	    // x's payload is 0
	    res = 0;
	    BID_RETURN (res);
	  }
	  res = (pyld_x >= pyld_y);
	  BID_RETURN (res);
	} else {
	  // either x = -sNaN and y = -qNaN or x = -qNaN and y = -sNaN
	  res = (y & MASK_SNAN32) == MASK_SNAN32; // totalOrder(-qNaN,-sNaN)==1
	  BID_RETURN (res);
	}
      }
    } else { // x is +NaN
      // return false, unless y is +NaN also
      if ((y & MASK_NAN32) != MASK_NAN32 || (y & MASK_SIGN32) == MASK_SIGN32) {
	res = 0; // y is a number, return 1
	BID_RETURN (res);
      } else {
	// x and y are both +NaN; 
	// must investigate payload if both quiet or both signaling
	// this xnor statement will be true if both x and y are +qNaN or +sNaN
	if (!(((y & MASK_SNAN32) == MASK_SNAN32) ^ 
            ((x & MASK_SNAN32) == MASK_SNAN32))) {
	  // it comes down to the payload.  we want to return true if x has a
	  // smaller payload, or if the payloads are equal (canonical forms
	  // are bitwise identical)
	  pyld_y = y & 0x000fffff;
	  pyld_x = x & 0x000fffff;
	  // if x is zero and y isn't, x has the smaller 
	  // payload definitely (since we know y isn't 0 at this point)
	  if (pyld_x > 999999 || pyld_x == 0) {
	    res = 1;
	    BID_RETURN (res);
	  }
	  if (pyld_y > 999999 || pyld_y == 0) {
	    // if y is zero, x must be less than or numerically equal
	    res = 0;
	    BID_RETURN (res);
	  }
	  res = (pyld_x <= pyld_y);
	  BID_RETURN (res);
	} else {
	  // return true if y is +qNaN and x is +sNaN 
	  // (we know they're different bc of xor if_stmt above)
	  res = ((x & MASK_SNAN32) == MASK_SNAN32);
	  BID_RETURN (res);
	}
      }
    }
  } else if ((y & MASK_NAN32) == MASK_NAN32) {
    // x is certainly not NAN in this case.
    // return true if y is positive
    res = ((y & MASK_SIGN32) != MASK_SIGN32);
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
  if (((x & MASK_SIGN32) == MASK_SIGN32) ^ 
        ((y & MASK_SIGN32) == MASK_SIGN32)) {
    res = (x & MASK_SIGN32) == MASK_SIGN32;
    BID_RETURN (res);
  } // from this point on x and y have the same sign
  // INFINITY (CASE4)
  if ((x & MASK_INF32) == MASK_INF32) {
    // if x==neg_inf, return (y == neg_inf)?1:0;
    if ((x & MASK_SIGN32) == MASK_SIGN32) {
      res = 1;
      BID_RETURN (res);
    } else {
      // x is positive infinity, only return1 if y 
      // is positive infinity as well
      // (we know y has same sign as x)
      res = ((y & MASK_INF32) == MASK_INF32);
      BID_RETURN (res);
    }
  } else if ((y & MASK_INF32) == MASK_INF32) {
    // x is finite, so:
    //    if y is +inf, x<y
    //    if y is -inf, x>y
    res = ((y & MASK_SIGN32) != MASK_SIGN32);
    BID_RETURN (res);
  }
  // if steering bits are 11 (condition will be 0), then exponent is G[0:w+1]
  if ((x & MASK_STEERING_BITS32) == MASK_STEERING_BITS32) {
    exp_x = (x & MASK_BINARY_EXPONENT2_32) >> 21;
    sig_x = (x & MASK_BINARY_SIG2_32) | MASK_BINARY_OR2_32;
    if (sig_x > 9999999 || sig_x == 0) {
      x_is_zero = 1;
    }
  } else {
    exp_x = (x & MASK_BINARY_EXPONENT1_32) >> 23;
    sig_x = (x & MASK_BINARY_SIG1_32);
    if (sig_x == 0) {
      x_is_zero = 1;
    }
  }

  // if steering bits are 11 (condition will be 0), then exponent is G[0:w+1]
  if ((y & MASK_STEERING_BITS32) == MASK_STEERING_BITS32) {
    exp_y = (y & MASK_BINARY_EXPONENT2_32) >> 21;
    sig_y = (y & MASK_BINARY_SIG2_32) | MASK_BINARY_OR2_32;
    if (sig_y > 9999999 || sig_y == 0) {
      y_is_zero = 1;
    }
  } else {
    exp_y = (y & MASK_BINARY_EXPONENT1_32) >> 23;
    sig_y = (y & MASK_BINARY_SIG1_32);
    if (sig_y == 0) {
      y_is_zero = 1;
    }
  }

  // ZERO (CASE 5)
  // if x and y represent the same entities, and 
  // both are negative , return true iff exp_x <= exp_y
  if (x_is_zero && y_is_zero) {
    // the signs are the same:
    // totalOrder(x,y) iff exp_x >= exp_y for negative numbers
    // totalOrder(x,y) iff exp_x <= exp_y for positive numbers
    if (exp_x == exp_y) {
      res = 1;
      BID_RETURN (res);
    }
    res = (exp_x <= exp_y) ^ ((x & MASK_SIGN32) == MASK_SIGN32);
    BID_RETURN (res);
  }
  // if x is zero and y isn't, clearly x has the smaller payload.
  if (x_is_zero) {
    res = ((y & MASK_SIGN32) != MASK_SIGN32);
    BID_RETURN (res);
  }
  // if y is zero, and x isn't, clearly y has the smaller payload.
  if (y_is_zero) {
    res = ((x & MASK_SIGN32) == MASK_SIGN32);
    BID_RETURN (res);
  }
  // REDUNDANT REPRESENTATIONS (CASE6)
  // if both components are either bigger or smaller, 
  // it is clear what needs to be done
  if (sig_x > sig_y && exp_x >= exp_y) {
    res = ((x & MASK_SIGN32) == MASK_SIGN32);
    BID_RETURN (res);
  }
  if (sig_x < sig_y && exp_x <= exp_y) {
    res = ((x & MASK_SIGN32) != MASK_SIGN32);
    BID_RETURN (res);
  }
  // if exp_x is 6 greater than exp_y, it is 
  // definitely larger, so no need for compensation
  if (exp_x - exp_y > 6) {
    // difference cannot be greater than 10^6
    res = ((x & MASK_SIGN32) == MASK_SIGN32);
    BID_RETURN (res);
  }
  // if exp_x is 6 less than exp_y, it is 
  // definitely smaller, no need for compensation
  if (exp_y - exp_x > 6) {
    res = ((x & MASK_SIGN32) != MASK_SIGN32);
    BID_RETURN (res);
  }
  // if |exp_x - exp_y| < 6, it comes down 
  // to the compensated significand
  if (exp_x > exp_y) {
    // otherwise adjust the x significand upwards
    sig_n_prime = (BID_UINT64)sig_x * (BID_UINT64)bid_mult_factor[exp_x - exp_y];
    // if x and y represent the same entities, 
    // and both are negative, return true iff exp_x <= exp_y
    if (sig_n_prime == (BID_UINT64)sig_y) {
      // case cannot occur, because all bits must 
      // be the same - would have been caught if (x==y)
      res = (exp_x <= exp_y) ^ ((x & MASK_SIGN32) == MASK_SIGN32);
      BID_RETURN (res);
    }
    // if positive, return 1 if adjusted x is smaller than y
    res = (sig_n_prime < (BID_UINT64)sig_y) ^ ((x & MASK_SIGN32) == MASK_SIGN32);
    BID_RETURN (res);
  }
  // adjust the y significand upwards
  sig_n_prime = (BID_UINT64)sig_y * (BID_UINT64)bid_mult_factor[exp_y - exp_x];

  // if x and y represent the same entities, 
  // and both are negative, return true iff exp_x <= exp_y
  if (sig_n_prime == (BID_UINT64)sig_x) {
    // Cannot occur, because all bits must be the same. 
    // Case would have been caught if (x==y)
    res = (exp_x <= exp_y) ^ ((x & MASK_SIGN32) == MASK_SIGN32);
    BID_RETURN (res);
  }
  // values are not equal, for positive numbers return 1 
  // if x is less than y.  0 otherwise
  res = ((BID_UINT64)sig_x < sig_n_prime) ^ ((x & MASK_SIGN32) == MASK_SIGN32);
  BID_RETURN (res);
}


// totalOrderMag is TotalOrder(abs(x), abs(y))
#if DECIMAL_CALL_BY_REFERENCE
void
bid32_totalOrderMag (int *pres, BID_UINT32 * px, BID_UINT32 * py 
    _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
  BID_UINT32 x = *px;
  BID_UINT32 y = *py;
#else
RES_WRAPFN_DFP_DFP(int, bid32_totalOrderMag, 32, 32)
int
bid32_totalOrderMag (BID_UINT32 x, BID_UINT32 y _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
#endif
  int res;
  int exp_x, exp_y;
  BID_UINT32 sig_x, sig_y, pyld_y, pyld_x;
  BID_UINT64 sig_n_prime;
  char x_is_zero = 0, y_is_zero = 0;

  // NaN (CASE 1)
  // if x and y are unordered numerically because either operand is NaN
  //    (1) totalOrder(number, +NaN) is true
  //    (2) if x and y are both NaN:
  //       i) signaling < quiet for +NaN
  //       ii) lesser payload < greater payload for +NaN
  //       iii) else if bitwise identical (in canonical form), return 1
  if ((x & MASK_NAN32) == MASK_NAN32) {
    // x is +NaN

    // return false, unless y is +NaN also
    if ((y & MASK_NAN32) != MASK_NAN32) {
      res = 0;	// y is a number, return 1
      BID_RETURN (res);

    } else {

      // x and y are both +NaN; 
      // must investigate payload if both quiet or both signaling
      // this xnor statement will be true if both x and y are +qNaN or +sNaN
      if (!(((y & MASK_SNAN32) == MASK_SNAN32) ^ 
          ((x & MASK_SNAN32) == MASK_SNAN32))) {
	// it comes down to the payload.  we want to return true if x has a
	// smaller payload, or if the payloads are equal (canonical forms
	// are bitwise identical)
	pyld_y = y & 0x000fffff;
	pyld_x = x & 0x000fffff;
	// if x is zero and y isn't, x has the smaller 
	// payload definitely (since we know y isn't 0 at this point)
	if (pyld_x > 999999 || pyld_x == 0) {
	  res = 1;
	  BID_RETURN (res);
	}

	if (pyld_y > 999999 || pyld_y == 0) {
	  // if y is zero, x must be less than or numerically equal
	  res = 0;
	  BID_RETURN (res);
	}
	res = (pyld_x <= pyld_y);
	BID_RETURN (res);

      } else {
	// return true if y is +qNaN and x is +sNaN 
	// (we know they're different bc of xor if_stmt above)
	res = ((x & MASK_SNAN32) == MASK_SNAN32);
	BID_RETURN (res);
      }
    }

  } else if ((y & MASK_NAN32) == MASK_NAN32) {
    // x is certainly not NAN in this case.
    // return true if y is positive
    res = 1;
    BID_RETURN (res);
  }
  // SIMPLE (CASE2)
  // if all the bits (except sign bit) are the same, 
  // these numbers are equal.
  if ((x & ~MASK_SIGN32) == (y & ~MASK_SIGN32)) {
    res = 1;
    BID_RETURN (res);
  }
  // INFINITY (CASE3)
  if ((x & MASK_INF32) == MASK_INF32) {
    // x is positive infinity; return 1 only 
    // if y is positive infinity as well
    res = ((y & MASK_INF32) == MASK_INF32);
    BID_RETURN (res);
  } else if ((y & MASK_INF32) == MASK_INF32) {
    // x is finite, so:
    //    if y is +inf, x<y
    res = 1;
    BID_RETURN (res);
  }
  // if steering bits are 11 (condition will be 0), then exponent is G[0:w+1]
  if ((x & MASK_STEERING_BITS32) == MASK_STEERING_BITS32) {
    exp_x = (x & MASK_BINARY_EXPONENT2_32) >> 21;
    sig_x = (x & MASK_BINARY_SIG2_32) | MASK_BINARY_OR2_32;
    if (sig_x > 9999999 || sig_x == 0) {
      x_is_zero = 1;
      sig_x = 0;
    }
  } else {
    exp_x = (x & MASK_BINARY_EXPONENT1_32) >> 23;
    sig_x = (x & MASK_BINARY_SIG1_32);
    if (sig_x == 0) {
      x_is_zero = 1;
      sig_x = 0;
    }
  }

  // if steering bits are 11 (condition will be 0), then exponent is G[0:w+1]
  if ((y & MASK_STEERING_BITS32) == MASK_STEERING_BITS32) {
    exp_y = (y & MASK_BINARY_EXPONENT2_32) >> 21;
    sig_y = (y & MASK_BINARY_SIG2_32) | MASK_BINARY_OR2_32;
    if (sig_y > 9999999 || sig_y == 0) {
      y_is_zero = 1;
      sig_y = 0;
    }
  } else {
    exp_y = (y & MASK_BINARY_EXPONENT1_32) >> 23;
    sig_y = (y & MASK_BINARY_SIG1_32);
    if (sig_y == 0) {
      y_is_zero = 1;
      sig_y = 0;
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
  // if exp_x is 6 greater than exp_y, it is definitely 
  // larger, so no need for compensation
  if (exp_x - exp_y > 6) {
    res = 0;	// difference cannot be greater than 10^6
    BID_RETURN (res);
  }
  // if exp_x is 6 less than exp_y, it is definitely 
  // smaller, no need for compensation
  if (exp_y - exp_x > 6) {
    res = 1;
    BID_RETURN (res);
  }
  // if |exp_x - exp_y| < 6, it comes down 
  // to the compensated significand
  if (exp_x > exp_y) {
    // adjust the x significand upwards
    sig_n_prime = (BID_UINT64)sig_x * (BID_UINT64)bid_mult_factor[exp_x - exp_y];
    // if x and y represent the same entities
    // and both are negative, return true iff exp_x <= exp_y
    if (sig_n_prime == (BID_UINT64)sig_y) {
      // case cannot occur, because all bits 
      // must be the same - would have been caught if (x==y)
      res = 0; // res = (exp_x <= exp_y); but 0 < exp_x - exp_y <= 5
      BID_RETURN (res);
    }
    // if positive, return 1 if adjusted x is smaller than y
    res = (sig_n_prime < (BID_UINT64)sig_y);
    BID_RETURN (res);
  } // from this point on -5 <= exp_x - exp_y <= 0
  // adjust the y significand upwards
  sig_n_prime = (BID_UINT64)sig_y * (BID_UINT64)bid_mult_factor[exp_y - exp_x];

  // if x and y represent the same entities, 
  // and both are negative, return true iff exp_x <= exp_y
  if (sig_n_prime == (BID_UINT64)sig_x) {
    res = 1; // res = (exp_x <= exp_y); but -5 <= exp_x - exp_y <= 0
    BID_RETURN (res);
  }
  // values are not equal, for positive numbers 
  // return 1 if x is less than y, and 0 otherwise
  res = ((BID_UINT64)sig_x < sig_n_prime);
  BID_RETURN (res);
}


#if DECIMAL_CALL_BY_REFERENCE
void
bid32_radix (int *pres, BID_UINT32 * px _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
  BID_UINT32 x = *px;
#else
RES_WRAPFN_DFP(int, bid32_radix, 32)
int
bid32_radix (BID_UINT32 x _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
#endif
  int res;
  if (x) // dummy test
    res = 10;
  else
    res = 10;
  BID_RETURN (res);
}

#if DECIMAL_CALL_BY_REFERENCE
void bid32_inf (BID_UINT32 *pres) {
#else
BID_UINT32 bid32_inf (void) {
#endif
 
  BID_UINT32 res; 
  res = 0x78000000; // + inf
  BID_RETURN(res);
}


DFP_WRAPFN_OTHERTYPE(32, bid32_nan, const char *);

#if DECIMAL_CALL_BY_REFERENCE
void bid32_nan (BID_UINT32 *pres, const char *tagp) {
#else
BID_UINT32 bid32_nan (const char *tagp) {
#endif
 
  BID_UINT32 res, x;

#if !DECIMAL_GLOBAL_ROUNDING
  unsigned int rnd_mode = BID_ROUNDING_TO_NEAREST;
#endif
#if !DECIMAL_GLOBAL_EXCEPTION_FLAGS
  unsigned int fpsf;
  unsigned int *pfpsf = &fpsf;
#endif

  res = 0x7c000000; // +QNaN
  if (!tagp) BID_RETURN(res);

#if DECIMAL_CALL_BY_REFERENCE
  bid32_from_string (&x, (char *)tagp _RND_MODE_ARG _EXC_FLAGS_ARG);
#else      
  x = bid32_from_string ((char *)tagp _RND_MODE_ARG _EXC_FLAGS_ARG);
#endif
  x = x & 0x000fffff; // valid values fit in 20 bits
  res = res | x;

  BID_RETURN(res);
}


