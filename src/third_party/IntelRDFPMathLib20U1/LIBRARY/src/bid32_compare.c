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


BID_TYPE_FUNCTION_ARG2_CUSTOMRESULT_NORND(int, bid32_quiet_equal, BID_UINT32, x, y)

  int res;
  int exp_x, exp_y, exp_t;
  BID_UINT32 sig_x, sig_y, sig_t;
  char x_is_zero = 0, y_is_zero = 0, non_canon_x, non_canon_y, lcv;

  // NaN (CASE1)
  // if either number is NAN, the comparison is unordered, 
  // rather than equal : return 0
  if (((x & MASK_NAN32) == MASK_NAN32) || ((y & MASK_NAN32) == MASK_NAN32)) {
    if ((x & MASK_SNAN32) == MASK_SNAN32 || (y & MASK_SNAN32) == MASK_SNAN32) {
      *pfpsf |= BID_INVALID_EXCEPTION;	// set exception if sNaN
    }
    res = 0;
    BID_RETURN (res);
  }
  // SIMPLE (CASE2)
  // if all the bits are the same, these numbers are equivalent.
  if (x == y) {
    res = 1;
    BID_RETURN (res);
  }
  // INFINITY (CASE3)
  if (((x & MASK_INF32) == MASK_INF32) && ((y & MASK_INF32) == MASK_INF32)) {
    res = (((x ^ y) & MASK_SIGN32) != MASK_SIGN32);
    BID_RETURN (res);
  }
  // ONE INFINITY (CASE3')
  if (((x & MASK_INF32) == MASK_INF32) || ((y & MASK_INF32) == MASK_INF32)) {
    res = 0;
    BID_RETURN (res);
  }
  // if steering bits are 11 (condition will be 0), then exponent is G[0:w+1] =>
  if ((x & MASK_STEERING_BITS32) == MASK_STEERING_BITS32) {
    exp_x = (x & MASK_BINARY_EXPONENT2_32) >> 21;
    sig_x = (x & MASK_BINARY_SIG2_32) | MASK_BINARY_OR2_32;
    if (sig_x > 9999999) {
      non_canon_x = 1;
    } else {
      non_canon_x = 0;
    }
  } else {
    exp_x = (x & MASK_BINARY_EXPONENT1_32) >> 23;
    sig_x = (x & MASK_BINARY_SIG1_32);
    non_canon_x = 0;
  }
  // if steering bits are 11 (condition will be 0), then exponent is G[0:w+1] =>
  if ((y & MASK_STEERING_BITS32) == MASK_STEERING_BITS32) {
    exp_y = (y & MASK_BINARY_EXPONENT2_32) >> 21;
    sig_y = (y & MASK_BINARY_SIG2_32) | MASK_BINARY_OR2_32;
    if (sig_y > 9999999) {
      non_canon_y = 1;
    } else {
      non_canon_y = 0;
    }
  } else {
    exp_y = (y & MASK_BINARY_EXPONENT1_32) >> 23;
    sig_y = (y & MASK_BINARY_SIG1_32);
    non_canon_y = 0;
  }
  // ZERO (CASE4)
  // some properties:
  // (+ZERO==-ZERO) => therefore ignore the sign
  //    (ZERO x 10^A == ZERO x 10^B) for any valid A, B => 
  //    therefore ignore the exponent field
  //    (Any non-canonical # is considered 0)
  if (non_canon_x || sig_x == 0) {
    x_is_zero = 1;
  }
  if (non_canon_y || sig_y == 0) {
    y_is_zero = 1;
  }
  if (x_is_zero && y_is_zero) {
    res = 1;
    BID_RETURN (res);
  } else if ((x_is_zero && !y_is_zero) || (!x_is_zero && y_is_zero)) {
    res = 0;
    BID_RETURN (res);
  }
  // OPPOSITE SIGN (CASE5)
  // now, if the sign bits differ => not equal : return 0
  if ((x ^ y) & MASK_SIGN32) {
    res = 0;
    BID_RETURN (res);
  }
  // REDUNDANT REPRESENTATIONS (CASE6)
  if (exp_x > exp_y) {	// to simplify the loop below,
    SWAP (exp_x, exp_y, exp_t);	// put the larger exp in y,
    SWAP (sig_x, sig_y, sig_t);	// and the smaller exp in x
  }
  if (exp_y - exp_x > 6) {
    res = 0;	// difference cannot be greater than 10^6
    BID_RETURN (res);
  }
  for (lcv = 0; lcv < (exp_y - exp_x); lcv++) {
    // recalculate y's significand upwards
    sig_y = sig_y * 10;
    if (sig_y > 9999999) {
      res = 0;
      BID_RETURN (res);
    }
  }
  res = (sig_y == sig_x);
  BID_RETURN (res);
}


BID_TYPE_FUNCTION_ARG2_CUSTOMRESULT_NORND(int, bid32_quiet_greater, BID_UINT32, x, y)

  int res;
  int exp_x, exp_y;
  BID_UINT32 sig_x, sig_y;
  BID_UINT64 sig_n_prime;
  char x_is_zero = 0, y_is_zero = 0, non_canon_x, non_canon_y;

  // NaN (CASE1)
  // if either number is NAN, the comparison is unordered, rather than equal : 
  // return 0
  if (((x & MASK_NAN32) == MASK_NAN32) || ((y & MASK_NAN32) == MASK_NAN32)) {
    if ((x & MASK_SNAN32) == MASK_SNAN32 || (y & MASK_SNAN32) == MASK_SNAN32) {
      *pfpsf |= BID_INVALID_EXCEPTION;	// set exception if sNaN
    }
    res = 0;
    BID_RETURN (res);
  }
  // SIMPLE (CASE2)
  // if all the bits are the same, these numbers are equal (not Greater).
  if (x == y) {
    res = 0;
    BID_RETURN (res);
  }
  // INFINITY (CASE3)
  if ((x & MASK_INF32) == MASK_INF32) {
    // if x is neg infinity, there is no way it is greater than y, return 0
    if (((x & MASK_SIGN32) == MASK_SIGN32)) {
      res = 0;
      BID_RETURN (res);
    } else {
      // x is pos infinity, it is greater, unless y is positive 
      // infinity => return y!=pos_infinity
      res = (((y & MASK_INF32) != MASK_INF32)
	     || ((y & MASK_SIGN32) == MASK_SIGN32));
      BID_RETURN (res);
    }
  } else if ((y & MASK_INF32) == MASK_INF32) {
    // x is finite, so if y is positive infinity, then x is less, return 0
    //                 if y is negative infinity, then x is greater, return 1
    res = ((y & MASK_SIGN32) == MASK_SIGN32);
    BID_RETURN (res);
  }
  // if steering bits are 11 (condition will be 0), then exponent is G[0:w+1] =>
  if ((x & MASK_STEERING_BITS32) == MASK_STEERING_BITS32) {
    exp_x = (x & MASK_BINARY_EXPONENT2_32) >> 21;
    sig_x = (x & MASK_BINARY_SIG2_32) | MASK_BINARY_OR2_32;
    if (sig_x > 9999999) {
      non_canon_x = 1;
    } else {
      non_canon_x = 0;
    }
  } else {
    exp_x = (x & MASK_BINARY_EXPONENT1_32) >> 23;
    sig_x = (x & MASK_BINARY_SIG1_32);
    non_canon_x = 0;
  }
  // if steering bits are 11 (condition will be 0), then exponent is G[0:w+1] =>
  if ((y & MASK_STEERING_BITS32) == MASK_STEERING_BITS32) {
    exp_y = (y & MASK_BINARY_EXPONENT2_32) >> 21;
    sig_y = (y & MASK_BINARY_SIG2_32) | MASK_BINARY_OR2_32;
    if (sig_y > 9999999) {
      non_canon_y = 1;
    } else {
      non_canon_y = 0;
    }
  } else {
    exp_y = (y & MASK_BINARY_EXPONENT1_32) >> 23;
    sig_y = (y & MASK_BINARY_SIG1_32);
    non_canon_y = 0;
  }
  // ZERO (CASE4)
  // some properties:
  //(+ZERO==-ZERO) => therefore ignore the sign, and neither number is greater
  //(ZERO x 10^A == ZERO x 10^B) for any valid A, B => therefore ignore the 
  // exponent field
  // (Any non-canonical # is considered 0)
  if (non_canon_x || sig_x == 0) {
    x_is_zero = 1;
  }
  if (non_canon_y || sig_y == 0) {
    y_is_zero = 1;
  }
  // if both numbers are zero, neither is greater => return NOTGREATERTHAN
  if (x_is_zero && y_is_zero) {
    res = 0;
    BID_RETURN (res);
  } else if (x_is_zero) {
    // is x is zero, it is greater if Y is negative
    res = ((y & MASK_SIGN32) == MASK_SIGN32);
    BID_RETURN (res);
  } else if (y_is_zero) {
    // is y is zero, X is greater if it is positive
    res = ((x & MASK_SIGN32) != MASK_SIGN32);
    BID_RETURN (res);
  }
  // OPPOSITE SIGN (CASE5)
  // now, if the sign bits differ, x is greater if y is negative
  if (((x ^ y) & MASK_SIGN32) == MASK_SIGN32) {
    res = ((y & MASK_SIGN32) == MASK_SIGN32);
    BID_RETURN (res);
  }
  // REDUNDANT REPRESENTATIONS (CASE6)
  // if both components are either bigger or smaller, 
  // it is clear what needs to be done
  if (sig_x > sig_y && exp_x > exp_y) {
    res = ((x & MASK_SIGN32) != MASK_SIGN32);
    BID_RETURN (res);
  }
  if (sig_x < sig_y && exp_x < exp_y) {
    res = ((x & MASK_SIGN32) == MASK_SIGN32);
    BID_RETURN (res);
  }
  // if exp_x is 6 greater than exp_y, no need for compensation
  if (exp_x - exp_y > 6) {	// difference cannot be greater than 10^6
    if (x & MASK_SIGN32)	// if both are negative
      res = 0;
    else	// if both are positive
      res = 1;
    BID_RETURN (res);
  }
  // if exp_x is 6 less than exp_y, no need for compensation
  if (exp_y - exp_x > 6) {
    if (x & MASK_SIGN32)	// if both are negative
      res = 1;
    else	// if both are positive
      res = 0;
    BID_RETURN (res);
  }
  // if |exp_x - exp_y| < 6, it comes down to the compensated significand
  if (exp_x > exp_y) {	// to simplify the loop below,
    // otherwise adjust the x significand upwards
    sig_n_prime = (BID_UINT64)sig_x * (BID_UINT64)bid_mult_factor[exp_x - exp_y];
    // if postitive, return whichever significand is larger (converse if neg.)
    if (sig_n_prime == sig_y) {
      res = 0;
      BID_RETURN (res);
    }
    res = ((sig_n_prime > sig_y) ^ ((x & MASK_SIGN32) == MASK_SIGN32));
    BID_RETURN (res);
  }
  // adjust the y significand upwards
  sig_n_prime = (BID_UINT64)sig_y * (BID_UINT64)bid_mult_factor[exp_y - exp_x];
  // if postitive, return whichever significand is larger 
  //     (converse if negative)
  if (sig_n_prime == sig_x) {
    res = 0;
    BID_RETURN (res);
  }
  res = ((sig_x > sig_n_prime) ^ ((x & MASK_SIGN32) == MASK_SIGN32));
  BID_RETURN (res);
}


BID_TYPE_FUNCTION_ARG2_CUSTOMRESULT_NORND(int, bid32_quiet_greater_equal, BID_UINT32, x, y)

  int res;
  int exp_x, exp_y;
  BID_UINT32 sig_x, sig_y;
  BID_UINT64 sig_n_prime;
  char x_is_zero = 0, y_is_zero = 0, non_canon_x, non_canon_y;

  // NaN (CASE1)
  // if either number is NAN, the comparison is unordered : return 1
  if (((x & MASK_NAN32) == MASK_NAN32) || ((y & MASK_NAN32) == MASK_NAN32)) {
    if ((x & MASK_SNAN32) == MASK_SNAN32 || (y & MASK_SNAN32) == MASK_SNAN32) {
      *pfpsf |= BID_INVALID_EXCEPTION;	// set exception if sNaN
    }
    res = 0;
    BID_RETURN (res);
  }
  // SIMPLE (CASE2)
  // if all the bits are the same, these numbers are equal.
  if (x == y) {
    res = 1;
    BID_RETURN (res);
  }
  // INFINITY (CASE3)
  if ((x & MASK_INF32) == MASK_INF32) {
    // if x==neg_inf, { res = (y == neg_inf)?1:0; BID_RETURN (res) }
    if ((x & MASK_SIGN32) == MASK_SIGN32) {
      // x is -inf, so it is less than y unless y is -inf
      res = (((y & MASK_INF32) == MASK_INF32)
	     && (y & MASK_SIGN32) == MASK_SIGN32);
      BID_RETURN (res);
    } else {	// x is pos_inf, no way for it to be less than y
      res = 1;
      BID_RETURN (res);
    }
  } else if ((y & MASK_INF32) == MASK_INF32) {
    // x is finite, so:
    //    if y is +inf, x<y
    //    if y is -inf, x>y
    res = ((y & MASK_SIGN32) == MASK_SIGN32);
    BID_RETURN (res);
  }
  // if steering bits are 11 (condition will be 0), then exponent is G[0:w+1] =>
  if ((x & MASK_STEERING_BITS32) == MASK_STEERING_BITS32) {
    exp_x = (x & MASK_BINARY_EXPONENT2_32) >> 21;
    sig_x = (x & MASK_BINARY_SIG2_32) | MASK_BINARY_OR2_32;
    if (sig_x > 9999999) {
      non_canon_x = 1;
    } else {
      non_canon_x = 0;
    }
  } else {
    exp_x = (x & MASK_BINARY_EXPONENT1_32) >> 23;
    sig_x = (x & MASK_BINARY_SIG1_32);
    non_canon_x = 0;
  }
  // if steering bits are 11 (condition will be 0), then exponent is G[0:w+1] =>
  if ((y & MASK_STEERING_BITS32) == MASK_STEERING_BITS32) {
    exp_y = (y & MASK_BINARY_EXPONENT2_32) >> 21;
    sig_y = (y & MASK_BINARY_SIG2_32) | MASK_BINARY_OR2_32;
    if (sig_y > 9999999) {
      non_canon_y = 1;
    } else {
      non_canon_y = 0;
    }
  } else {
    exp_y = (y & MASK_BINARY_EXPONENT1_32) >> 23;
    sig_y = (y & MASK_BINARY_SIG1_32);
    non_canon_y = 0;
  }
  // ZERO (CASE4)
  // some properties:
  // (+ZERO==-ZERO) => therefore ignore the sign, and neither number is greater
  // (ZERO x 10^A == ZERO x 10^B) for any valid A, B => 
  //   therefore ignore the exponent field
  //  (Any non-canonical # is considered 0)
  if (non_canon_x || sig_x == 0) {
    x_is_zero = 1;
  }
  if (non_canon_y || sig_y == 0) {
    y_is_zero = 1;
  }
  if (x_is_zero && y_is_zero) {
    // if both numbers are zero, they are equal
    res = 1;
    BID_RETURN (res);
  } else if (x_is_zero) {
    // if x is zero, it is lessthan if Y is positive
    res = ((y & MASK_SIGN32) == MASK_SIGN32);
    BID_RETURN (res);
  } else if (y_is_zero) {
    // if y is zero, X is less if it is negative
    res = ((x & MASK_SIGN32) != MASK_SIGN32);
    BID_RETURN (res);
  }
  // OPPOSITE SIGN (CASE5)
  // now, if the sign bits differ, x is less than if y is positive
  if (((x ^ y) & MASK_SIGN32) == MASK_SIGN32) {
    res = ((y & MASK_SIGN32) == MASK_SIGN32);
    BID_RETURN (res);
  }
  // REDUNDANT REPRESENTATIONS (CASE6)
  // if both components are either bigger or smaller
  if (sig_x > sig_y && exp_x >= exp_y) {
    res = ((x & MASK_SIGN32) != MASK_SIGN32);
    BID_RETURN (res);
  }
  if (sig_x < sig_y && exp_x <= exp_y) {
    res = ((x & MASK_SIGN32) == MASK_SIGN32);
    BID_RETURN (res);
  }
  // if exp_x is 6 greater than exp_y, no need for compensation
  if (exp_x - exp_y > 6) {
    res = ((x & MASK_SIGN32) != MASK_SIGN32);
    // difference cannot be greater than 10^6
    BID_RETURN (res);
  }
  // if exp_x is 6 less than exp_y, no need for compensation
  if (exp_y - exp_x > 6) {
    res = ((x & MASK_SIGN32) == MASK_SIGN32);
    BID_RETURN (res);
  }
  // if |exp_x - exp_y| < 6, it comes down to the compensated significand
  if (exp_x > exp_y) {	// to simplify the loop below,
    // otherwise adjust the x significand upwards
    sig_n_prime = (BID_UINT64)sig_x * (BID_UINT64)bid_mult_factor[exp_x - exp_y];
    // return 1 if values are equal
    if (sig_n_prime == sig_y) {
      res = 1;
      BID_RETURN (res);
    }
    // if postitive, return whichever significand abs is smaller 
    // (converse if negative)
    res = ((sig_n_prime < sig_y) ^ ((x & MASK_SIGN32) != MASK_SIGN32));
    BID_RETURN (res);
  }
  // adjust the y significand upwards
  sig_n_prime = (BID_UINT64)sig_y * (BID_UINT64)bid_mult_factor[exp_y - exp_x];
  // return 0 if values are equal
  if (sig_n_prime == sig_x) {
    res = 1;
    BID_RETURN (res);
  }
  // if positive, return whichever significand abs is smaller 
  // (converse if negative)
  res = ((sig_x < sig_n_prime) ^ ((x & MASK_SIGN32) != MASK_SIGN32));
  BID_RETURN (res);
}

BID_TYPE_FUNCTION_ARG2_CUSTOMRESULT_NORND(int, bid32_quiet_greater_unordered, BID_UINT32, x, y)

  int res;
  int exp_x, exp_y;
  BID_UINT32 sig_x, sig_y;
  BID_UINT64 sig_n_prime;
  char x_is_zero = 0, y_is_zero = 0, non_canon_x, non_canon_y;

  // NaN (CASE1)
  // if either number is NAN, the comparison is unordered, rather than equal : 
  // return 0
  if (((x & MASK_NAN32) == MASK_NAN32) || ((y & MASK_NAN32) == MASK_NAN32)) {
    if ((x & MASK_SNAN32) == MASK_SNAN32 || (y & MASK_SNAN32) == MASK_SNAN32) {
      *pfpsf |= BID_INVALID_EXCEPTION;	// set exception if sNaN
    }
    res = 1;
    BID_RETURN (res);
  }
  // SIMPLE (CASE2)
  // if all the bits are the same, these numbers are equal (not Greater).
  if (x == y) {
    res = 0;
    BID_RETURN (res);
  }
  // INFINITY (CASE3)
  if ((x & MASK_INF32) == MASK_INF32) {
    // if x is neg infinity, there is no way it is greater than y, return 0
    if (((x & MASK_SIGN32) == MASK_SIGN32)) {
      res = 0;
      BID_RETURN (res);
    } else {
      // x is pos infinity, it is greater, unless y is positive infinity => 
      // return y!=pos_infinity
      res = (((y & MASK_INF32) != MASK_INF32)
	     || ((y & MASK_SIGN32) == MASK_SIGN32));
      BID_RETURN (res);
    }
  } else if ((y & MASK_INF32) == MASK_INF32) {
    // x is finite, so if y is positive infinity, then x is less, return 0
    //                 if y is negative infinity, then x is greater, return 1
    res = ((y & MASK_SIGN32) == MASK_SIGN32);
    BID_RETURN (res);
  }
  // if steering bits are 11 (condition will be 0), then exponent is G[0:w+1] =>
  if ((x & MASK_STEERING_BITS32) == MASK_STEERING_BITS32) {
    exp_x = (x & MASK_BINARY_EXPONENT2_32) >> 21;
    sig_x = (x & MASK_BINARY_SIG2_32) | MASK_BINARY_OR2_32;
    if (sig_x > 9999999) {
      non_canon_x = 1;
    } else {
      non_canon_x = 0;
    }
  } else {
    exp_x = (x & MASK_BINARY_EXPONENT1_32) >> 23;
    sig_x = (x & MASK_BINARY_SIG1_32);
    non_canon_x = 0;
  }
  // if steering bits are 11 (condition will be 0), then exponent is G[0:w+1] =>
  if ((y & MASK_STEERING_BITS32) == MASK_STEERING_BITS32) {
    exp_y = (y & MASK_BINARY_EXPONENT2_32) >> 21;
    sig_y = (y & MASK_BINARY_SIG2_32) | MASK_BINARY_OR2_32;
    if (sig_y > 9999999) {
      non_canon_y = 1;
    } else {
      non_canon_y = 0;
    }
  } else {
    exp_y = (y & MASK_BINARY_EXPONENT1_32) >> 23;
    sig_y = (y & MASK_BINARY_SIG1_32);
    non_canon_y = 0;
  }
  // ZERO (CASE4)
  // some properties:
  // (+ZERO==-ZERO) => therefore ignore the sign, and neither number is greater
  // (ZERO x 10^A == ZERO x 10^B) for any valid A, B => 
  // therefore ignore the exponent field
  //    (Any non-canonical # is considered 0)
  if (non_canon_x || sig_x == 0) {
    x_is_zero = 1;
  }
  if (non_canon_y || sig_y == 0) {
    y_is_zero = 1;
  }
  // if both numbers are zero, neither is greater => return NOTGREATERTHAN
  if (x_is_zero && y_is_zero) {
    res = 0;
    BID_RETURN (res);
  } else if (x_is_zero) {
    // is x is zero, it is greater if Y is negative
    res = ((y & MASK_SIGN32) == MASK_SIGN32);
    BID_RETURN (res);
  } else if (y_is_zero) {
    // is y is zero, X is greater if it is positive
    res = ((x & MASK_SIGN32) != MASK_SIGN32);
    BID_RETURN (res);
  }
  // OPPOSITE SIGN (CASE5)
  // now, if the sign bits differ, x is greater if y is negative
  if (((x ^ y) & MASK_SIGN32) == MASK_SIGN32) {
    res = ((y & MASK_SIGN32) == MASK_SIGN32);
    BID_RETURN (res);
  }
  // REDUNDANT REPRESENTATIONS (CASE6)
  // if both components are either bigger or smaller
  if (sig_x > sig_y && exp_x >= exp_y) {
    res = ((x & MASK_SIGN32) != MASK_SIGN32);
    BID_RETURN (res);
  }
  if (sig_x < sig_y && exp_x <= exp_y) {
    res = ((x & MASK_SIGN32) == MASK_SIGN32);
    BID_RETURN (res);
  }
  // if exp_x is 6 greater than exp_y, no need for compensation
  if (exp_x - exp_y > 6) {
    // difference cannot be greater than 10^6
    res = ((x & MASK_SIGN32) != MASK_SIGN32);
    BID_RETURN (res);
  }
  // if exp_x is 6 less than exp_y, no need for compensation
  if (exp_y - exp_x > 6) {
    res = ((x & MASK_SIGN32) == MASK_SIGN32);
    BID_RETURN (res);
  }
  // if |exp_x - exp_y| < 6, it comes down to the compensated significand
  if (exp_x > exp_y) {	// to simplify the loop below,
    // otherwise adjust the x significand upwards
    sig_n_prime = (BID_UINT64)sig_x * (BID_UINT64)bid_mult_factor[exp_x - exp_y];
    // if postitive, return whichever significand is larger 
    // (converse if negative)
    if (sig_n_prime == sig_y) {
      res = 0;
      BID_RETURN (res);
    }
    res = ((sig_n_prime > sig_y) ^ ((x & MASK_SIGN32) == MASK_SIGN32));
    BID_RETURN (res);
  }
  // adjust the y significand upwards
  sig_n_prime = (BID_UINT64)sig_y * (BID_UINT64)bid_mult_factor[exp_y - exp_x];
  // if postitive, return whichever significand is larger (converse if negative)
  if (sig_n_prime == sig_x) {
    res = 0;
    BID_RETURN (res);
  }
  res = ((sig_x > sig_n_prime) ^ ((x & MASK_SIGN32) == MASK_SIGN32));
  BID_RETURN (res);
}

BID_TYPE_FUNCTION_ARG2_CUSTOMRESULT_NORND(int, bid32_quiet_less, BID_UINT32, x, y)

  int res;
  int exp_x, exp_y;
  BID_UINT32 sig_x, sig_y;
  BID_UINT64 sig_n_prime;
  char x_is_zero = 0, y_is_zero = 0, non_canon_x, non_canon_y;

  // NaN (CASE1)
  // if either number is NAN, the comparison is unordered : return 0
  if (((x & MASK_NAN32) == MASK_NAN32) || ((y & MASK_NAN32) == MASK_NAN32)) {
    if ((x & MASK_SNAN32) == MASK_SNAN32 || (y & MASK_SNAN32) == MASK_SNAN32) {
      *pfpsf |= BID_INVALID_EXCEPTION;	// set exception if sNaN
    }
    res = 0;
    BID_RETURN (res);
  }
  // SIMPLE (CASE2)
  // if all the bits are the same, these numbers are equal.
  if (x == y) {
    res = 0;
    BID_RETURN (res);
  }
  // INFINITY (CASE3)
  if ((x & MASK_INF32) == MASK_INF32) {
    // if x==neg_inf, { res = (y == neg_inf)?0:1; BID_RETURN (res) }
    if ((x & MASK_SIGN32) == MASK_SIGN32) {
      // x is -inf, so it is less than y unless y is -inf
      res = (((y & MASK_INF32) != MASK_INF32)
	     || (y & MASK_SIGN32) != MASK_SIGN32);
      BID_RETURN (res);
    } else {
      // x is pos_inf, no way for it to be less than y
      res = 0;
      BID_RETURN (res);
    }
  } else if ((y & MASK_INF32) == MASK_INF32) {
    // x is finite, so:
    //    if y is +inf, x<y
    //    if y is -inf, x>y
    res = ((y & MASK_SIGN32) != MASK_SIGN32);
    BID_RETURN (res);
  }
  // if steering bits are 11 (condition will be 0), then exponent is G[0:w+1] =>
  if ((x & MASK_STEERING_BITS32) == MASK_STEERING_BITS32) {
    exp_x = (x & MASK_BINARY_EXPONENT2_32) >> 21;
    sig_x = (x & MASK_BINARY_SIG2_32) | MASK_BINARY_OR2_32;
    if (sig_x > 9999999) {
      non_canon_x = 1;
    } else {
      non_canon_x = 0;
    }
  } else {
    exp_x = (x & MASK_BINARY_EXPONENT1_32) >> 23;
    sig_x = (x & MASK_BINARY_SIG1_32);
    non_canon_x = 0;
  }
  // if steering bits are 11 (condition will be 0), then exponent is G[0:w+1] =>
  if ((y & MASK_STEERING_BITS32) == MASK_STEERING_BITS32) {
    exp_y = (y & MASK_BINARY_EXPONENT2_32) >> 21;
    sig_y = (y & MASK_BINARY_SIG2_32) | MASK_BINARY_OR2_32;
    if (sig_y > 9999999) {
      non_canon_y = 1;
    } else {
      non_canon_y = 0;
    }
  } else {
    exp_y = (y & MASK_BINARY_EXPONENT1_32) >> 23;
    sig_y = (y & MASK_BINARY_SIG1_32);
    non_canon_y = 0;
  }
  // ZERO (CASE4)
  // some properties:
  // (+ZERO==-ZERO) => therefore ignore the sign, and neither number is greater
  // (ZERO x 10^A == ZERO x 10^B) for any valid A, B => 
  //  therefore ignore the exponent field
  //    (Any non-canonical # is considered 0)
  if (non_canon_x || sig_x == 0) {
    x_is_zero = 1;
  }
  if (non_canon_y || sig_y == 0) {
    y_is_zero = 1;
  }
  if (x_is_zero && y_is_zero) {
    // if both numbers are zero, they are equal
    res = 0;
    BID_RETURN (res);
  } else if (x_is_zero) {
    // if x is zero, it is lessthan if Y is positive
    res = ((y & MASK_SIGN32) != MASK_SIGN32);
    BID_RETURN (res);
  } else if (y_is_zero) {
    // if y is zero, X is less if it is negative
    res = ((x & MASK_SIGN32) == MASK_SIGN32);
    BID_RETURN (res);
  }
  // OPPOSITE SIGN (CASE5)
  // now, if the sign bits differ, x is less than if y is positive
  if (((x ^ y) & MASK_SIGN32) == MASK_SIGN32) {
    res = ((y & MASK_SIGN32) != MASK_SIGN32);
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
  // if exp_x is 6 greater than exp_y, no need for compensation
  if (exp_x - exp_y > 6) {
    res = ((x & MASK_SIGN32) == MASK_SIGN32);
    // difference cannot be greater than 10^6
    BID_RETURN (res);
  }
  // if exp_x is 6 less than exp_y, no need for compensation
  if (exp_y - exp_x > 6) {
    res = ((x & MASK_SIGN32) != MASK_SIGN32);
    BID_RETURN (res);
  }
  // if |exp_x - exp_y| < 6, it comes down to the compensated significand
  if (exp_x > exp_y) {	// to simplify the loop below,
    // otherwise adjust the x significand upwards
    sig_n_prime = (BID_UINT64)sig_x * (BID_UINT64)bid_mult_factor[exp_x - exp_y];
    // return 0 if values are equal
    if (sig_n_prime == sig_y) {
      res = 0;
      BID_RETURN (res);
    }
    // if postitive, return whichever significand abs is smaller 
    // (converse if negative)
    res = ((sig_n_prime < sig_y) ^ ((x & MASK_SIGN32) == MASK_SIGN32));
    BID_RETURN (res);
  }
  // adjust the y significand upwards
  sig_n_prime = (BID_UINT64)sig_y * (BID_UINT64)bid_mult_factor[exp_y - exp_x];
  // return 0 if values are equal
  if (sig_n_prime == sig_x) {
    res = 0;
    BID_RETURN (res);
  }
  // if positive, return whichever significand abs is smaller 
  // (converse if negative)
  res = ((sig_x < sig_n_prime) ^ ((x & MASK_SIGN32) == MASK_SIGN32));
  BID_RETURN (res);
}

BID_TYPE_FUNCTION_ARG2_CUSTOMRESULT_NORND(int, bid32_quiet_less_equal, BID_UINT32, x, y)

  int res;
  int exp_x, exp_y;
  BID_UINT32 sig_x, sig_y;
  BID_UINT64 sig_n_prime;
  char x_is_zero = 0, y_is_zero = 0, non_canon_x, non_canon_y;

  // NaN (CASE1)
  // if either number is NAN, the comparison is unordered, rather than equal : 
  //     return 0
  if (((x & MASK_NAN32) == MASK_NAN32) || ((y & MASK_NAN32) == MASK_NAN32)) {
    if ((x & MASK_SNAN32) == MASK_SNAN32 || (y & MASK_SNAN32) == MASK_SNAN32) {
      *pfpsf |= BID_INVALID_EXCEPTION;	// set exception if sNaN
    }
    res = 0;
    BID_RETURN (res);
  }
  // SIMPLE (CASE2)
  // if all the bits are the same, these numbers are equal (LESSEQUAL).
  if (x == y) {
    res = 1;
    BID_RETURN (res);
  }
  // INFINITY (CASE3)
  if ((x & MASK_INF32) == MASK_INF32) {
    if (((x & MASK_SIGN32) == MASK_SIGN32)) {
      // if x is neg infinity, it must be lessthan or equal to y return 1
      res = 1;
      BID_RETURN (res);
    } else {
      // x is pos infinity, it is greater, unless y is positive infinity => 
      // return y==pos_infinity
      res = !(((y & MASK_INF32) != MASK_INF32)
	      || ((y & MASK_SIGN32) == MASK_SIGN32));
      BID_RETURN (res);
    }
  } else if ((y & MASK_INF32) == MASK_INF32) {
    // x is finite, so if y is positive infinity, then x is less, return 1
    //                 if y is negative infinity, then x is greater, return 0
    res = ((y & MASK_SIGN32) != MASK_SIGN32);
    BID_RETURN (res);
  }
  // if steering bits are 11 (condition will be 0), then exponent is G[0:w+1] =>
  if ((x & MASK_STEERING_BITS32) == MASK_STEERING_BITS32) {
    exp_x = (x & MASK_BINARY_EXPONENT2_32) >> 21;
    sig_x = (x & MASK_BINARY_SIG2_32) | MASK_BINARY_OR2_32;
    if (sig_x > 9999999) {
      non_canon_x = 1;
    } else {
      non_canon_x = 0;
    }
  } else {
    exp_x = (x & MASK_BINARY_EXPONENT1_32) >> 23;
    sig_x = (x & MASK_BINARY_SIG1_32);
    non_canon_x = 0;
  }
  // if steering bits are 11 (condition will be 0), then exponent is G[0:w+1] =>
  if ((y & MASK_STEERING_BITS32) == MASK_STEERING_BITS32) {
    exp_y = (y & MASK_BINARY_EXPONENT2_32) >> 21;
    sig_y = (y & MASK_BINARY_SIG2_32) | MASK_BINARY_OR2_32;
    if (sig_y > 9999999) {
      non_canon_y = 1;
    } else {
      non_canon_y = 0;
    }
  } else {
    exp_y = (y & MASK_BINARY_EXPONENT1_32) >> 23;
    sig_y = (y & MASK_BINARY_SIG1_32);
    non_canon_y = 0;
  }
  // ZERO (CASE4)
  // some properties:
  // (+ZERO==-ZERO) => therefore ignore the sign, and neither number is greater
  // (ZERO x 10^A == ZERO x 10^B) for any valid A, B => 
  //     therefore ignore the exponent field
  //    (Any non-canonical # is considered 0)
  if (non_canon_x || sig_x == 0) {
    x_is_zero = 1;
  }
  if (non_canon_y || sig_y == 0) {
    y_is_zero = 1;
  }
  if (x_is_zero && y_is_zero) {
    // if both numbers are zero, they are equal -> return 1
    res = 1;
    BID_RETURN (res);
  } else if (x_is_zero) {
    // if x is zero, it is lessthan if Y is positive
    res = ((y & MASK_SIGN32) != MASK_SIGN32);
    BID_RETURN (res);
  } else if (y_is_zero) {
    // if y is zero, X is less if it is negative
    res = ((x & MASK_SIGN32) == MASK_SIGN32);
    BID_RETURN (res);
  }
  // OPPOSITE SIGN (CASE5)
  // now, if the sign bits differ, x is less than if y is positive
  if (((x ^ y) & MASK_SIGN32) == MASK_SIGN32) {
    res = ((y & MASK_SIGN32) != MASK_SIGN32);
    BID_RETURN (res);
  }
  // REDUNDANT REPRESENTATIONS (CASE6)
  // if both components are either bigger or smaller
  if (sig_x > sig_y && exp_x >= exp_y) {
    res = ((x & MASK_SIGN32) == MASK_SIGN32);
    BID_RETURN (res);
  }
  if (sig_x < sig_y && exp_x <= exp_y) {
    res = ((x & MASK_SIGN32) != MASK_SIGN32);
    BID_RETURN (res);
  }
  // if exp_x is 6 greater than exp_y, no need for compensation
  if (exp_x - exp_y > 6) {
    res = ((x & MASK_SIGN32) == MASK_SIGN32);
    // difference cannot be greater than 10^6
    BID_RETURN (res);
  }
  // if exp_x is 6 less than exp_y, no need for compensation
  if (exp_y - exp_x > 6) {
    res = ((x & MASK_SIGN32) != MASK_SIGN32);
    BID_RETURN (res);
  }
  // if |exp_x - exp_y| < 6, it comes down to the compensated significand
  if (exp_x > exp_y) {	// to simplify the loop below,
    // otherwise adjust the x significand upwards
    sig_n_prime = (BID_UINT64)sig_x * (BID_UINT64)bid_mult_factor[exp_x - exp_y];
    // return 1 if values are equal
    if (sig_n_prime == sig_y) {
      res = 1;
      BID_RETURN (res);
    }
    // if postitive, return whichever significand abs is smaller 
    //     (converse if negative)
    res = ((sig_n_prime < sig_y) ^ ((x & MASK_SIGN32) == MASK_SIGN32));
    BID_RETURN (res);
  }
  // adjust the y significand upwards
  sig_n_prime = (BID_UINT64)sig_y * (BID_UINT64)bid_mult_factor[exp_y - exp_x];
  // return 1 if values are equal
  if (sig_n_prime == sig_x) {
    res = 1;
    BID_RETURN (res);
  }
  // if positive, return whichever significand abs is smaller 
  //     (converse if negative)
  res = ((sig_x < sig_n_prime) ^ ((x & MASK_SIGN32) == MASK_SIGN32));
  BID_RETURN (res);
}

BID_TYPE_FUNCTION_ARG2_CUSTOMRESULT_NORND(int, bid32_quiet_less_unordered, BID_UINT32, x, y)

  int res;
  int exp_x, exp_y;
  BID_UINT32 sig_x, sig_y;
  BID_UINT64 sig_n_prime;
  char x_is_zero = 0, y_is_zero = 0, non_canon_x, non_canon_y;

  // NaN (CASE1)
  // if either number is NAN, the comparison is unordered : return 0
  if (((x & MASK_NAN32) == MASK_NAN32) || ((y & MASK_NAN32) == MASK_NAN32)) {
    if ((x & MASK_SNAN32) == MASK_SNAN32 || (y & MASK_SNAN32) == MASK_SNAN32) {
      *pfpsf |= BID_INVALID_EXCEPTION;	// set exception if sNaN
    }
    res = 1;
    BID_RETURN (res);
  }
  // SIMPLE (CASE2)
  // if all the bits are the same, these numbers are equal.
  if (x == y) {
    res = 0;
    BID_RETURN (res);
  }
  // INFINITY (CASE3)
  if ((x & MASK_INF32) == MASK_INF32) {
    // if x==neg_inf, { res = (y == neg_inf)?0:1; BID_RETURN (res) }
    if ((x & MASK_SIGN32) == MASK_SIGN32) {
      // x is -inf, so it is less than y unless y is -inf
      res = (((y & MASK_INF32) != MASK_INF32)
	     || (y & MASK_SIGN32) != MASK_SIGN32);
      BID_RETURN (res);
    } else {
      // x is pos_inf, no way for it to be less than y
      res = 0;
      BID_RETURN (res);
    }
  } else if ((y & MASK_INF32) == MASK_INF32) {
    // x is finite, so:
    //    if y is +inf, x<y
    //    if y is -inf, x>y
    res = ((y & MASK_SIGN32) != MASK_SIGN32);
    BID_RETURN (res);
  }
  // if steering bits are 11 (condition will be 0), then exponent is G[0:w+1] =>
  if ((x & MASK_STEERING_BITS32) == MASK_STEERING_BITS32) {
    exp_x = (x & MASK_BINARY_EXPONENT2_32) >> 21;
    sig_x = (x & MASK_BINARY_SIG2_32) | MASK_BINARY_OR2_32;
    if (sig_x > 9999999) {
      non_canon_x = 1;
    } else {
      non_canon_x = 0;
    }
  } else {
    exp_x = (x & MASK_BINARY_EXPONENT1_32) >> 23;
    sig_x = (x & MASK_BINARY_SIG1_32);
    non_canon_x = 0;
  }
  // if steering bits are 11 (condition will be 0), then exponent is G[0:w+1] =>
  if ((y & MASK_STEERING_BITS32) == MASK_STEERING_BITS32) {
    exp_y = (y & MASK_BINARY_EXPONENT2_32) >> 21;
    sig_y = (y & MASK_BINARY_SIG2_32) | MASK_BINARY_OR2_32;
    if (sig_y > 9999999) {
      non_canon_y = 1;
    } else {
      non_canon_y = 0;
    }
  } else {
    exp_y = (y & MASK_BINARY_EXPONENT1_32) >> 23;
    sig_y = (y & MASK_BINARY_SIG1_32);
    non_canon_y = 0;
  }
  // ZERO (CASE4)
  // some properties:
  // (+ZERO==-ZERO) => therefore ignore the sign, and neither number is greater
  // (ZERO x 10^A == ZERO x 10^B) for any valid A, B => 
  //     therefore ignore the exponent field
  //    (Any non-canonical # is considered 0)
  if (non_canon_x || sig_x == 0) {
    x_is_zero = 1;
  }
  if (non_canon_y || sig_y == 0) {
    y_is_zero = 1;
  }
  if (x_is_zero && y_is_zero) {
    // if both numbers are zero, they are equal
    res = 0;
    BID_RETURN (res);
  } else if (x_is_zero) {
    // if x is zero, it is lessthan if Y is positive
    res = ((y & MASK_SIGN32) != MASK_SIGN32);
    BID_RETURN (res);
  } else if (y_is_zero) {
    // if y is zero, X is less if it is negative
    res = ((x & MASK_SIGN32) == MASK_SIGN32);
    BID_RETURN (res);
  }
  // OPPOSITE SIGN (CASE5)
  // now, if the sign bits differ, x is less than if y is positive
  if (((x ^ y) & MASK_SIGN32) == MASK_SIGN32) {
    res = ((y & MASK_SIGN32) != MASK_SIGN32);
    BID_RETURN (res);
  }
  // REDUNDANT REPRESENTATIONS (CASE6)
  // if both components are either bigger or smaller
  if (sig_x > sig_y && exp_x >= exp_y) {
    res = ((x & MASK_SIGN32) == MASK_SIGN32);
    BID_RETURN (res);
  }
  if (sig_x < sig_y && exp_x <= exp_y) {
    res = ((x & MASK_SIGN32) != MASK_SIGN32);
    BID_RETURN (res);
  }
  // if exp_x is 6 greater than exp_y, no need for compensation
  if (exp_x - exp_y > 6) {
    res = ((x & MASK_SIGN32) == MASK_SIGN32);
    // difference cannot be greater than 10^6
    BID_RETURN (res);
  }
  // if exp_x is 6 less than exp_y, no need for compensation
  if (exp_y - exp_x > 6) {
    res = ((x & MASK_SIGN32) != MASK_SIGN32);
    BID_RETURN (res);
  }
  // if |exp_x - exp_y| < 6, it comes down to the compensated significand
  if (exp_x > exp_y) {	// to simplify the loop below,
    // otherwise adjust the x significand upwards
    sig_n_prime = (BID_UINT64)sig_x * (BID_UINT64)bid_mult_factor[exp_x - exp_y];
    // return 0 if values are equal
    if (sig_n_prime == sig_y) {
      res = 0;
      BID_RETURN (res);
    }
    // if postitive, return whichever significand abs is smaller 
    //     (converse if negative)
    res = ((sig_n_prime < sig_y) ^ ((x & MASK_SIGN32) == MASK_SIGN32));
    BID_RETURN (res);
  }
  // adjust the y significand upwards
  sig_n_prime = (BID_UINT64)sig_y * (BID_UINT64)bid_mult_factor[exp_y - exp_x];
  // return 0 if values are equal
  if (sig_n_prime == sig_x) {
    res = 0;
    BID_RETURN (res);
  }
  // if positive, return whichever significand abs is smaller 
  //     (converse if negative)
  res = ((sig_x < sig_n_prime) ^ ((x & MASK_SIGN32) == MASK_SIGN32));
  BID_RETURN (res);
}

BID_TYPE_FUNCTION_ARG2_CUSTOMRESULT_NORND(int, bid32_quiet_not_equal, BID_UINT32, x, y)

  int res;
  int exp_x, exp_y, exp_t;
  BID_UINT32 sig_x, sig_y, sig_t;
  char x_is_zero = 0, y_is_zero = 0, non_canon_x, non_canon_y, lcv;

  // NaN (CASE1)
  // if either number is NAN, the comparison is unordered, 
  // rather than equal : return 1
  if (((x & MASK_NAN32) == MASK_NAN32) || ((y & MASK_NAN32) == MASK_NAN32)) {
    if ((x & MASK_SNAN32) == MASK_SNAN32 || (y & MASK_SNAN32) == MASK_SNAN32) {
      *pfpsf |= BID_INVALID_EXCEPTION;	// set exception if sNaN
    }
    res = 1;
    BID_RETURN (res);
  }
  // SIMPLE (CASE2)
  // if all the bits are the same, these numbers are equivalent.
  if (x == y) {
    res = 0;
    BID_RETURN (res);
  }
  // INFINITY (CASE3)
  if (((x & MASK_INF32) == MASK_INF32) && ((y & MASK_INF32) == MASK_INF32)) {
    res = (((x ^ y) & MASK_SIGN32) == MASK_SIGN32);
    BID_RETURN (res);
  }
  // ONE INFINITY (CASE3')
  if (((x & MASK_INF32) == MASK_INF32) || ((y & MASK_INF32) == MASK_INF32)) {
    res = 1;
    BID_RETURN (res);
  }
  // if steering bits are 11 (condition will be 0), then exponent is G[0:w+1] =>
  if ((x & MASK_STEERING_BITS32) == MASK_STEERING_BITS32) {
    exp_x = (x & MASK_BINARY_EXPONENT2_32) >> 21;
    sig_x = (x & MASK_BINARY_SIG2_32) | MASK_BINARY_OR2_32;
    if (sig_x > 9999999) {
      non_canon_x = 1;
    } else {
      non_canon_x = 0;
    }
  } else {
    exp_x = (x & MASK_BINARY_EXPONENT1_32) >> 23;
    sig_x = (x & MASK_BINARY_SIG1_32);
    non_canon_x = 0;
  }

  // if steering bits are 11 (condition will be 0), then exponent is G[0:w+1] =>
  if ((y & MASK_STEERING_BITS32) == MASK_STEERING_BITS32) {
    exp_y = (y & MASK_BINARY_EXPONENT2_32) >> 21;
    sig_y = (y & MASK_BINARY_SIG2_32) | MASK_BINARY_OR2_32;
    if (sig_y > 9999999) {
      non_canon_y = 1;
    } else {
      non_canon_y = 0;
    }
  } else {
    exp_y = (y & MASK_BINARY_EXPONENT1_32) >> 23;
    sig_y = (y & MASK_BINARY_SIG1_32);
    non_canon_y = 0;
  }

  // ZERO (CASE4)
  // some properties:
  // (+ZERO==-ZERO) => therefore ignore the sign
  //    (ZERO x 10^A == ZERO x 10^B) for any valid A, B => 
  //        therefore ignore the exponent field
  //    (Any non-canonical # is considered 0)
  if (non_canon_x || sig_x == 0) {
    x_is_zero = 1;
  }
  if (non_canon_y || sig_y == 0) {
    y_is_zero = 1;
  }

  if (x_is_zero && y_is_zero) {
    res = 0;
    BID_RETURN (res);
  } else if ((x_is_zero && !y_is_zero) || (!x_is_zero && y_is_zero)) {
    res = 1;
    BID_RETURN (res);
  }
  // OPPOSITE SIGN (CASE5)
  // now, if the sign bits differ => not equal : return 1
  if ((x ^ y) & MASK_SIGN32) {
    res = 1;
    BID_RETURN (res);
  }
  // REDUNDANT REPRESENTATIONS (CASE6)
  if (exp_x > exp_y) {	// to simplify the loop below,
    SWAP (exp_x, exp_y, exp_t);	// put the larger exp in y,
    SWAP (sig_x, sig_y, sig_t);	// and the smaller exp in x
  }

  if (exp_y - exp_x > 6) {
    res = 1;
    BID_RETURN (res);
  }
  // difference cannot be greater than 10^16

  for (lcv = 0; lcv < (exp_y - exp_x); lcv++) {

    // recalculate y's significand upwards
    sig_y = sig_y * 10;
    if (sig_y > 9999999) {
      res = 1;
      BID_RETURN (res);
    }
  }

  {
    res = sig_y != sig_x;
    BID_RETURN (res);
  }

}

BID_TYPE_FUNCTION_ARG2_CUSTOMRESULT_NORND(int, bid32_quiet_not_greater, BID_UINT32, x, y)

  int res;
  int exp_x, exp_y;
  BID_UINT32 sig_x, sig_y;
  BID_UINT64 sig_n_prime;
  char x_is_zero = 0, y_is_zero = 0, non_canon_x, non_canon_y;

  // NaN (CASE1)
  // if either number is NAN, the comparison is unordered, 
  //   rather than equal : return 0
  if (((x & MASK_NAN32) == MASK_NAN32) || ((y & MASK_NAN32) == MASK_NAN32)) {
    if ((x & MASK_SNAN32) == MASK_SNAN32 || (y & MASK_SNAN32) == MASK_SNAN32) {
      *pfpsf |= BID_INVALID_EXCEPTION;	// set exception if sNaN
    }
    res = 1;
    BID_RETURN (res);
  }
  // SIMPLE (CASE2)
  // if all the bits are the same, these numbers are equal (LESSEQUAL).
  if (x == y) {
    res = 1;
    BID_RETURN (res);
  }
  // INFINITY (CASE3)
  if ((x & MASK_INF32) == MASK_INF32) {
    // if x is neg infinity, it must be lessthan or equal to y return 1
    if (((x & MASK_SIGN32) == MASK_SIGN32)) {
      res = 1;
      BID_RETURN (res);
    }
    // x is pos infinity, it is greater, unless y is positive 
    // infinity => return y==pos_infinity
    else {
      res = !(((y & MASK_INF32) != MASK_INF32)
	      || ((y & MASK_SIGN32) == MASK_SIGN32));
      BID_RETURN (res);
    }
  } else if ((y & MASK_INF32) == MASK_INF32) {
    // x is finite, so if y is positive infinity, then x is less, return 1
    //                 if y is negative infinity, then x is greater, return 0
    {
      res = ((y & MASK_SIGN32) != MASK_SIGN32);
      BID_RETURN (res);
    }
  }
  // if steering bits are 11 (condition will be 0), then exponent is G[0:w+1] =>
  if ((x & MASK_STEERING_BITS32) == MASK_STEERING_BITS32) {
    exp_x = (x & MASK_BINARY_EXPONENT2_32) >> 21;
    sig_x = (x & MASK_BINARY_SIG2_32) | MASK_BINARY_OR2_32;
    if (sig_x > 9999999) {
      non_canon_x = 1;
    } else {
      non_canon_x = 0;
    }
  } else {
    exp_x = (x & MASK_BINARY_EXPONENT1_32) >> 23;
    sig_x = (x & MASK_BINARY_SIG1_32);
    non_canon_x = 0;
  }

  // if steering bits are 11 (condition will be 0), then exponent is G[0:w+1] =>
  if ((y & MASK_STEERING_BITS32) == MASK_STEERING_BITS32) {
    exp_y = (y & MASK_BINARY_EXPONENT2_32) >> 21;
    sig_y = (y & MASK_BINARY_SIG2_32) | MASK_BINARY_OR2_32;
    if (sig_y > 9999999) {
      non_canon_y = 1;
    } else {
      non_canon_y = 0;
    }
  } else {
    exp_y = (y & MASK_BINARY_EXPONENT1_32) >> 23;
    sig_y = (y & MASK_BINARY_SIG1_32);
    non_canon_y = 0;
  }

  // ZERO (CASE4)
  // some properties:
  // (+ZERO==-ZERO) => therefore ignore the sign, and neither 
  //         number is greater
  //    (ZERO x 10^A == ZERO x 10^B) for any valid A, B => 
  //         therefore ignore the exponent field
  //    (Any non-canonical # is considered 0)
  if (non_canon_x || sig_x == 0) {
    x_is_zero = 1;
  }
  if (non_canon_y || sig_y == 0) {
    y_is_zero = 1;
  }
  // if both numbers are zero, they are equal -> return 1
  if (x_is_zero && y_is_zero) {
    res = 1;
    BID_RETURN (res);
  }
  // if x is zero, it is lessthan if Y is positive
  else if (x_is_zero) {
    res = ((y & MASK_SIGN32) != MASK_SIGN32);
    BID_RETURN (res);
  }
  // if y is zero, X is less if it is negative
  else if (y_is_zero) {
    res = ((x & MASK_SIGN32) == MASK_SIGN32);
    BID_RETURN (res);
  }
  // OPPOSITE SIGN (CASE5)
  // now, if the sign bits differ, x is less than if y is positive
  if (((x ^ y) & MASK_SIGN32) == MASK_SIGN32) {
    res = ((y & MASK_SIGN32) != MASK_SIGN32);
    BID_RETURN (res);
  }
  // REDUNDANT REPRESENTATIONS (CASE6)
  // if both components are either bigger or smaller
  if (sig_x > sig_y && exp_x >= exp_y) {
    res = ((x & MASK_SIGN32) == MASK_SIGN32);
    BID_RETURN (res);
  }
  if (sig_x < sig_y && exp_x <= exp_y) {
    res = ((x & MASK_SIGN32) != MASK_SIGN32);
    BID_RETURN (res);
  }
  // if exp_x is 6 greater than exp_y, no need for compensation
  if (exp_x - exp_y > 6) {
    res = ((x & MASK_SIGN32) == MASK_SIGN32);
    BID_RETURN (res);
  }
  // difference cannot be greater than 10^6

  // if exp_x is 6 less than exp_y, no need for compensation
  if (exp_y - exp_x > 6) {
    res = ((x & MASK_SIGN32) != MASK_SIGN32);
    BID_RETURN (res);
  }
  // if |exp_x - exp_y| < 6, it comes down to the compensated significand
  if (exp_x > exp_y) {	// to simplify the loop below,

    // otherwise adjust the x significand upwards
    sig_n_prime = (BID_UINT64)sig_x * (BID_UINT64)bid_mult_factor[exp_x - exp_y];

    // return 1 if values are equal
    if (sig_n_prime == sig_y) {
      res = 1;
      BID_RETURN (res);
    }
    // if postitive, return whichever significand abs is smaller 
    //     (converse if negative)
    {
      res = ((sig_n_prime < sig_y) ^ ((x & MASK_SIGN32) == MASK_SIGN32));
      BID_RETURN (res);
    }
  }
  // adjust the y significand upwards
  sig_n_prime = (BID_UINT64)sig_y * (BID_UINT64)bid_mult_factor[exp_y - exp_x];

  // return 1 if values are equal
  if (sig_n_prime == sig_x) {
    res = 1;
    BID_RETURN (res);
  }
  // if positive, return whichever significand abs is smaller 
  //     (converse if negative)
  {
    res = ((sig_x < sig_n_prime) ^ ((x & MASK_SIGN32) == MASK_SIGN32));
    BID_RETURN (res);
  }
}

BID_TYPE_FUNCTION_ARG2_CUSTOMRESULT_NORND(int, bid32_quiet_not_less, BID_UINT32, x, y)

  int res;
  int exp_x, exp_y;
  BID_UINT32 sig_x, sig_y;
  BID_UINT64 sig_n_prime;
  char x_is_zero = 0, y_is_zero = 0, non_canon_x, non_canon_y;

  // NaN (CASE1)
  // if either number is NAN, the comparison is unordered : return 1
  if (((x & MASK_NAN32) == MASK_NAN32) || ((y & MASK_NAN32) == MASK_NAN32)) {
    if ((x & MASK_SNAN32) == MASK_SNAN32 || (y & MASK_SNAN32) == MASK_SNAN32) {
      *pfpsf |= BID_INVALID_EXCEPTION;	// set exception if sNaN
    }
    res = 1;
    BID_RETURN (res);
  }
  // SIMPLE (CASE2)
  // if all the bits are the same, these numbers are equal.
  if (x == y) {
    res = 1;
    BID_RETURN (res);
  }
  // INFINITY (CASE3)
  if ((x & MASK_INF32) == MASK_INF32) {
    // if x==neg_inf, { res = (y == neg_inf)?1:0; BID_RETURN (res) }
    if ((x & MASK_SIGN32) == MASK_SIGN32)
      // x is -inf, so it is less than y unless y is -inf
    {
      res = (((y & MASK_INF32) == MASK_INF32)
	     && (y & MASK_SIGN32) == MASK_SIGN32);
      BID_RETURN (res);
    } else
      // x is pos_inf, no way for it to be less than y
    {
      res = 1;
      BID_RETURN (res);
    }
  } else if ((y & MASK_INF32) == MASK_INF32) {
    // x is finite, so:
    //    if y is +inf, x<y
    //    if y is -inf, x>y
    {
      res = ((y & MASK_SIGN32) == MASK_SIGN32);
      BID_RETURN (res);
    }
  }
  // if steering bits are 11 (condition will be 0), then exponent is G[0:w+1] =>
  if ((x & MASK_STEERING_BITS32) == MASK_STEERING_BITS32) {
    exp_x = (x & MASK_BINARY_EXPONENT2_32) >> 21;
    sig_x = (x & MASK_BINARY_SIG2_32) | MASK_BINARY_OR2_32;
    if (sig_x > 9999999) {
      non_canon_x = 1;
    } else {
      non_canon_x = 0;
    }
  } else {
    exp_x = (x & MASK_BINARY_EXPONENT1_32) >> 23;
    sig_x = (x & MASK_BINARY_SIG1_32);
    non_canon_x = 0;
  }

  // if steering bits are 11 (condition will be 0), then exponent is G[0:w+1] =>
  if ((y & MASK_STEERING_BITS32) == MASK_STEERING_BITS32) {
    exp_y = (y & MASK_BINARY_EXPONENT2_32) >> 21;
    sig_y = (y & MASK_BINARY_SIG2_32) | MASK_BINARY_OR2_32;
    if (sig_y > 9999999) {
      non_canon_y = 1;
    } else {
      non_canon_y = 0;
    }
  } else {
    exp_y = (y & MASK_BINARY_EXPONENT1_32) >> 23;
    sig_y = (y & MASK_BINARY_SIG1_32);
    non_canon_y = 0;
  }

  // ZERO (CASE4)
  // some properties:
  // (+ZERO==-ZERO) => therefore ignore the sign, and neither 
  //        number is greater
  //    (ZERO x 10^A == ZERO x 10^B) for any valid A, B => 
  //        therefore ignore the exponent field
  //    (Any non-canonical # is considered 0)
  if (non_canon_x || sig_x == 0) {
    x_is_zero = 1;
  }
  if (non_canon_y || sig_y == 0) {
    y_is_zero = 1;
  }
  // if both numbers are zero, they are equal
  if (x_is_zero && y_is_zero) {
    res = 1;
    BID_RETURN (res);
  }
  // if x is zero, it is lessthan if Y is positive
  else if (x_is_zero) {
    res = ((y & MASK_SIGN32) == MASK_SIGN32);
    BID_RETURN (res);
  }
  // if y is zero, X is less if it is negative
  else if (y_is_zero) {
    res = ((x & MASK_SIGN32) != MASK_SIGN32);
    BID_RETURN (res);
  }
  // OPPOSITE SIGN (CASE5)
  // now, if the sign bits differ, x is less than if y is positive
  if (((x ^ y) & MASK_SIGN32) == MASK_SIGN32) {
    res = ((y & MASK_SIGN32) == MASK_SIGN32);
    BID_RETURN (res);
  }
  // REDUNDANT REPRESENTATIONS (CASE6)
  // if both components are either bigger or smaller
  if (sig_x > sig_y && exp_x >= exp_y) {
    res = ((x & MASK_SIGN32) != MASK_SIGN32);
    BID_RETURN (res);
  }
  if (sig_x < sig_y && exp_x <= exp_y) {
    res = ((x & MASK_SIGN32) == MASK_SIGN32);
    BID_RETURN (res);
  }
  // if exp_x is 6 greater than exp_y, no need for compensation
  if (exp_x - exp_y > 6) {
    res = ((x & MASK_SIGN32) != MASK_SIGN32);
    BID_RETURN (res);
  }
  // difference cannot be greater than 10^6

  // if exp_x is 6 less than exp_y, no need for compensation
  if (exp_y - exp_x > 6) {
    res = ((x & MASK_SIGN32) == MASK_SIGN32);
    BID_RETURN (res);
  }
  // if |exp_x - exp_y| < 6, it comes down to the compensated significand
  if (exp_x > exp_y) {	// to simplify the loop below,

    // otherwise adjust the x significand upwards
    sig_n_prime = (BID_UINT64)sig_x * (BID_UINT64)bid_mult_factor[exp_x - exp_y];

    // return 0 if values are equal
    if (sig_n_prime == sig_y) {
      res = 1;
      BID_RETURN (res);
    }
    // if postitive, return whichever significand abs is smaller 
    //     (converse if negative)
    {
      res = ((sig_n_prime < sig_y) ^ ((x & MASK_SIGN32) != MASK_SIGN32));
      BID_RETURN (res);
    }
  }
  // adjust the y significand upwards
  sig_n_prime = (BID_UINT64)sig_y * (BID_UINT64)bid_mult_factor[exp_y - exp_x];

  // return 0 if values are equal
  if (sig_n_prime == sig_x) {
    res = 1;
    BID_RETURN (res);
  }
  // if positive, return whichever significand abs is smaller 
  //     (converse if negative)
  {
    res = ((sig_x < sig_n_prime) ^ ((x & MASK_SIGN32) != MASK_SIGN32));
    BID_RETURN (res);
  }
}

BID_TYPE_FUNCTION_ARG2_CUSTOMRESULT_NORND(int, bid32_quiet_ordered, BID_UINT32, x, y)

  int res;

  // NaN (CASE1)
  // if either number is NAN, the comparison is ordered, rather than equal : return 0
  if (((x & MASK_NAN32) == MASK_NAN32) || ((y & MASK_NAN32) == MASK_NAN32)) {
    if ((x & MASK_SNAN32) == MASK_SNAN32 || (y & MASK_SNAN32) == MASK_SNAN32) {
      *pfpsf |= BID_INVALID_EXCEPTION;	// set exception if sNaN
    }
    res = 0;
    BID_RETURN (res);
  } else {
    res = 1;
    BID_RETURN (res);
  }
}

BID_TYPE_FUNCTION_ARG2_CUSTOMRESULT_NORND(int, bid32_quiet_unordered, BID_UINT32, x, y)

  int res;

  // NaN (CASE1)
  // if either number is NAN, the comparison is unordered, 
  //     rather than equal : return 0
  if (((x & MASK_NAN32) == MASK_NAN32) || ((y & MASK_NAN32) == MASK_NAN32)) {
    if ((x & MASK_SNAN32) == MASK_SNAN32 || (y & MASK_SNAN32) == MASK_SNAN32) {
      *pfpsf |= BID_INVALID_EXCEPTION;	// set exception if sNaN
    }
    res = 1;
    BID_RETURN (res);
  } else {
    res = 0;
    BID_RETURN (res);
  }
}

BID_TYPE_FUNCTION_ARG2_CUSTOMRESULT_NORND(int, bid32_signaling_greater, BID_UINT32, x, y)

  int res;
  int exp_x, exp_y;
  BID_UINT32 sig_x, sig_y;
  BID_UINT64 sig_n_prime;
  char x_is_zero = 0, y_is_zero = 0, non_canon_x, non_canon_y;

  // NaN (CASE1)
  // if either number is NAN, the comparison is unordered, 
  //     rather than equal : return 0
  if (((x & MASK_NAN32) == MASK_NAN32) || ((y & MASK_NAN32) == MASK_NAN32)) {
    *pfpsf |= BID_INVALID_EXCEPTION;	// set invalid exception if NaN
    res = 0;
    BID_RETURN (res);
  }
  // SIMPLE (CASE2)
  // if all the bits are the same, these numbers are equal (not Greater).
  if (x == y) {
    res = 0;
    BID_RETURN (res);
  }
  // INFINITY (CASE3)
  if ((x & MASK_INF32) == MASK_INF32) {
    // if x is neg infinity, there is no way it is greater than y, return 0
    if (((x & MASK_SIGN32) == MASK_SIGN32)) {
      res = 0;
      BID_RETURN (res);
    }
    // x is pos infinity, it is greater, 
    // unless y is positive infinity => return y!=pos_infinity
    else {
      res = (((y & MASK_INF32) != MASK_INF32)
	     || ((y & MASK_SIGN32) == MASK_SIGN32));
      BID_RETURN (res);
    }
  } else if ((y & MASK_INF32) == MASK_INF32) {
    // x is finite, so if y is positive infinity, then x is less, return 0
    //                 if y is negative infinity, then x is greater, return 1
    {
      res = ((y & MASK_SIGN32) == MASK_SIGN32);
      BID_RETURN (res);
    }
  }
  // if steering bits are 11 (condition will be 0), then exponent is G[0:w+1] =>
  if ((x & MASK_STEERING_BITS32) == MASK_STEERING_BITS32) {
    exp_x = (x & MASK_BINARY_EXPONENT2_32) >> 21;
    sig_x = (x & MASK_BINARY_SIG2_32) | MASK_BINARY_OR2_32;
    if (sig_x > 9999999) {
      non_canon_x = 1;
    } else {
      non_canon_x = 0;
    }
  } else {
    exp_x = (x & MASK_BINARY_EXPONENT1_32) >> 23;
    sig_x = (x & MASK_BINARY_SIG1_32);
    non_canon_x = 0;
  }

  // if steering bits are 11 (condition will be 0), then exponent is G[0:w+1] =>
  if ((y & MASK_STEERING_BITS32) == MASK_STEERING_BITS32) {
    exp_y = (y & MASK_BINARY_EXPONENT2_32) >> 21;
    sig_y = (y & MASK_BINARY_SIG2_32) | MASK_BINARY_OR2_32;
    if (sig_y > 9999999) {
      non_canon_y = 1;
    } else {
      non_canon_y = 0;
    }
  } else {
    exp_y = (y & MASK_BINARY_EXPONENT1_32) >> 23;
    sig_y = (y & MASK_BINARY_SIG1_32);
    non_canon_y = 0;
  }

  // ZERO (CASE4)
  // some properties:
  // (+ZERO==-ZERO) => therefore ignore the sign, and neither number is greater
  //    (ZERO x 10^A == ZERO x 10^B) for any valid A, B => 
  //      therefore ignore the exponent field
  //    (Any non-canonical # is considered 0)
  if (non_canon_x || sig_x == 0) {
    x_is_zero = 1;
  }
  if (non_canon_y || sig_y == 0) {
    y_is_zero = 1;
  }
  // if both numbers are zero, neither is greater => return NOTGREATERTHAN
  if (x_is_zero && y_is_zero) {
    res = 0;
    BID_RETURN (res);
  }
  // is x is zero, it is greater if Y is negative
  else if (x_is_zero) {
    res = ((y & MASK_SIGN32) == MASK_SIGN32);
    BID_RETURN (res);
  }
  // is y is zero, X is greater if it is positive
  else if (y_is_zero) {
    res = ((x & MASK_SIGN32) != MASK_SIGN32);
    BID_RETURN (res);
  }
  // OPPOSITE SIGN (CASE5)
  // now, if the sign bits differ, x is greater if y is negative
  if (((x ^ y) & MASK_SIGN32) == MASK_SIGN32) {
    res = ((y & MASK_SIGN32) == MASK_SIGN32);
    BID_RETURN (res);
  }
  // REDUNDANT REPRESENTATIONS (CASE6)

  // if both components are either bigger or smaller
  if (sig_x > sig_y && exp_x >= exp_y) {
    res = ((x & MASK_SIGN32) != MASK_SIGN32);
    BID_RETURN (res);
  }
  if (sig_x < sig_y && exp_x <= exp_y) {
    res = ((x & MASK_SIGN32) == MASK_SIGN32);
    BID_RETURN (res);
  }
  // if exp_x is 6 greater than exp_y, no need for compensation
  if (exp_x - exp_y > 6) {
    res = ((x & MASK_SIGN32) != MASK_SIGN32);
    BID_RETURN (res);
  }
  // difference cannot be greater than 10^6

  // if exp_x is 6 less than exp_y, no need for compensation
  if (exp_y - exp_x > 6) {
    res = ((x & MASK_SIGN32) == MASK_SIGN32);
    BID_RETURN (res);
  }
  // if |exp_x - exp_y| < 6, it comes down to the compensated significand
  if (exp_x > exp_y) {	// to simplify the loop below,

    // otherwise adjust the x significand upwards
    sig_n_prime = (BID_UINT64)sig_x * (BID_UINT64)bid_mult_factor[exp_x - exp_y];


    // if postitive, return whichever significand is larger 
    //     (converse if negative)
    if (sig_n_prime == sig_y) {
      res = 0;
      BID_RETURN (res);
    }

    {
      res = ((sig_n_prime > sig_y) ^ ((x & MASK_SIGN32) == MASK_SIGN32));
      BID_RETURN (res);
    }
  }
  // adjust the y significand upwards
  sig_n_prime = (BID_UINT64)sig_y * (BID_UINT64)bid_mult_factor[exp_y - exp_x];

  // if postitive, return whichever significand is larger 
  //     (converse if negative)
  if (sig_n_prime == sig_x) {
    res = 0;
    BID_RETURN (res);
  }
  {
    res = ((sig_x > sig_n_prime) ^ ((x & MASK_SIGN32) == MASK_SIGN32));
    BID_RETURN (res);
  }
}

BID_TYPE_FUNCTION_ARG2_CUSTOMRESULT_NORND(int, bid32_signaling_greater_equal, BID_UINT32, x, y)

  int res;
  int exp_x, exp_y;
  BID_UINT32 sig_x, sig_y;
  BID_UINT64 sig_n_prime;
  char x_is_zero = 0, y_is_zero = 0, non_canon_x, non_canon_y;

  // NaN (CASE1)
  // if either number is NAN, the comparison is unordered : return 1
  if (((x & MASK_NAN32) == MASK_NAN32) || ((y & MASK_NAN32) == MASK_NAN32)) {
    *pfpsf |= BID_INVALID_EXCEPTION;	// set invalid exception if NaN
    res = 0;
    BID_RETURN (res);
  }
  // SIMPLE (CASE2)
  // if all the bits are the same, these numbers are equal.
  if (x == y) {
    res = 1;
    BID_RETURN (res);
  }
  // INFINITY (CASE3)
  if ((x & MASK_INF32) == MASK_INF32) {
    // if x==neg_inf, { res = (y == neg_inf)?1:0; BID_RETURN (res) }
    if ((x & MASK_SIGN32) == MASK_SIGN32)
      // x is -inf, so it is less than y unless y is -inf
    {
      res = (((y & MASK_INF32) == MASK_INF32)
	     && (y & MASK_SIGN32) == MASK_SIGN32);
      BID_RETURN (res);
    } else
      // x is pos_inf, no way for it to be less than y
    {
      res = 1;
      BID_RETURN (res);
    }
  } else if ((y & MASK_INF32) == MASK_INF32) {
    // x is finite, so:
    //    if y is +inf, x<y
    //    if y is -inf, x>y
    {
      res = ((y & MASK_SIGN32) == MASK_SIGN32);
      BID_RETURN (res);
    }
  }
  // if steering bits are 11 (condition will be 0), then exponent is G[0:w+1] =>
  if ((x & MASK_STEERING_BITS32) == MASK_STEERING_BITS32) {
    exp_x = (x & MASK_BINARY_EXPONENT2_32) >> 21;
    sig_x = (x & MASK_BINARY_SIG2_32) | MASK_BINARY_OR2_32;
    if (sig_x > 9999999) {
      non_canon_x = 1;
    } else {
      non_canon_x = 0;
    }
  } else {
    exp_x = (x & MASK_BINARY_EXPONENT1_32) >> 23;
    sig_x = (x & MASK_BINARY_SIG1_32);
    non_canon_x = 0;
  }

  // if steering bits are 11 (condition will be 0), then exponent is G[0:w+1] =>
  if ((y & MASK_STEERING_BITS32) == MASK_STEERING_BITS32) {
    exp_y = (y & MASK_BINARY_EXPONENT2_32) >> 21;
    sig_y = (y & MASK_BINARY_SIG2_32) | MASK_BINARY_OR2_32;
    if (sig_y > 9999999) {
      non_canon_y = 1;
    } else {
      non_canon_y = 0;
    }
  } else {
    exp_y = (y & MASK_BINARY_EXPONENT1_32) >> 23;
    sig_y = (y & MASK_BINARY_SIG1_32);
    non_canon_y = 0;
  }

  // ZERO (CASE4)
  // some properties:
  // (+ZERO==-ZERO) => therefore ignore the sign, and neither number is greater
  //    (ZERO x 10^A == ZERO x 10^B) for any valid A, B => 
  //      therefore ignore the exponent field
  //    (Any non-canonical # is considered 0)
  if (non_canon_x || sig_x == 0) {
    x_is_zero = 1;
  }
  if (non_canon_y || sig_y == 0) {
    y_is_zero = 1;
  }
  // if both numbers are zero, they are equal
  if (x_is_zero && y_is_zero) {
    res = 1;
    BID_RETURN (res);
  }
  // if x is zero, it is lessthan if Y is positive
  else if (x_is_zero) {
    res = ((y & MASK_SIGN32) == MASK_SIGN32);
    BID_RETURN (res);
  }
  // if y is zero, X is less if it is negative
  else if (y_is_zero) {
    res = ((x & MASK_SIGN32) != MASK_SIGN32);
    BID_RETURN (res);
  }
  // OPPOSITE SIGN (CASE5)
  // now, if the sign bits differ, x is less than if y is positive
  if (((x ^ y) & MASK_SIGN32) == MASK_SIGN32) {
    res = ((y & MASK_SIGN32) == MASK_SIGN32);
    BID_RETURN (res);
  }
  // REDUNDANT REPRESENTATIONS (CASE6)
  // if both components are either bigger or smaller
  if (sig_x > sig_y && exp_x >= exp_y) {
    res = ((x & MASK_SIGN32) != MASK_SIGN32);
    BID_RETURN (res);
  }
  if (sig_x < sig_y && exp_x <= exp_y) {
    res = ((x & MASK_SIGN32) == MASK_SIGN32);
    BID_RETURN (res);
  }
  // if exp_x is 6 greater than exp_y, no need for compensation
  if (exp_x - exp_y > 6) {
    res = ((x & MASK_SIGN32) != MASK_SIGN32);
    BID_RETURN (res);
  }
  // difference cannot be greater than 10^6

  // if exp_x is 6 less than exp_y, no need for compensation
  if (exp_y - exp_x > 6) {
    res = ((x & MASK_SIGN32) == MASK_SIGN32);
    BID_RETURN (res);
  }
  // if |exp_x - exp_y| < 6, it comes down to the compensated significand
  if (exp_x > exp_y) {	// to simplify the loop below,

    // otherwise adjust the x significand upwards
    sig_n_prime = (BID_UINT64)sig_x * (BID_UINT64)bid_mult_factor[exp_x - exp_y];

    // return 1 if values are equal
    if (sig_n_prime == sig_y) {
      res = 1;
      BID_RETURN (res);
    }
    // if postitive, return whichever significand abs is smaller 
    //     (converse if negative)
    {
      res = ((sig_n_prime < sig_y) ^ ((x & MASK_SIGN32) != MASK_SIGN32));
      BID_RETURN (res);
    }
  }
  // adjust the y significand upwards
  sig_n_prime = (BID_UINT64)sig_y * (BID_UINT64)bid_mult_factor[exp_y - exp_x];

  // return 0 if values are equal
  if (sig_n_prime == sig_x) {
    res = 1;
    BID_RETURN (res);
  }
  // if positive, return whichever significand abs is smaller 
  //     (converse if negative)
  {
    res = ((sig_x < sig_n_prime) ^ ((x & MASK_SIGN32) != MASK_SIGN32));
    BID_RETURN (res);
  }
}

BID_TYPE_FUNCTION_ARG2_CUSTOMRESULT_NORND(int, bid32_signaling_greater_unordered, BID_UINT32, x, y)

  int res;
  int exp_x, exp_y;
  BID_UINT32 sig_x, sig_y;
  BID_UINT64 sig_n_prime;
  char x_is_zero = 0, y_is_zero = 0, non_canon_x, non_canon_y;

  // NaN (CASE1)
  // if either number is NAN, the comparison is unordered, 
  // rather than equal : return 0
  if (((x & MASK_NAN32) == MASK_NAN32) || ((y & MASK_NAN32) == MASK_NAN32)) {
    *pfpsf |= BID_INVALID_EXCEPTION;	// set invalid exception if NaN
    res = 1;
    BID_RETURN (res);
  }
  // SIMPLE (CASE2)
  // if all the bits are the same, these numbers are equal (not Greater).
  if (x == y) {
    res = 0;
    BID_RETURN (res);
  }
  // INFINITY (CASE3)
  if ((x & MASK_INF32) == MASK_INF32) {
    // if x is neg infinity, there is no way it is greater than y, return 0
    if (((x & MASK_SIGN32) == MASK_SIGN32)) {
      res = 0;
      BID_RETURN (res);
    }
    // x is pos infinity, it is greater, 
    // unless y is positive infinity => return y!=pos_infinity
    else {
      res = (((y & MASK_INF32) != MASK_INF32)
	     || ((y & MASK_SIGN32) == MASK_SIGN32));
      BID_RETURN (res);
    }
  } else if ((y & MASK_INF32) == MASK_INF32) {
    // x is finite, so if y is positive infinity, then x is less, return 0
    //                 if y is negative infinity, then x is greater, return 1
    {
      res = ((y & MASK_SIGN32) == MASK_SIGN32);
      BID_RETURN (res);
    }
  }
  // if steering bits are 11 (condition will be 0), then exponent is G[0:w+1] =>
  if ((x & MASK_STEERING_BITS32) == MASK_STEERING_BITS32) {
    exp_x = (x & MASK_BINARY_EXPONENT2_32) >> 21;
    sig_x = (x & MASK_BINARY_SIG2_32) | MASK_BINARY_OR2_32;
    if (sig_x > 9999999) {
      non_canon_x = 1;
    } else {
      non_canon_x = 0;
    }
  } else {
    exp_x = (x & MASK_BINARY_EXPONENT1_32) >> 23;
    sig_x = (x & MASK_BINARY_SIG1_32);
    non_canon_x = 0;
  }

  // if steering bits are 11 (condition will be 0), then exponent is G[0:w+1] =>
  if ((y & MASK_STEERING_BITS32) == MASK_STEERING_BITS32) {
    exp_y = (y & MASK_BINARY_EXPONENT2_32) >> 21;
    sig_y = (y & MASK_BINARY_SIG2_32) | MASK_BINARY_OR2_32;
    if (sig_y > 9999999) {
      non_canon_y = 1;
    } else {
      non_canon_y = 0;
    }
  } else {
    exp_y = (y & MASK_BINARY_EXPONENT1_32) >> 23;
    sig_y = (y & MASK_BINARY_SIG1_32);
    non_canon_y = 0;
  }

  // ZERO (CASE4)
  // some properties:
  // (+ZERO==-ZERO) => therefore ignore the sign, and neither number is greater
  //    (ZERO x 10^A == ZERO x 10^B) for any valid A, B => 
  //      therefore ignore the exponent field
  //    (Any non-canonical # is considered 0)
  if (non_canon_x || sig_x == 0) {
    x_is_zero = 1;
  }
  if (non_canon_y || sig_y == 0) {
    y_is_zero = 1;
  }
  // if both numbers are zero, neither is greater => return NOTGREATERTHAN
  if (x_is_zero && y_is_zero) {
    res = 0;
    BID_RETURN (res);
  }
  // is x is zero, it is greater if Y is negative
  else if (x_is_zero) {
    res = ((y & MASK_SIGN32) == MASK_SIGN32);
    BID_RETURN (res);
  }
  // is y is zero, X is greater if it is positive
  else if (y_is_zero) {
    res = ((x & MASK_SIGN32) != MASK_SIGN32);
    BID_RETURN (res);
  }
  // OPPOSITE SIGN (CASE5)
  // now, if the sign bits differ, x is greater if y is negative
  if (((x ^ y) & MASK_SIGN32) == MASK_SIGN32) {
    res = ((y & MASK_SIGN32) == MASK_SIGN32);
    BID_RETURN (res);
  }
  // REDUNDANT REPRESENTATIONS (CASE6)

  // if both components are either bigger or smaller
  if (sig_x > sig_y && exp_x >= exp_y) {
    res = ((x & MASK_SIGN32) != MASK_SIGN32);
    BID_RETURN (res);
  }
  if (sig_x < sig_y && exp_x <= exp_y) {
    res = ((x & MASK_SIGN32) == MASK_SIGN32);
    BID_RETURN (res);
  }
  // if exp_x is 6 greater than exp_y, no need for compensation
  if (exp_x - exp_y > 6) {
    res = ((x & MASK_SIGN32) != MASK_SIGN32);
    BID_RETURN (res);
  }
  // difference cannot be greater than 10^6

  // if exp_x is 6 less than exp_y, no need for compensation
  if (exp_y - exp_x > 6) {
    res = ((x & MASK_SIGN32) == MASK_SIGN32);
    BID_RETURN (res);
  }
  // if |exp_x - exp_y| < 6, it comes down to the compensated significand
  if (exp_x > exp_y) {	// to simplify the loop below,

    // otherwise adjust the x significand upwards
    sig_n_prime = (BID_UINT64)sig_x * (BID_UINT64)bid_mult_factor[exp_x - exp_y];

    // if postitive, return whichever significand is larger 
    //     (converse if negative)
    if (sig_n_prime == sig_y) {
      res = 0;
      BID_RETURN (res);
    }

    {
      res = ((sig_n_prime > sig_y) ^ ((x & MASK_SIGN32) == MASK_SIGN32));
      BID_RETURN (res);
    }
  }
  // adjust the y significand upwards
  sig_n_prime = (BID_UINT64)sig_y * (BID_UINT64)bid_mult_factor[exp_y - exp_x];

  // if postitive, return whichever significand is larger 
  //     (converse if negative)
  if (sig_n_prime == sig_x) {
    res = 0;
    BID_RETURN (res);
  }
  {
    res = ((sig_x > sig_n_prime) ^ ((x & MASK_SIGN32) == MASK_SIGN32));
    BID_RETURN (res);
  }
}

BID_TYPE_FUNCTION_ARG2_CUSTOMRESULT_NORND(int, bid32_signaling_less, BID_UINT32, x, y)

  int res;
  int exp_x, exp_y;
  BID_UINT32 sig_x, sig_y;
  BID_UINT64 sig_n_prime;
  char x_is_zero = 0, y_is_zero = 0, non_canon_x, non_canon_y;

  // NaN (CASE1)
  // if either number is NAN, the comparison is unordered : return 0
  if (((x & MASK_NAN32) == MASK_NAN32) || ((y & MASK_NAN32) == MASK_NAN32)) {
    *pfpsf |= BID_INVALID_EXCEPTION;	// set invalid exception if NaN
    res = 0;
    BID_RETURN (res);
  }
  // SIMPLE (CASE2)
  // if all the bits are the same, these numbers are equal.
  if (x == y) {
    res = 0;
    BID_RETURN (res);
  }
  // INFINITY (CASE3)
  if ((x & MASK_INF32) == MASK_INF32) {
    // if x==neg_inf, { res = (y == neg_inf)?0:1; BID_RETURN (res) }
    if ((x & MASK_SIGN32) == MASK_SIGN32)
      // x is -inf, so it is less than y unless y is -inf
    {
      res = (((y & MASK_INF32) != MASK_INF32)
	     || (y & MASK_SIGN32) != MASK_SIGN32);
      BID_RETURN (res);
    } else
      // x is pos_inf, no way for it to be less than y
    {
      res = 0;
      BID_RETURN (res);
    }
  } else if ((y & MASK_INF32) == MASK_INF32) {
    // x is finite, so:
    //    if y is +inf, x<y
    //    if y is -inf, x>y
    {
      res = ((y & MASK_SIGN32) != MASK_SIGN32);
      BID_RETURN (res);
    }
  }
  // if steering bits are 11 (condition will be 0), then exponent is G[0:w+1] =>
  if ((x & MASK_STEERING_BITS32) == MASK_STEERING_BITS32) {
    exp_x = (x & MASK_BINARY_EXPONENT2_32) >> 21;
    sig_x = (x & MASK_BINARY_SIG2_32) | MASK_BINARY_OR2_32;
    if (sig_x > 9999999) {
      non_canon_x = 1;
    } else {
      non_canon_x = 0;
    }
  } else {
    exp_x = (x & MASK_BINARY_EXPONENT1_32) >> 23;
    sig_x = (x & MASK_BINARY_SIG1_32);
    non_canon_x = 0;
  }

  // if steering bits are 11 (condition will be 0), then exponent is G[0:w+1] =>
  if ((y & MASK_STEERING_BITS32) == MASK_STEERING_BITS32) {
    exp_y = (y & MASK_BINARY_EXPONENT2_32) >> 21;
    sig_y = (y & MASK_BINARY_SIG2_32) | MASK_BINARY_OR2_32;
    if (sig_y > 9999999) {
      non_canon_y = 1;
    } else {
      non_canon_y = 0;
    }
  } else {
    exp_y = (y & MASK_BINARY_EXPONENT1_32) >> 23;
    sig_y = (y & MASK_BINARY_SIG1_32);
    non_canon_y = 0;
  }

  // ZERO (CASE4)
  // some properties:
  // (+ZERO==-ZERO) => therefore ignore the sign, and neither number is greater
  //    (ZERO x 10^A == ZERO x 10^B) for any valid A, B => 
  //      therefore ignore the exponent field
  //    (Any non-canonical # is considered 0)
  if (non_canon_x || sig_x == 0) {
    x_is_zero = 1;
  }
  if (non_canon_y || sig_y == 0) {
    y_is_zero = 1;
  }
  // if both numbers are zero, they are equal
  if (x_is_zero && y_is_zero) {
    res = 0;
    BID_RETURN (res);
  }
  // if x is zero, it is lessthan if Y is positive
  else if (x_is_zero) {
    res = ((y & MASK_SIGN32) != MASK_SIGN32);
    BID_RETURN (res);
  }
  // if y is zero, X is less if it is negative
  else if (y_is_zero) {
    res = ((x & MASK_SIGN32) == MASK_SIGN32);
    BID_RETURN (res);
  }
  // OPPOSITE SIGN (CASE5)
  // now, if the sign bits differ, x is less than if y is positive
  if (((x ^ y) & MASK_SIGN32) == MASK_SIGN32) {
    res = ((y & MASK_SIGN32) != MASK_SIGN32);
    BID_RETURN (res);
  }
  // REDUNDANT REPRESENTATIONS (CASE6)
  // if both components are either bigger or smaller
  if (sig_x > sig_y && exp_x >= exp_y) {
    res = ((x & MASK_SIGN32) == MASK_SIGN32);
    BID_RETURN (res);
  }
  if (sig_x < sig_y && exp_x <= exp_y) {
    res = ((x & MASK_SIGN32) != MASK_SIGN32);
    BID_RETURN (res);
  }
  // if exp_x is 6 greater than exp_y, no need for compensation
  if (exp_x - exp_y > 6) {
    res = ((x & MASK_SIGN32) == MASK_SIGN32);
    BID_RETURN (res);
  }
  // difference cannot be greater than 10^6

  // if exp_x is 6 less than exp_y, no need for compensation
  if (exp_y - exp_x > 6) {
    res = ((x & MASK_SIGN32) != MASK_SIGN32);
    BID_RETURN (res);
  }
  // if |exp_x - exp_y| < 6, it comes down to the compensated significand
  if (exp_x > exp_y) {	// to simplify the loop below,

    // otherwise adjust the x significand upwards
    sig_n_prime = (BID_UINT64)sig_x * (BID_UINT64)bid_mult_factor[exp_x - exp_y];

    // return 0 if values are equal
    if (sig_n_prime == sig_y) {
      res = 0;
      BID_RETURN (res);
    }
    // if postitive, return whichever significand abs is smaller 
    //     (converse if negative)
    {
      res = ((sig_n_prime < sig_y) ^ ((x & MASK_SIGN32) == MASK_SIGN32));
      BID_RETURN (res);
    }
  }
  // adjust the y significand upwards
  sig_n_prime = (BID_UINT64)sig_y * (BID_UINT64)bid_mult_factor[exp_y - exp_x];

  // return 0 if values are equal
  if (sig_n_prime == sig_x) {
    res = 0;
    BID_RETURN (res);
  }
  // if positive, return whichever significand abs is smaller 
  //     (converse if negative)
  {
    res = ((sig_x < sig_n_prime) ^ ((x & MASK_SIGN32) == MASK_SIGN32));
    BID_RETURN (res);
  }
}

BID_TYPE_FUNCTION_ARG2_CUSTOMRESULT_NORND(int, bid32_signaling_less_equal, BID_UINT32, x, y)

  int res;
  int exp_x, exp_y;
  BID_UINT32 sig_x, sig_y;
  BID_UINT64 sig_n_prime;
  char x_is_zero = 0, y_is_zero = 0, non_canon_x, non_canon_y;

  // NaN (CASE1)
  // if either number is NAN, the comparison is unordered, 
  // rather than equal : return 0
  if (((x & MASK_NAN32) == MASK_NAN32) || ((y & MASK_NAN32) == MASK_NAN32)) {
    *pfpsf |= BID_INVALID_EXCEPTION;	// set invalid exception if NaN
    res = 0;
    BID_RETURN (res);
  }
  // SIMPLE (CASE2)
  // if all the bits are the same, these numbers are equal (LESSEQUAL).
  if (x == y) {
    res = 1;
    BID_RETURN (res);
  }
  // INFINITY (CASE3)
  if ((x & MASK_INF32) == MASK_INF32) {
    // if x is neg infinity, it must be lessthan or equal to y return 1
    if (((x & MASK_SIGN32) == MASK_SIGN32)) {
      res = 1;
      BID_RETURN (res);
    }
    // x is pos infinity, it is greater, 
    // unless y is positive infinity => return y==pos_infinity
    else {
      res = !(((y & MASK_INF32) != MASK_INF32)
	      || ((y & MASK_SIGN32) == MASK_SIGN32));
      BID_RETURN (res);
    }
  } else if ((y & MASK_INF32) == MASK_INF32) {
    // x is finite, so if y is positive infinity, then x is less, return 1
    //                 if y is negative infinity, then x is greater, return 0
    {
      res = ((y & MASK_SIGN32) != MASK_SIGN32);
      BID_RETURN (res);
    }
  }
  // if steering bits are 11 (condition will be 0), then exponent is G[0:w+1] =>
  if ((x & MASK_STEERING_BITS32) == MASK_STEERING_BITS32) {
    exp_x = (x & MASK_BINARY_EXPONENT2_32) >> 21;
    sig_x = (x & MASK_BINARY_SIG2_32) | MASK_BINARY_OR2_32;
    if (sig_x > 9999999) {
      non_canon_x = 1;
    } else {
      non_canon_x = 0;
    }
  } else {
    exp_x = (x & MASK_BINARY_EXPONENT1_32) >> 23;
    sig_x = (x & MASK_BINARY_SIG1_32);
    non_canon_x = 0;
  }

  // if steering bits are 11 (condition will be 0), then exponent is G[0:w+1] =>
  if ((y & MASK_STEERING_BITS32) == MASK_STEERING_BITS32) {
    exp_y = (y & MASK_BINARY_EXPONENT2_32) >> 21;
    sig_y = (y & MASK_BINARY_SIG2_32) | MASK_BINARY_OR2_32;
    if (sig_y > 9999999) {
      non_canon_y = 1;
    } else {
      non_canon_y = 0;
    }
  } else {
    exp_y = (y & MASK_BINARY_EXPONENT1_32) >> 23;
    sig_y = (y & MASK_BINARY_SIG1_32);
    non_canon_y = 0;
  }

  // ZERO (CASE4)
  // some properties:
  // (+ZERO==-ZERO) => therefore ignore the sign, and neither number is greater
  //    (ZERO x 10^A == ZERO x 10^B) for any valid A, B => 
  //      therefore ignore the exponent field
  //    (Any non-canonical # is considered 0)
  if (non_canon_x || sig_x == 0) {
    x_is_zero = 1;
  }
  if (non_canon_y || sig_y == 0) {
    y_is_zero = 1;
  }
  // if both numbers are zero, they are equal -> return 1
  if (x_is_zero && y_is_zero) {
    res = 1;
    BID_RETURN (res);
  }
  // if x is zero, it is lessthan if Y is positive
  else if (x_is_zero) {
    res = ((y & MASK_SIGN32) != MASK_SIGN32);
    BID_RETURN (res);
  }
  // if y is zero, X is less if it is negative
  else if (y_is_zero) {
    res = ((x & MASK_SIGN32) == MASK_SIGN32);
    BID_RETURN (res);
  }
  // OPPOSITE SIGN (CASE5)
  // now, if the sign bits differ, x is less than if y is positive
  if (((x ^ y) & MASK_SIGN32) == MASK_SIGN32) {
    res = ((y & MASK_SIGN32) != MASK_SIGN32);
    BID_RETURN (res);
  }
  // REDUNDANT REPRESENTATIONS (CASE6)
  // if both components are either bigger or smaller
  if (sig_x > sig_y && exp_x >= exp_y) {
    res = ((x & MASK_SIGN32) == MASK_SIGN32);
    BID_RETURN (res);
  }
  if (sig_x < sig_y && exp_x <= exp_y) {
    res = ((x & MASK_SIGN32) != MASK_SIGN32);
    BID_RETURN (res);
  }
  // if exp_x is 6 greater than exp_y, no need for compensation
  if (exp_x - exp_y > 6) {
    res = ((x & MASK_SIGN32) == MASK_SIGN32);
    BID_RETURN (res);
  }
  // difference cannot be greater than 10^6

  // if exp_x is 6 less than exp_y, no need for compensation
  if (exp_y - exp_x > 6) {
    res = ((x & MASK_SIGN32) != MASK_SIGN32);
    BID_RETURN (res);
  }
  // if |exp_x - exp_y| < 6, it comes down to the compensated significand
  if (exp_x > exp_y) {	// to simplify the loop below,

    // otherwise adjust the x significand upwards
    sig_n_prime = (BID_UINT64)sig_x * (BID_UINT64)bid_mult_factor[exp_x - exp_y];

    // return 1 if values are equal
    if (sig_n_prime == sig_y) {
      res = 1;
      BID_RETURN (res);
    }
    // if postitive, return whichever significand abs is smaller 
    //     (converse if negative)
    {
      res = ((sig_n_prime < sig_y) ^ ((x & MASK_SIGN32) == MASK_SIGN32));
      BID_RETURN (res);
    }
  }
  // adjust the y significand upwards
  sig_n_prime = (BID_UINT64)sig_y * (BID_UINT64)bid_mult_factor[exp_y - exp_x];

  // return 1 if values are equal
  if (sig_n_prime == sig_x) {
    res = 1;
    BID_RETURN (res);
  }
  // if positive, return whichever significand abs is smaller 
  //     (converse if negative)
  {
    res = ((sig_x < sig_n_prime) ^ ((x & MASK_SIGN32) == MASK_SIGN32));
    BID_RETURN (res);
  }
}

BID_TYPE_FUNCTION_ARG2_CUSTOMRESULT_NORND(int, bid32_signaling_less_unordered, BID_UINT32, x, y)

  int res;
  int exp_x, exp_y;
  BID_UINT32 sig_x, sig_y;
  BID_UINT64 sig_n_prime;
  char x_is_zero = 0, y_is_zero = 0, non_canon_x, non_canon_y;

  // NaN (CASE1)
  // if either number is NAN, the comparison is unordered : return 0
  if (((x & MASK_NAN32) == MASK_NAN32) || ((y & MASK_NAN32) == MASK_NAN32)) {
    *pfpsf |= BID_INVALID_EXCEPTION;	// set invalid exception if NaN
    res = 1;
    BID_RETURN (res);
  }
  // SIMPLE (CASE2)
  // if all the bits are the same, these numbers are equal.
  if (x == y) {
    res = 0;
    BID_RETURN (res);
  }
  // INFINITY (CASE3)
  if ((x & MASK_INF32) == MASK_INF32) {
    // if x==neg_inf, { res = (y == neg_inf)?0:1; BID_RETURN (res) }
    if ((x & MASK_SIGN32) == MASK_SIGN32)
      // x is -inf, so it is less than y unless y is -inf
    {
      res = (((y & MASK_INF32) != MASK_INF32)
	     || (y & MASK_SIGN32) != MASK_SIGN32);
      BID_RETURN (res);
    } else
      // x is pos_inf, no way for it to be less than y
    {
      res = 0;
      BID_RETURN (res);
    }
  } else if ((y & MASK_INF32) == MASK_INF32) {
    // x is finite, so:
    //    if y is +inf, x<y
    //    if y is -inf, x>y
    {
      res = ((y & MASK_SIGN32) != MASK_SIGN32);
      BID_RETURN (res);
    }
  }
  // if steering bits are 11 (condition will be 0), then exponent is G[0:w+1] =>
  if ((x & MASK_STEERING_BITS32) == MASK_STEERING_BITS32) {
    exp_x = (x & MASK_BINARY_EXPONENT2_32) >> 21;
    sig_x = (x & MASK_BINARY_SIG2_32) | MASK_BINARY_OR2_32;
    if (sig_x > 9999999) {
      non_canon_x = 1;
    } else {
      non_canon_x = 0;
    }
  } else {
    exp_x = (x & MASK_BINARY_EXPONENT1_32) >> 23;
    sig_x = (x & MASK_BINARY_SIG1_32);
    non_canon_x = 0;
  }

  // if steering bits are 11 (condition will be 0), then exponent is G[0:w+1] =>
  if ((y & MASK_STEERING_BITS32) == MASK_STEERING_BITS32) {
    exp_y = (y & MASK_BINARY_EXPONENT2_32) >> 21;
    sig_y = (y & MASK_BINARY_SIG2_32) | MASK_BINARY_OR2_32;
    if (sig_y > 9999999) {
      non_canon_y = 1;
    } else {
      non_canon_y = 0;
    }
  } else {
    exp_y = (y & MASK_BINARY_EXPONENT1_32) >> 23;
    sig_y = (y & MASK_BINARY_SIG1_32);
    non_canon_y = 0;
  }

  // ZERO (CASE4)
  // some properties:
  // (+ZERO==-ZERO) => therefore ignore the sign, and neither number is greater
  //    (ZERO x 10^A == ZERO x 10^B) for any valid A, B => 
  //      therefore ignore the exponent field
  //    (Any non-canonical # is considered 0)
  if (non_canon_x || sig_x == 0) {
    x_is_zero = 1;
  }
  if (non_canon_y || sig_y == 0) {
    y_is_zero = 1;
  }
  // if both numbers are zero, they are equal
  if (x_is_zero && y_is_zero) {
    res = 0;
    BID_RETURN (res);
  }
  // if x is zero, it is lessthan if Y is positive
  else if (x_is_zero) {
    res = ((y & MASK_SIGN32) != MASK_SIGN32);
    BID_RETURN (res);
  }
  // if y is zero, X is less if it is negative
  else if (y_is_zero) {
    res = ((x & MASK_SIGN32) == MASK_SIGN32);
    BID_RETURN (res);
  }
  // OPPOSITE SIGN (CASE5)
  // now, if the sign bits differ, x is less than if y is positive
  if (((x ^ y) & MASK_SIGN32) == MASK_SIGN32) {
    res = ((y & MASK_SIGN32) != MASK_SIGN32);
    BID_RETURN (res);
  }
  // REDUNDANT REPRESENTATIONS (CASE6)
  // if both components are either bigger or smaller
  if (sig_x > sig_y && exp_x >= exp_y) {
    res = ((x & MASK_SIGN32) == MASK_SIGN32);
    BID_RETURN (res);
  }
  if (sig_x < sig_y && exp_x <= exp_y) {
    res = ((x & MASK_SIGN32) != MASK_SIGN32);
    BID_RETURN (res);
  }
  // if exp_x is 6 greater than exp_y, no need for compensation
  if (exp_x - exp_y > 6) {
    res = ((x & MASK_SIGN32) == MASK_SIGN32);
    BID_RETURN (res);
  }
  // difference cannot be greater than 10^6

  // if exp_x is 6 less than exp_y, no need for compensation
  if (exp_y - exp_x > 6) {
    res = ((x & MASK_SIGN32) != MASK_SIGN32);
    BID_RETURN (res);
  }
  // if |exp_x - exp_y| < 6, it comes down to the compensated significand
  if (exp_x > exp_y) {	// to simplify the loop below,

    // otherwise adjust the x significand upwards
    sig_n_prime = (BID_UINT64)sig_x * (BID_UINT64)bid_mult_factor[exp_x - exp_y];

    // return 0 if values are equal
    if (sig_n_prime == sig_y) {
      res = 0;
      BID_RETURN (res);
    }
    // if postitive, return whichever significand abs is smaller 
    //     (converse if negative)
    {
      res = ((sig_n_prime < sig_y) ^ ((x & MASK_SIGN32) == MASK_SIGN32));
      BID_RETURN (res);
    }
  }
  // adjust the y significand upwards
  sig_n_prime = (BID_UINT64)sig_y * (BID_UINT64)bid_mult_factor[exp_y - exp_x];

  // return 0 if values are equal
  if (sig_n_prime == sig_x) {
    res = 0;
    BID_RETURN (res);
  }
  // if positive, return whichever significand abs is smaller 
  //     (converse if negative)
  {
    res = ((sig_x < sig_n_prime) ^ ((x & MASK_SIGN32) == MASK_SIGN32));
    BID_RETURN (res);
  }
}

BID_TYPE_FUNCTION_ARG2_CUSTOMRESULT_NORND(int, bid32_signaling_not_greater, BID_UINT32, x, y)

  int res;
  int exp_x, exp_y;
  BID_UINT32 sig_x, sig_y;
  BID_UINT64 sig_n_prime;
  char x_is_zero = 0, y_is_zero = 0, non_canon_x, non_canon_y;

  // NaN (CASE1)
  // if either number is NAN, the comparison is unordered, 
  // rather than equal : return 0
  if (((x & MASK_NAN32) == MASK_NAN32) || ((y & MASK_NAN32) == MASK_NAN32)) {
    *pfpsf |= BID_INVALID_EXCEPTION;	// set invalid exception if NaN
    res = 1;
    BID_RETURN (res);
  }
  // SIMPLE (CASE2)
  // if all the bits are the same, these numbers are equal (LESSEQUAL).
  if (x == y) {
    res = 1;
    BID_RETURN (res);
  }
  // INFINITY (CASE3)
  if ((x & MASK_INF32) == MASK_INF32) {
    // if x is neg infinity, it must be lessthan or equal to y return 1
    if (((x & MASK_SIGN32) == MASK_SIGN32)) {
      res = 1;
      BID_RETURN (res);
    }
    // x is pos infinity, it is greater, 
    // unless y is positive infinity => return y==pos_infinity
    else {
      res = !(((y & MASK_INF32) != MASK_INF32)
	      || ((y & MASK_SIGN32) == MASK_SIGN32));
      BID_RETURN (res);
    }
  } else if ((y & MASK_INF32) == MASK_INF32) {
    // x is finite, so if y is positive infinity, then x is less, return 1
    //                 if y is negative infinity, then x is greater, return 0
    {
      res = ((y & MASK_SIGN32) != MASK_SIGN32);
      BID_RETURN (res);
    }
  }
  // if steering bits are 11 (condition will be 0), then exponent is G[0:w+1] =>
  if ((x & MASK_STEERING_BITS32) == MASK_STEERING_BITS32) {
    exp_x = (x & MASK_BINARY_EXPONENT2_32) >> 21;
    sig_x = (x & MASK_BINARY_SIG2_32) | MASK_BINARY_OR2_32;
    if (sig_x > 9999999) {
      non_canon_x = 1;
    } else {
      non_canon_x = 0;
    }
  } else {
    exp_x = (x & MASK_BINARY_EXPONENT1_32) >> 23;
    sig_x = (x & MASK_BINARY_SIG1_32);
    non_canon_x = 0;
  }

  // if steering bits are 11 (condition will be 0), then exponent is G[0:w+1] =>
  if ((y & MASK_STEERING_BITS32) == MASK_STEERING_BITS32) {
    exp_y = (y & MASK_BINARY_EXPONENT2_32) >> 21;
    sig_y = (y & MASK_BINARY_SIG2_32) | MASK_BINARY_OR2_32;
    if (sig_y > 9999999) {
      non_canon_y = 1;
    } else {
      non_canon_y = 0;
    }
  } else {
    exp_y = (y & MASK_BINARY_EXPONENT1_32) >> 23;
    sig_y = (y & MASK_BINARY_SIG1_32);
    non_canon_y = 0;
  }

  // ZERO (CASE4)
  // some properties:
  // (+ZERO==-ZERO) => therefore ignore the sign, and neither number is greater
  //    (ZERO x 10^A == ZERO x 10^B) for any valid A, B => 
  //      therefore ignore the exponent field
  //    (Any non-canonical # is considered 0)
  if (non_canon_x || sig_x == 0) {
    x_is_zero = 1;
  }
  if (non_canon_y || sig_y == 0) {
    y_is_zero = 1;
  }
  // if both numbers are zero, they are equal -> return 1
  if (x_is_zero && y_is_zero) {
    res = 1;
    BID_RETURN (res);
  }
  // if x is zero, it is lessthan if Y is positive
  else if (x_is_zero) {
    res = ((y & MASK_SIGN32) != MASK_SIGN32);
    BID_RETURN (res);
  }
  // if y is zero, X is less if it is negative
  else if (y_is_zero) {
    res = ((x & MASK_SIGN32) == MASK_SIGN32);
    BID_RETURN (res);
  }
  // OPPOSITE SIGN (CASE5)
  // now, if the sign bits differ, x is less than if y is positive
  if (((x ^ y) & MASK_SIGN32) == MASK_SIGN32) {
    res = ((y & MASK_SIGN32) != MASK_SIGN32);
    BID_RETURN (res);
  }
  // REDUNDANT REPRESENTATIONS (CASE6)
  // if both components are either bigger or smaller
  if (sig_x > sig_y && exp_x >= exp_y) {
    res = ((x & MASK_SIGN32) == MASK_SIGN32);
    BID_RETURN (res);
  }
  if (sig_x < sig_y && exp_x <= exp_y) {
    res = ((x & MASK_SIGN32) != MASK_SIGN32);
    BID_RETURN (res);
  }
  // if exp_x is 6 greater than exp_y, no need for compensation
  if (exp_x - exp_y > 6) {
    res = ((x & MASK_SIGN32) == MASK_SIGN32);
    BID_RETURN (res);
  }
  // difference cannot be greater than 10^6

  // if exp_x is 6 less than exp_y, no need for compensation
  if (exp_y - exp_x > 6) {
    res = ((x & MASK_SIGN32) != MASK_SIGN32);
    BID_RETURN (res);
  }
  // if |exp_x - exp_y| < 6, it comes down to the compensated significand
  if (exp_x > exp_y) {	// to simplify the loop below,

    // otherwise adjust the x significand upwards
    sig_n_prime = (BID_UINT64)sig_x * (BID_UINT64)bid_mult_factor[exp_x - exp_y];

    // return 1 if values are equal
    if (sig_n_prime == sig_y) {
      res = 1;
      BID_RETURN (res);
    }
    // if postitive, return whichever significand abs is smaller 
    //     (converse if negative)
    {
      res = ((sig_n_prime < sig_y) ^ ((x & MASK_SIGN32) == MASK_SIGN32));
      BID_RETURN (res);
    }
  }
  // adjust the y significand upwards
  sig_n_prime = (BID_UINT64)sig_y * (BID_UINT64)bid_mult_factor[exp_y - exp_x];

  // return 1 if values are equal
  if (sig_n_prime == sig_x) {
    res = 1;
    BID_RETURN (res);
  }
  // if positive, return whichever significand abs is smaller 
  //     (converse if negative)
  {
    res = ((sig_x < sig_n_prime) ^ ((x & MASK_SIGN32) == MASK_SIGN32));
    BID_RETURN (res);
  }
}

BID_TYPE_FUNCTION_ARG2_CUSTOMRESULT_NORND(int, bid32_signaling_not_less, BID_UINT32, x, y)

  int res;
  int exp_x, exp_y;
  BID_UINT32 sig_x, sig_y;
  BID_UINT64 sig_n_prime;
  char x_is_zero = 0, y_is_zero = 0, non_canon_x, non_canon_y;

  // NaN (CASE1)
  // if either number is NAN, the comparison is unordered : return 1
  if (((x & MASK_NAN32) == MASK_NAN32) || ((y & MASK_NAN32) == MASK_NAN32)) {
    *pfpsf |= BID_INVALID_EXCEPTION;	// set invalid exception if NaN
    res = 1;
    BID_RETURN (res);
  }
  // SIMPLE (CASE2)
  // if all the bits are the same, these numbers are equal.
  if (x == y) {
    res = 1;
    BID_RETURN (res);
  }
  // INFINITY (CASE3)
  if ((x & MASK_INF32) == MASK_INF32) {
    // if x==neg_inf, { res = (y == neg_inf)?1:0; BID_RETURN (res) }
    if ((x & MASK_SIGN32) == MASK_SIGN32)
      // x is -inf, so it is less than y unless y is -inf
    {
      res = (((y & MASK_INF32) == MASK_INF32)
	     && (y & MASK_SIGN32) == MASK_SIGN32);
      BID_RETURN (res);
    } else
      // x is pos_inf, no way for it to be less than y
    {
      res = 1;
      BID_RETURN (res);
    }
  } else if ((y & MASK_INF32) == MASK_INF32) {
    // x is finite, so:
    //    if y is +inf, x<y
    //    if y is -inf, x>y
    {
      res = ((y & MASK_SIGN32) == MASK_SIGN32);
      BID_RETURN (res);
    }
  }
  // if steering bits are 11 (condition will be 0), then exponent is G[0:w+1] =>
  if ((x & MASK_STEERING_BITS32) == MASK_STEERING_BITS32) {
    exp_x = (x & MASK_BINARY_EXPONENT2_32) >> 21;
    sig_x = (x & MASK_BINARY_SIG2_32) | MASK_BINARY_OR2_32;
    if (sig_x > 9999999) {
      non_canon_x = 1;
    } else {
      non_canon_x = 0;
    }
  } else {
    exp_x = (x & MASK_BINARY_EXPONENT1_32) >> 23;
    sig_x = (x & MASK_BINARY_SIG1_32);
    non_canon_x = 0;
  }

  // if steering bits are 11 (condition will be 0), then exponent is G[0:w+1] =>
  if ((y & MASK_STEERING_BITS32) == MASK_STEERING_BITS32) {
    exp_y = (y & MASK_BINARY_EXPONENT2_32) >> 21;
    sig_y = (y & MASK_BINARY_SIG2_32) | MASK_BINARY_OR2_32;
    if (sig_y > 9999999) {
      non_canon_y = 1;
    } else {
      non_canon_y = 0;
    }
  } else {
    exp_y = (y & MASK_BINARY_EXPONENT1_32) >> 23;
    sig_y = (y & MASK_BINARY_SIG1_32);
    non_canon_y = 0;
  }

  // ZERO (CASE4)
  // some properties:
  // (+ZERO==-ZERO) => therefore ignore the sign, and neither number is greater
  //    (ZERO x 10^A == ZERO x 10^B) for any valid A, B => 
  //      therefore ignore the exponent field
  //    (Any non-canonical # is considered 0)
  if (non_canon_x || sig_x == 0) {
    x_is_zero = 1;
  }
  if (non_canon_y || sig_y == 0) {
    y_is_zero = 1;
  }
  // if both numbers are zero, they are equal
  if (x_is_zero && y_is_zero) {
    res = 1;
    BID_RETURN (res);
  }
  // if x is zero, it is lessthan if Y is positive
  else if (x_is_zero) {
    res = ((y & MASK_SIGN32) == MASK_SIGN32);
    BID_RETURN (res);
  }
  // if y is zero, X is less if it is negative
  else if (y_is_zero) {
    res = ((x & MASK_SIGN32) != MASK_SIGN32);
    BID_RETURN (res);
  }
  // OPPOSITE SIGN (CASE5)
  // now, if the sign bits differ, x is less than if y is positive
  if (((x ^ y) & MASK_SIGN32) == MASK_SIGN32) {
    res = ((y & MASK_SIGN32) == MASK_SIGN32);
    BID_RETURN (res);
  }
  // REDUNDANT REPRESENTATIONS (CASE6)
  // if both components are either bigger or smaller
  if (sig_x > sig_y && exp_x >= exp_y) {
    res = ((x & MASK_SIGN32) != MASK_SIGN32);
    BID_RETURN (res);
  }
  if (sig_x < sig_y && exp_x <= exp_y) {
    res = ((x & MASK_SIGN32) == MASK_SIGN32);
    BID_RETURN (res);
  }
  // if exp_x is 6 greater than exp_y, no need for compensation
  if (exp_x - exp_y > 6) {
    res = ((x & MASK_SIGN32) != MASK_SIGN32);
    BID_RETURN (res);
  }
  // difference cannot be greater than 10^6

  // if exp_x is 6 less than exp_y, no need for compensation
  if (exp_y - exp_x > 6) {
    res = ((x & MASK_SIGN32) == MASK_SIGN32);
    BID_RETURN (res);
  }
  // if |exp_x - exp_y| < 6, it comes down to the compensated significand
  if (exp_x > exp_y) {	// to simplify the loop below,

    // otherwise adjust the x significand upwards
    sig_n_prime = (BID_UINT64)sig_x * (BID_UINT64)bid_mult_factor[exp_x - exp_y];

    // return 0 if values are equal
    if (sig_n_prime == sig_y) {
      res = 1;
      BID_RETURN (res);
    }
    // if postitive, return whichever significand abs is smaller 
    //     (converse if negative)
    {
      res = ((sig_n_prime < sig_y) ^ ((x & MASK_SIGN32) != MASK_SIGN32));
      BID_RETURN (res);
    }
  }
  // adjust the y significand upwards
  sig_n_prime = (BID_UINT64)sig_y * (BID_UINT64)bid_mult_factor[exp_y - exp_x];

  // return 0 if values are equal
  if (sig_n_prime == sig_x) {
    res = 1;
    BID_RETURN (res);
  }
  // if positive, return whichever significand abs is smaller 
  //     (converse if negative)
  {
    res = ((sig_x < sig_n_prime) ^ ((x & MASK_SIGN32) != MASK_SIGN32));
    BID_RETURN (res);
  }
}
