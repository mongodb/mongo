/******************************************************************************
  Copyright (c) 2011, Intel Corp.
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
 *  BID32 minimum function - returns greater of two numbers
 *****************************************************************************/

static const BID_UINT32 bid_mult_factor[7] = {
  1, 10, 100, 1000, 10000, 100000, 1000000
};

BID_TYPE0_FUNCTION_ARGTYPE1_ARGTYPE2_NORND(BID_UINT32, bid32_minnum, BID_UINT32, x, BID_UINT32, y)

  BID_UINT32 res;
  int exp_x, exp_y;
  BID_UINT32 sig_x, sig_y;
  BID_UINT64 sig_n_prime;
  char x_is_zero = 0, y_is_zero = 0;

  // check for non-canonical x
  if ((x & MASK_NAN32) == MASK_NAN32) {	// x is NaN
    x = x & 0xfe0fffff;	// clear G6-G10
    if ((x & 0x000fffff) > 999999) {
      x = x & 0xfe000000;	// clear G6-G10 and the payload bits
    }
  } else if ((x & MASK_INF32) == MASK_INF32) {	// check for Infinity
    x = x & (MASK_SIGN32 | MASK_INF32);
  } else {	// x is not special
    // check for non-canonical values - treated as zero
    if ((x & MASK_STEERING_BITS32) == MASK_STEERING_BITS32) {
      // if the steering bits are 11, then the exponent is G[0:w+1]
      if (((x & MASK_BINARY_SIG2_32) | MASK_BINARY_OR2_32) > 9999999) {
	// non-canonical
	x = (x & MASK_SIGN32) | ((x & MASK_BINARY_EXPONENT2_32) << 2);
      }	// else canonical
    }	// else canonical 
  }

  // check for non-canonical y
  if ((y & MASK_NAN32) == MASK_NAN32) {	// y is NaN
    y = y & 0xfe0fffff;	// clear G6-G10
    if ((y & 0x000fffff) > 999999) {
      y = y & 0xfe000000;	// clear G6-G10 and the payload bits
    }
  } else if ((y & MASK_INF32) == MASK_INF32) {	// check for Infinity
    y = y & (MASK_SIGN32 | MASK_INF32);
  } else {	// y is not special
    // check for non-canonical values - treated as zero
    if ((y & MASK_STEERING_BITS32) == MASK_STEERING_BITS32) {
      // if the steering bits are 11, then the exponent is G[0:w+1]
      if (((y & MASK_BINARY_SIG2_32) | MASK_BINARY_OR2_32) > 9999999) {
	// non-canonical
	y = (y & MASK_SIGN32) | ((y & MASK_BINARY_EXPONENT2_32) << 2);
      }	// else canonical
    }	// else canonical
  }

  // NaN (CASE1)
  if ((x & MASK_NAN32) == MASK_NAN32) {	// x is NAN
    if ((x & MASK_SNAN32) == MASK_SNAN32) {	// x is SNaN
      // if x is SNAN, then return quiet (x)
      *pfpsf |= BID_INVALID_EXCEPTION;	// set exception if SNaN
      x = x & 0xfdffffff;	// quietize x
      res = x;
    } else {	// x is QNaN
      if ((y & MASK_NAN32) == MASK_NAN32) {	// y is NAN
	if ((y & MASK_SNAN32) == MASK_SNAN32) {	// y is SNAN
	  *pfpsf |= BID_INVALID_EXCEPTION;	// set invalid flag
	}
	res = x;
      } else {
	res = y;
      }
    }
    BID_RETURN (res);
  } else if ((y & MASK_NAN32) == MASK_NAN32) {	// y is NaN, but x is not
    if ((y & MASK_SNAN32) == MASK_SNAN32) {
      *pfpsf |= BID_INVALID_EXCEPTION;	// set exception if SNaN
      y = y & 0xfdffffff;	// quietize y
      res = y;
    } else {
      // will return x (which is not NaN)
      res = x;
    }
    BID_RETURN (res);
  }
  // SIMPLE (CASE2)
  // if all the bits are the same, these numbers are equal, return either number
  if (x == y) {
    res = x;
    BID_RETURN (res);
  }
  // INFINITY (CASE3)
  if ((x & MASK_INF32) == MASK_INF32) {
    // if x is neg infinity, there is no way it is greater than y, return x
    if (((x & MASK_SIGN32) == MASK_SIGN32)) {
      res = x;
      BID_RETURN (res);
    }
    // x is pos infinity, return y
    else {
      res = y;
      BID_RETURN (res);
    }
  } else if ((y & MASK_INF32) == MASK_INF32) {
    // x is finite, so if y is positive infinity, then x is less, return y
    //                 if y is negative infinity, then x is greater, return x
    res = ((y & MASK_SIGN32) == MASK_SIGN32) ? y : x;
    BID_RETURN (res);
  }
  // if steering bits are 11 (condition will be 0), then exponent is G[0:w+1] =>
  if ((x & MASK_STEERING_BITS32) == MASK_STEERING_BITS32) {
    exp_x = (x & MASK_BINARY_EXPONENT2_32) >> 21;
    sig_x = (x & MASK_BINARY_SIG2_32) | MASK_BINARY_OR2_32;
  } else {
    exp_x = (x & MASK_BINARY_EXPONENT1_32) >> 23;
    sig_x = (x & MASK_BINARY_SIG1_32);
  }

  // if steering bits are 11 (condition will be 0), then exponent is G[0:w+1] =>
  if ((y & MASK_STEERING_BITS32) == MASK_STEERING_BITS32) {
    exp_y = (y & MASK_BINARY_EXPONENT2_32) >> 21;
    sig_y = (y & MASK_BINARY_SIG2_32) | MASK_BINARY_OR2_32;
  } else {
    exp_y = (y & MASK_BINARY_EXPONENT1_32) >> 23;
    sig_y = (y & MASK_BINARY_SIG1_32);
  }

  // ZERO (CASE4)
  // some properties:
  //    (+ZERO == -ZERO) => therefore 
  //        ignore the sign, and neither number is greater
  //    (ZERO x 10^A == ZERO x 10^B) for any valid A, B => 
  //        ignore the exponent field
  //    (Any non-canonical # is considered 0)
  if (sig_x == 0) {
    x_is_zero = 1;
  }
  if (sig_y == 0) {
    y_is_zero = 1;
  }

  if (x_is_zero && y_is_zero) {
    // if both numbers are zero, neither is greater => return either
    res = y;
    BID_RETURN (res);
  } else if (x_is_zero) {
    // is x is zero, it is greater if Y is negative
    res = ((y & MASK_SIGN32) == MASK_SIGN32) ? y : x;
    BID_RETURN (res);
  } else if (y_is_zero) {
    // is y is zero, X is greater if it is positive
    res = ((x & MASK_SIGN32) != MASK_SIGN32) ? y : x;;
    BID_RETURN (res);
  }
  // OPPOSITE SIGN (CASE5)
  // now, if the sign bits differ, x is greater if y is negative
  if (((x ^ y) & MASK_SIGN32) == MASK_SIGN32) {
    res = ((y & MASK_SIGN32) == MASK_SIGN32) ? y : x;
    BID_RETURN (res);
  }
  // REDUNDANT REPRESENTATIONS (CASE6)

  // if both components are either bigger or smaller, 
  // it is clear what needs to be done
  if (sig_x > sig_y && exp_x >= exp_y) {
    res = ((x & MASK_SIGN32) != MASK_SIGN32) ? y : x;
    BID_RETURN (res);
  }
  if (sig_x < sig_y && exp_x <= exp_y) {
    res = ((x & MASK_SIGN32) == MASK_SIGN32) ? y : x;
    BID_RETURN (res);
  }
  // if exp_x is 6 greater than exp_y, no need for compensation
  if (exp_x - exp_y > 6) {
    res = ((x & MASK_SIGN32) != MASK_SIGN32) ? y : x;	// difference cannot be >10^6
    BID_RETURN (res);
  }
  // if exp_x is 6 less than exp_y, no need for compensation
  if (exp_y - exp_x > 6) {
    res = ((x & MASK_SIGN32) == MASK_SIGN32) ? y : x;
    BID_RETURN (res);
  }
  // if |exp_x - exp_y|< 6, it comes down to the compensated significand
  if (exp_x > exp_y) {	// to simplify the loop below,

    // otherwise adjust the x significand upwards
    sig_n_prime = (BID_UINT64)sig_x * (BID_UINT64)bid_mult_factor[exp_x - exp_y];
    // if postitive, return whichever significand is larger 
    // (converse if negative)
    if (sig_n_prime == sig_y) {
      res = y;
      BID_RETURN (res);
    }

    res = ((sig_n_prime > sig_y) ^ ((x & MASK_SIGN32) == MASK_SIGN32)) ? y : x;
    BID_RETURN (res);
  }
  // adjust the y significand upwards
  sig_n_prime = (BID_UINT64)sig_y * (BID_UINT64)bid_mult_factor[exp_y - exp_x];

  // if postitive, return whichever significand is larger (converse if negative)
  if (sig_n_prime == sig_x) {
    res = y;
    BID_RETURN (res);
  }
  res = ((sig_x > sig_n_prime) ^ ((x & MASK_SIGN32) == MASK_SIGN32)) ? y : x;
  BID_RETURN (res);
}

/*****************************************************************************
 *  BID32 minimum magnitude function - returns greater of two numbers
 *****************************************************************************/

BID_TYPE0_FUNCTION_ARGTYPE1_ARGTYPE2_NORND(BID_UINT32, bid32_minnum_mag, BID_UINT32, x, BID_UINT32, y)

  BID_UINT32 res;
  int exp_x, exp_y;
  BID_UINT32 sig_x, sig_y;
  BID_UINT64 sig_n_prime;

  // check for non-canonical x
  if ((x & MASK_NAN32) == MASK_NAN32) {	// x is NaN
    x = x & 0xfe0fffff;	// clear G6-G10
    if ((x & 0x000fffff) > 999999) {
      x = x & 0xfe000000;	// clear G6-G10 and the payload bits
    }
  } else if ((x & MASK_INF32) == MASK_INF32) {	// check for Infinity
    x = x & (MASK_SIGN32 | MASK_INF32);
  } else {	// x is not special
    // check for non-canonical values - treated as zero
    if ((x & MASK_STEERING_BITS32) == MASK_STEERING_BITS32) {
      // if the steering bits are 11, then the exponent is G[0:w+1]
      if (((x & MASK_BINARY_SIG2_32) | MASK_BINARY_OR2_32) > 9999999) {
	// non-canonical
	x = (x & MASK_SIGN32) | ((x & MASK_BINARY_EXPONENT2_32) << 2);
      }	// else canonical
    }	// else canonical 
  }

  // check for non-canonical y
  if ((y & MASK_NAN32) == MASK_NAN32) {	// y is NaN
    y = y & 0xfe0fffff;	// clear G6-G10
    if ((y & 0x000fffff) > 999999) {
      y = y & 0xfe000000;	// clear G6-G10 and the payload bits
    }
  } else if ((y & MASK_INF32) == MASK_INF32) {	// check for Infinity
    y = y & (MASK_SIGN32 | MASK_INF32);
  } else {	// y is not special
    // check for non-canonical values - treated as zero
    if ((y & MASK_STEERING_BITS32) == MASK_STEERING_BITS32) {
      // if the steering bits are 11, then the exponent is G[0:w+1]
      if (((y & MASK_BINARY_SIG2_32) | MASK_BINARY_OR2_32) > 9999999) {
	// non-canonical
	y = (y & MASK_SIGN32) | ((y & MASK_BINARY_EXPONENT2_32) << 2);
      }	// else canonical
    }	// else canonical
  }

  // NaN (CASE1)
  if ((x & MASK_NAN32) == MASK_NAN32) {	// x is NAN
    if ((x & MASK_SNAN32) == MASK_SNAN32) {	// x is SNaN
      // if x is SNAN, then return quiet (x)
      *pfpsf |= BID_INVALID_EXCEPTION;	// set exception if SNaN
      x = x & 0xfdffffff;	// quietize x
      res = x;
    } else {	// x is QNaN
      if ((y & MASK_NAN32) == MASK_NAN32) {	// y is NAN
	if ((y & MASK_SNAN32) == MASK_SNAN32) {	// y is SNAN
	  *pfpsf |= BID_INVALID_EXCEPTION;	// set invalid flag
	}
	res = x;
      } else {
	res = y;
      }
    }
    BID_RETURN (res);
  } else if ((y & MASK_NAN32) == MASK_NAN32) {	// y is NaN, but x is not
    if ((y & MASK_SNAN32) == MASK_SNAN32) {
      *pfpsf |= BID_INVALID_EXCEPTION;	// set exception if SNaN
      y = y & 0xfdffffff;	// quietize y
      res = y;
    } else {
      // will return x (which is not NaN)
      res = x;
    }
    BID_RETURN (res);
  }
  // SIMPLE (CASE2)
  // if all the bits are the same, these numbers are equal, return either number
  if (x == y) {
    res = x;
    BID_RETURN (res);
  }
  // INFINITY (CASE3)
  if ((x & MASK_INF32) == MASK_INF32) {
    // x is infinity, its magnitude is greater than or equal to y
    // return x only if y is infinity and x is negative
    res = ((x & MASK_SIGN32) == MASK_SIGN32
	   && (y & MASK_INF32) == MASK_INF32) ? x : y;
    BID_RETURN (res);
  } else if ((y & MASK_INF32) == MASK_INF32) {
    // y is infinity, then it must be greater in magnitude, return x
    res = x;
    BID_RETURN (res);
  }
  // if steering bits are 11 (condition will be 0), then exponent is G[0:w+1] =>
  if ((x & MASK_STEERING_BITS32) == MASK_STEERING_BITS32) {
    exp_x = (x & MASK_BINARY_EXPONENT2_32) >> 21;
    sig_x = (x & MASK_BINARY_SIG2_32) | MASK_BINARY_OR2_32;
  } else {
    exp_x = (x & MASK_BINARY_EXPONENT1_32) >> 23;
    sig_x = (x & MASK_BINARY_SIG1_32);
  }

  // if steering bits are 11 (condition will be 0), then exponent is G[0:w+1] =>
  if ((y & MASK_STEERING_BITS32) == MASK_STEERING_BITS32) {
    exp_y = (y & MASK_BINARY_EXPONENT2_32) >> 21;
    sig_y = (y & MASK_BINARY_SIG2_32) | MASK_BINARY_OR2_32;
  } else {
    exp_y = (y & MASK_BINARY_EXPONENT1_32) >> 23;
    sig_y = (y & MASK_BINARY_SIG1_32);
  }

  // ZERO (CASE4)
  // some properties:
  //    (+ZERO == -ZERO) => therefore 
  //        ignore the sign, and neither number is greater
  //    (ZERO x 10^A == ZERO x 10^B) for any valid A, B => 
  //        ignore the exponent field
  //    (Any non-canonical # is considered 0)
  if (sig_x == 0) {
    res = x;	// x_is_zero, its magnitude must be smaller than y
    BID_RETURN (res);
  }
  if (sig_y == 0) {
    res = y;	// y_is_zero, its magnitude must be smaller than x
    BID_RETURN (res);
  }
  // REDUNDANT REPRESENTATIONS (CASE6)
  // if both components are either bigger or smaller, 
  // it is clear what needs to be done
  if (sig_x > sig_y && exp_x >= exp_y) {
    res = y;
    BID_RETURN (res);
  }
  if (sig_x < sig_y && exp_x <= exp_y) {
    res = x;
    BID_RETURN (res);
  }
  // if exp_x is 6 greater than exp_y, no need for compensation
  if (exp_x - exp_y > 6) {
    res = y;	// difference cannot be greater than 10^6
    BID_RETURN (res);
  }
  // if exp_x is 6 less than exp_y, no need for compensation
  if (exp_y - exp_x > 6) {
    res = x;
    BID_RETURN (res);
  }
  // if |exp_x - exp_y|< 6, it comes down to the compensated significand
  if (exp_x > exp_y) {	// to simplify the loop below,
    // otherwise adjust the x significand upwards
    sig_n_prime = (BID_UINT64)sig_x * (BID_UINT64)bid_mult_factor[exp_x - exp_y];
    // now, sig_n_prime has: sig_x * 10^(exp_x-exp_y), this is 
    // the compensated signif.
    if (sig_n_prime == sig_y) {
      // two numbers are equal, return minNum(x,y)
      res = ((y & MASK_SIGN32) == MASK_SIGN32) ? y : x;
      BID_RETURN (res);
    }
    // now, if compensated_x (sig_n_prime) is greater than y, return y,  
    // otherwise return x
    res = (sig_n_prime > sig_y) ? y : x;
    BID_RETURN (res);
  }
  // exp_y must be greater than exp_x, thus adjust the y significand upwards
  sig_n_prime = (BID_UINT64)sig_y * (BID_UINT64)bid_mult_factor[exp_y - exp_x];

  if (sig_n_prime == sig_x) {
    res = ((y & MASK_SIGN32) == MASK_SIGN32) ? y : x;
    // two numbers are equal, return either
    BID_RETURN (res);
  }

  res = (sig_x > sig_n_prime) ? y : x;
  BID_RETURN (res);
}

/*****************************************************************************
 *  BID32 maximum function - returns greater of two numbers
 *****************************************************************************/

BID_TYPE0_FUNCTION_ARGTYPE1_ARGTYPE2_NORND(BID_UINT32, bid32_maxnum, BID_UINT32, x, BID_UINT32, y)


  BID_UINT32 res;
  int exp_x, exp_y;
  BID_UINT32 sig_x, sig_y;
  BID_UINT64 sig_n_prime;
  char x_is_zero = 0, y_is_zero = 0;

  // check for non-canonical x
  if ((x & MASK_NAN32) == MASK_NAN32) {	// x is NaN
    x = x & 0xfe0fffff;	// clear G6-G10
    if ((x & 0x000fffff) > 999999) {
      x = x & 0xfe000000;	// clear G6-G10 and the payload bits
    }
  } else if ((x & MASK_INF32) == MASK_INF32) {	// check for Infinity
    x = x & (MASK_SIGN32 | MASK_INF32);
  } else {	// x is not special
    // check for non-canonical values - treated as zero
    if ((x & MASK_STEERING_BITS32) == MASK_STEERING_BITS32) {
      // if the steering bits are 11, then the exponent is G[0:w+1]
      if (((x & MASK_BINARY_SIG2_32) | MASK_BINARY_OR2_32) > 9999999) {
	// non-canonical
	x = (x & MASK_SIGN32) | ((x & MASK_BINARY_EXPONENT2_32) << 2);
      }	// else canonical
    }	// else canonical 
  }

  // check for non-canonical y
  if ((y & MASK_NAN32) == MASK_NAN32) {	// y is NaN
    y = y & 0xfe0fffff;	// clear G6-G10
    if ((y & 0x000fffff) > 999999) {
      y = y & 0xfe000000;	// clear G6-G10 and the payload bits
    }
  } else if ((y & MASK_INF32) == MASK_INF32) {	// check for Infinity
    y = y & (MASK_SIGN32 | MASK_INF32);
  } else {	// y is not special
    // check for non-canonical values - treated as zero
    if ((y & MASK_STEERING_BITS32) == MASK_STEERING_BITS32) {
      // if the steering bits are 11, then the exponent is G[0:w+1]
      if (((y & MASK_BINARY_SIG2_32) | MASK_BINARY_OR2_32) > 9999999) {
	// non-canonical
	y = (y & MASK_SIGN32) | ((y & MASK_BINARY_EXPONENT2_32) << 2);
      }	// else canonical
    }	// else canonical
  }

  // NaN (CASE1)
  if ((x & MASK_NAN32) == MASK_NAN32) {	// x is NAN
    if ((x & MASK_SNAN32) == MASK_SNAN32) {	// x is SNaN
      // if x is SNAN, then return quiet (x)
      *pfpsf |= BID_INVALID_EXCEPTION;	// set exception if SNaN
      x = x & 0xfdffffff;	// quietize x
      res = x;
    } else {	// x is QNaN
      if ((y & MASK_NAN32) == MASK_NAN32) {	// y is NAN
	if ((y & MASK_SNAN32) == MASK_SNAN32) {	// y is SNAN
	  *pfpsf |= BID_INVALID_EXCEPTION;	// set invalid flag
	}
	res = x;
      } else {
	res = y;
      }
    }
    BID_RETURN (res);
  } else if ((y & MASK_NAN32) == MASK_NAN32) {	// y is NaN, but x is not
    if ((y & MASK_SNAN32) == MASK_SNAN32) {
      *pfpsf |= BID_INVALID_EXCEPTION;	// set exception if SNaN
      y = y & 0xfdffffff;	// quietize y
      res = y;
    } else {
      // will return x (which is not NaN)
      res = x;
    }
    BID_RETURN (res);
  }
  // SIMPLE (CASE2)
  // if all the bits are the same, these numbers are equal (not Greater).
  if (x == y) {
    res = x;
    BID_RETURN (res);
  }
  // INFINITY (CASE3)
  if ((x & MASK_INF32) == MASK_INF32) { // x = +/-infinity
    // if x is neg infinity, there is no way it is greater than y, return y
    // x is pos infinity, it is greater, unless y is positive infinity => 
    // return y!=pos_infinity
    if (((x & MASK_SIGN32) == MASK_SIGN32)) { // x = -infinity
      res = y;
    } else { // x = +infinity
      res = x;
    }
    BID_RETURN (res);
  } else if ((y & MASK_INF32) == MASK_INF32) {
    // x is finite, so if y is positive infinity, then x is less, return y
    //                 if y is negative infinity, then x is greater, return x
    res = ((y & MASK_SIGN32) == MASK_SIGN32) ? x : y;
    BID_RETURN (res);
  }
  // if steering bits are 11 (condition will be 0), then exponent is G[0:w+1] =>
  if ((x & MASK_STEERING_BITS32) == MASK_STEERING_BITS32) {
    exp_x = (x & MASK_BINARY_EXPONENT2_32) >> 21;
    sig_x = (x & MASK_BINARY_SIG2_32) | MASK_BINARY_OR2_32;
  } else {
    exp_x = (x & MASK_BINARY_EXPONENT1_32) >> 23;
    sig_x = (x & MASK_BINARY_SIG1_32);
  }

  // if steering bits are 11 (condition will be 0), then exponent is G[0:w+1] =>
  if ((y & MASK_STEERING_BITS32) == MASK_STEERING_BITS32) {
    exp_y = (y & MASK_BINARY_EXPONENT2_32) >> 21;
    sig_y = (y & MASK_BINARY_SIG2_32) | MASK_BINARY_OR2_32;
  } else {
    exp_y = (y & MASK_BINARY_EXPONENT1_32) >> 23;
    sig_y = (y & MASK_BINARY_SIG1_32);
  }

  // ZERO (CASE4)
  // some properties:
  //    (+ZERO == -ZERO) => therefore 
  //        ignore the sign, and neither number is greater
  //    (ZERO x 10^A == ZERO x 10^B) for any valid A, B => 
  //        ignore the exponent field
  //    (Any non-canonical # is considered 0)
  if (sig_x == 0) {
    x_is_zero = 1;
  }
  if (sig_y == 0) {
    y_is_zero = 1;
  }

  if (x_is_zero && y_is_zero) {
    // if both numbers are zero, neither is greater => return NOTGREATERTHAN
    res = y;
    BID_RETURN (res);
  } else if (x_is_zero) {
    // is x is zero, it is greater if Y is negative
    res = ((y & MASK_SIGN32) == MASK_SIGN32) ? x : y;
    BID_RETURN (res);
  } else if (y_is_zero) {
    // is y is zero, X is greater if it is positive
    res = ((x & MASK_SIGN32) != MASK_SIGN32) ? x : y;;
    BID_RETURN (res);
  }
  // OPPOSITE SIGN (CASE5)
  // now, if the sign bits differ, x is greater if y is negative
  if (((x ^ y) & MASK_SIGN32) == MASK_SIGN32) {
    res = ((y & MASK_SIGN32) == MASK_SIGN32) ? x : y;
    BID_RETURN (res);
  }
  // REDUNDANT REPRESENTATIONS (CASE6)

  // if both components are either bigger or smaller, 
  //     it is clear what needs to be done
  if (sig_x > sig_y && exp_x >= exp_y) {
    res = ((x & MASK_SIGN32) != MASK_SIGN32) ? x : y;
    BID_RETURN (res);
  }
  if (sig_x < sig_y && exp_x <= exp_y) {
    res = ((x & MASK_SIGN32) == MASK_SIGN32) ? x : y;
    BID_RETURN (res);
  }
  // if exp_x is 6 greater than exp_y, no need for compensation
  if (exp_x - exp_y > 6) {
    res = ((x & MASK_SIGN32) != MASK_SIGN32) ? x : y;
    // difference cannot be > 10^6
    BID_RETURN (res);
  }
  // if exp_x is 6 less than exp_y, no need for compensation
  if (exp_y - exp_x > 6) {
    res = ((x & MASK_SIGN32) == MASK_SIGN32) ? x : y;
    BID_RETURN (res);
  }
  // if |exp_x - exp_y|< 6, it comes down to the compensated significand
  if (exp_x > exp_y) {	// to simplify the loop below,
    // otherwise adjust the x significand upwards
    sig_n_prime = (BID_UINT64)sig_x * (BID_UINT64)bid_mult_factor[exp_x - exp_y];
    // if postitive, return whichever significand is larger 
    // (converse if negative)
    if (sig_n_prime == sig_y) {
      res = y;
      BID_RETURN (res);
    }
    res = ((sig_n_prime > sig_y) ^ ((x & MASK_SIGN32) == MASK_SIGN32)) ? x : y;
    BID_RETURN (res);
  }
  // adjust the y significand upwards
  sig_n_prime = (BID_UINT64)sig_y * (BID_UINT64)bid_mult_factor[exp_y - exp_x];

  // if postitive, return whichever significand is larger (converse if negative)
  if (sig_n_prime == sig_x) {
    res = y;
    BID_RETURN (res);
  }
  res = ((sig_x > sig_n_prime) ^ ((x & MASK_SIGN32) == MASK_SIGN32)) ? x : y;
  BID_RETURN (res);
}

/*****************************************************************************
 *  BID32 maximum magnitude function - returns greater of two numbers
 *****************************************************************************/

BID_TYPE0_FUNCTION_ARGTYPE1_ARGTYPE2_NORND(BID_UINT32, bid32_maxnum_mag, BID_UINT32, x, BID_UINT32, y)

  BID_UINT32 res;
  int exp_x, exp_y;
  BID_UINT32 sig_x, sig_y;
  BID_UINT64 sig_n_prime;

  // check for non-canonical x
  if ((x & MASK_NAN32) == MASK_NAN32) {	// x is NaN
    x = x & 0xfe0fffff;	// clear G6-G10
    if ((x & 0x000fffff) > 999999) {
      x = x & 0xfe000000;	// clear G6-G10 and the payload bits
    }
  } else if ((x & MASK_INF32) == MASK_INF32) {	// check for Infinity
    x = x & (MASK_SIGN32 | MASK_INF32);
  } else {	// x is not special
    // check for non-canonical values - treated as zero
    if ((x & MASK_STEERING_BITS32) == MASK_STEERING_BITS32) {
      // if the steering bits are 11, then the exponent is G[0:w+1]
      if (((x & MASK_BINARY_SIG2_32) | MASK_BINARY_OR2_32) > 9999999) {
	// non-canonical
	x = (x & MASK_SIGN32) | ((x & MASK_BINARY_EXPONENT2_32) << 2);
      }	// else canonical
    }	// else canonical 
  }

  // check for non-canonical y
  if ((y & MASK_NAN32) == MASK_NAN32) {	// y is NaN
    y = y & 0xfe0fffff;	// clear G6-G10
    if ((y & 0x000fffff) > 999999) {
      y = y & 0xfe000000;	// clear G6-G10 and the payload bits
    }
  } else if ((y & MASK_INF32) == MASK_INF32) {	// check for Infinity
    y = y & (MASK_SIGN32 | MASK_INF32);
  } else {	// y is not special
    // check for non-canonical values - treated as zero
    if ((y & MASK_STEERING_BITS32) == MASK_STEERING_BITS32) {
      // if the steering bits are 11, then the exponent is G[0:w+1]
      if (((y & MASK_BINARY_SIG2_32) | MASK_BINARY_OR2_32) > 9999999) {
	// non-canonical
	y = (y & MASK_SIGN32) | ((y & MASK_BINARY_EXPONENT2_32) << 2);
      }	// else canonical
    }	// else canonical
  }

  // NaN (CASE1)
  if ((x & MASK_NAN32) == MASK_NAN32) {	// x is NAN
    if ((x & MASK_SNAN32) == MASK_SNAN32) {	// x is SNaN
      // if x is SNAN, then return quiet (x)
      *pfpsf |= BID_INVALID_EXCEPTION;	// set exception if SNaN
      x = x & 0xfdffffff;	// quietize x
      res = x;
    } else {	// x is QNaN
      if ((y & MASK_NAN32) == MASK_NAN32) {	// y is NAN
	if ((y & MASK_SNAN32) == MASK_SNAN32) {	// y is SNAN
	  *pfpsf |= BID_INVALID_EXCEPTION;	// set invalid flag
	}
	res = x;
      } else {
	res = y;
      }
    }
    BID_RETURN (res);
  } else if ((y & MASK_NAN32) == MASK_NAN32) {	// y is NaN, but x is not
    if ((y & MASK_SNAN32) == MASK_SNAN32) {
      *pfpsf |= BID_INVALID_EXCEPTION;	// set exception if SNaN
      y = y & 0xfdffffff;	// quietize y
      res = y;
    } else {
      // will return x (which is not NaN)
      res = x;
    }
    BID_RETURN (res);
  }
  // SIMPLE (CASE2)
  // if all the bits are the same, these numbers are equal, return either number
  if (x == y) {
    res = x;
    BID_RETURN (res);
  }
  // INFINITY (CASE3)
  if ((x & MASK_INF32) == MASK_INF32) {
    // x is infinity, its magnitude is greater than or equal to y
    // return y as long as x isn't negative infinity
    res = ((x & MASK_SIGN32) == MASK_SIGN32
	   && (y & MASK_INF32) == MASK_INF32) ? y : x;
    BID_RETURN (res);
  } else if ((y & MASK_INF32) == MASK_INF32) {
    // y is infinity, then it must be greater in magnitude
    res = y;
    BID_RETURN (res);
  }
  // if steering bits are 11 (condition will be 0), then exponent is G[0:w+1] =>
  if ((x & MASK_STEERING_BITS32) == MASK_STEERING_BITS32) {
    exp_x = (x & MASK_BINARY_EXPONENT2_32) >> 21;
    sig_x = (x & MASK_BINARY_SIG2_32) | MASK_BINARY_OR2_32;
  } else {
    exp_x = (x & MASK_BINARY_EXPONENT1_32) >> 23;
    sig_x = (x & MASK_BINARY_SIG1_32);
  }

  // if steering bits are 11 (condition will be 0), then exponent is G[0:w+1] =>
  if ((y & MASK_STEERING_BITS32) == MASK_STEERING_BITS32) {
    exp_y = (y & MASK_BINARY_EXPONENT2_32) >> 21;
    sig_y = (y & MASK_BINARY_SIG2_32) | MASK_BINARY_OR2_32;
  } else {
    exp_y = (y & MASK_BINARY_EXPONENT1_32) >> 23;
    sig_y = (y & MASK_BINARY_SIG1_32);
  }

  // ZERO (CASE4)
  // some properties:
  //    (+ZERO == -ZERO) => therefore 
  //        ignore the sign, and neither number is greater
  //    (ZERO x 10^A == ZERO x 10^B) for any valid A, B => 
  //        ignore the exponent field
  //    (Any non-canonical # is considered 0)
  if (sig_x == 0) {
    res = y;	// x_is_zero, its magnitude must be smaller than y
    BID_RETURN (res);
  }
  if (sig_y == 0) {
    res = x;	// y_is_zero, its magnitude must be smaller than x
    BID_RETURN (res);
  }
  // REDUNDANT REPRESENTATIONS (CASE6)
  // if both components are either bigger or smaller, 
  // it is clear what needs to be done
  if (sig_x > sig_y && exp_x >= exp_y) {
    res = x;
    BID_RETURN (res);
  }
  if (sig_x < sig_y && exp_x <= exp_y) {
    res = y;
    BID_RETURN (res);
  }
  // if exp_x is 6 greater than exp_y, no need for compensation
  if (exp_x - exp_y > 6) {
    res = x;	// difference cannot be greater than 10^6
    BID_RETURN (res);
  }
  // if exp_x is 6 less than exp_y, no need for compensation
  if (exp_y - exp_x > 6) {
    res = y;
    BID_RETURN (res);
  }
  // if |exp_x - exp_y|< 6, it comes down to the compensated significand
  if (exp_x > exp_y) {	// to simplify the loop below,
    // otherwise adjust the x significand upwards
    sig_n_prime = (BID_UINT64)sig_x * (BID_UINT64)bid_mult_factor[exp_x - exp_y];
    // now, sig_n_prime has: sig_x * 10^(exp_x-exp_y), 
    // this is the compensated signif.
    if (sig_n_prime == sig_y) {
      // two numbers are equal, return maxNum(x,y)
      res = ((y & MASK_SIGN32) == MASK_SIGN32) ? x : y;
      BID_RETURN (res);
    }
    // now, if compensated_x (sig_n_prime) is greater than y return y,  
    // otherwise return x
    res = (sig_n_prime > sig_y) ? x : y;
    BID_RETURN (res);
  }
  // exp_y must be greater than exp_x, thus adjust the y significand upwards
  sig_n_prime = (BID_UINT64)sig_y * (BID_UINT64)bid_mult_factor[exp_y - exp_x];

  if (sig_n_prime == sig_x) {
    res = ((y & MASK_SIGN32) == MASK_SIGN32) ? x : y;
    // two numbers are equal, return either
    BID_RETURN (res);
  }

  res = (sig_x > sig_n_prime) ? x : y;
  BID_RETURN (res);
}
