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
 *  BID64_round_integral_exact
 ****************************************************************************/

BID_TYPE0_FUNCTION_ARGTYPE1(BID_UINT64, bid64_round_integral_exact, BID_UINT64, x)

  BID_UINT64 res = 0xbaddbaddbaddbaddull;
  BID_UINT64 x_sign;
  int exp;			// unbiased exponent
  // Note: C1 represents the significand (BID_UINT64)
  BID_UI64DOUBLE tmp1;
  int x_nr_bits;
  int q, ind, shift;
  BID_UINT64 C1;
  // BID_UINT64 res is C* at first - represents up to 16 decimal digits <= 54 bits
  BID_UINT128 fstar = { {0x0ull, 0x0ull} };
  BID_UINT128 P128;

  x_sign = x & MASK_SIGN;	// 0 for positive, MASK_SIGN for negative

  // check for NaNs and infinities
  if ((x & MASK_NAN) == MASK_NAN) {	// check for NaN
    if ((x & 0x0003ffffffffffffull) > 999999999999999ull)
      x = x & 0xfe00000000000000ull;	// clear G6-G12 and the payload bits
    else
      x = x & 0xfe03ffffffffffffull;	// clear G6-G12
    if ((x & MASK_SNAN) == MASK_SNAN) {	// SNaN
      // set invalid flag
      *pfpsf |= BID_INVALID_EXCEPTION;
      // return quiet (SNaN)
      res = x & 0xfdffffffffffffffull;
    } else {	// QNaN
      res = x;
    }
    BID_RETURN (res);
  } else if ((x & MASK_INF) == MASK_INF) {	// check for Infinity
    res = x_sign | 0x7800000000000000ull;
    BID_RETURN (res);
  }
  // unpack x
  if ((x & MASK_STEERING_BITS) == MASK_STEERING_BITS) {
    // if the steering bits are 11 (condition will be 0), then 
    // the exponent is G[0:w+1]
    exp = ((x & MASK_BINARY_EXPONENT2) >> 51) - 398;
    C1 = (x & MASK_BINARY_SIG2) | MASK_BINARY_OR2;
    if (C1 > 9999999999999999ull) {	// non-canonical
      C1 = 0;
    }
  } else {	// if ((x & MASK_STEERING_BITS) != MASK_STEERING_BITS)
    exp = ((x & MASK_BINARY_EXPONENT1) >> 53) - 398;
    C1 = (x & MASK_BINARY_SIG1);
  }

  // if x is 0 or non-canonical return 0 preserving the sign bit and 
  // the preferred exponent of MAX(Q(x), 0)
  if (C1 == 0) {
    if (exp < 0)
      exp = 0;
    res = x_sign | (((BID_UINT64) exp + 398) << 53);
    BID_RETURN (res);
  }
  // x is a finite non-zero number (not 0, non-canonical, or special)

  switch (rnd_mode) {
  case BID_ROUNDING_TO_NEAREST:
  case BID_ROUNDING_TIES_AWAY:
    // return 0 if (exp <= -(p+1))
    if (exp <= -17) {
      res = x_sign | 0x31c0000000000000ull;
      *pfpsf |= BID_INEXACT_EXCEPTION;
      BID_RETURN (res);
    }
    break;
  case BID_ROUNDING_DOWN:
    // return 0 if (exp <= -p)
    if (exp <= -16) {
      if (x_sign) {
	res = 0xb1c0000000000001ull;
      } else {
	res = 0x31c0000000000000ull;
      }
      *pfpsf |= BID_INEXACT_EXCEPTION;
      BID_RETURN (res);
    }
    break;
  case BID_ROUNDING_UP:
    // return 0 if (exp <= -p)
    if (exp <= -16) {
      if (x_sign) {
	res = 0xb1c0000000000000ull;
      } else {
	res = 0x31c0000000000001ull;
      }
      *pfpsf |= BID_INEXACT_EXCEPTION;
      BID_RETURN (res);
    }
    break;
  case BID_ROUNDING_TO_ZERO:
    // return 0 if (exp <= -p) 
    if (exp <= -16) {
      res = x_sign | 0x31c0000000000000ull;
      *pfpsf |= BID_INEXACT_EXCEPTION;
      BID_RETURN (res);
    }
    break;
  }	// end switch ()

  // q = nr. of decimal digits in x (1 <= q <= 54)
  //  determine first the nr. of bits in x
  if (C1 >= 0x0020000000000000ull) {	// x >= 2^53
    q = 16;
  } else {	// if x < 2^53
    tmp1.d = (double) C1;	// exact conversion
    x_nr_bits =
      1 + ((((unsigned int) (tmp1.ui64 >> 52)) & 0x7ff) - 0x3ff);
    q = bid_nr_digits[x_nr_bits - 1].digits;
    if (q == 0) {
      q = bid_nr_digits[x_nr_bits - 1].digits1;
      if (C1 >= bid_nr_digits[x_nr_bits - 1].threshold_lo)
	q++;
    }
  }

  if (exp >= 0) {	// -exp <= 0
    // the argument is an integer already
    res = x;
    BID_RETURN (res);
  }

  switch (rnd_mode) {
  case BID_ROUNDING_TO_NEAREST:
    if ((q + exp) >= 0) {	// exp < 0 and 1 <= -exp <= q
      // need to shift right -exp digits from the coefficient; exp will be 0
      ind = -exp;	// 1 <= ind <= 16; ind is a synonym for 'x'
      // chop off ind digits from the lower part of C1 
      // C1 = C1 + 1/2 * 10^x where the result C1 fits in 64 bits
      // FOR ROUND_TO_NEAREST, WE ADD 1/2 ULP(y) then truncate
      C1 = C1 + bid_midpoint64[ind - 1];
      // calculate C* and f*
      // C* is actually floor(C*) in this case
      // C* and f* need shifting and masking, as shown by
      // bid_shiftright128[] and bid_maskhigh128[]
      // 1 <= x <= 16
      // kx = 10^(-x) = bid_ten2mk64[ind - 1]
      // C* = (C1 + 1/2 * 10^x) * 10^(-x)
      // the approximation of 10^(-x) was rounded up to 64 bits
      __mul_64x64_to_128 (P128, C1, bid_ten2mk64[ind - 1]);

      // if (0 < f* < 10^(-x)) then the result is a midpoint
      //   if floor(C*) is even then C* = floor(C*) - logical right
      //       shift; C* has p decimal digits, correct by Prop. 1)
      //   else if floor(C*) is odd C* = floor(C*)-1 (logical right
      //       shift; C* has p decimal digits, correct by Pr. 1)
      // else  
      //   C* = floor(C*) (logical right shift; C has p decimal digits,
      //       correct by Property 1)
      // n = C* * 10^(e+x)  

      if (ind - 1 <= 2) {	// 0 <= ind - 1 <= 2 => shift = 0
	res = P128.w[1];
	fstar.w[1] = 0;
	fstar.w[0] = P128.w[0];
      } else if (ind - 1 <= 21) {	// 3 <= ind - 1 <= 21 => 3 <= shift <= 63
	shift = bid_shiftright128[ind - 1];	// 3 <= shift <= 63
	res = (P128.w[1] >> shift);
	fstar.w[1] = P128.w[1] & bid_maskhigh128[ind - 1];
	fstar.w[0] = P128.w[0];
      }
      // if (0 < f* < 10^(-x)) then the result is a midpoint
      // since round_to_even, subtract 1 if current result is odd
      if ((res & 0x0000000000000001ull) && (fstar.w[1] == 0)
	  && (fstar.w[0] < bid_ten2mk64[ind - 1])) {
	res--;
      }
      // determine inexactness of the rounding of C*
      // if (0 < f* - 1/2 < 10^(-x)) then
      //   the result is exact
      // else // if (f* - 1/2 > T*) then
      //   the result is inexact
      if (ind - 1 <= 2) {
	if (fstar.w[0] > 0x8000000000000000ull) {
	  // f* > 1/2 and the result may be exact
	  // fstar.w[0] - 0x8000000000000000ull is f* - 1/2
	  if ((fstar.w[0] - 0x8000000000000000ull) > bid_ten2mk64[ind - 1]) {
	    // set the inexact flag
	    *pfpsf |= BID_INEXACT_EXCEPTION;
	  }	// else the result is exact
	} else {	// the result is inexact; f2* <= 1/2
	  // set the inexact flag
	  *pfpsf |= BID_INEXACT_EXCEPTION;
	}
      } else {	// if 3 <= ind - 1 <= 21
	if (fstar.w[1] > bid_onehalf128[ind - 1] ||
	    (fstar.w[1] == bid_onehalf128[ind - 1] && fstar.w[0])) {
	  // f2* > 1/2 and the result may be exact
	  // Calculate f2* - 1/2
	  if (fstar.w[1] > bid_onehalf128[ind - 1]
	      || fstar.w[0] > bid_ten2mk64[ind - 1]) {
	    // set the inexact flag
	    *pfpsf |= BID_INEXACT_EXCEPTION;
	  }	// else the result is exact
	} else {	// the result is inexact; f2* <= 1/2
	  // set the inexact flag
	  *pfpsf |= BID_INEXACT_EXCEPTION;
	}
      }
      // set exponent to zero as it was negative before.
      res = x_sign | 0x31c0000000000000ull | res;
      BID_RETURN (res);
    } else {	// if exp < 0 and q + exp < 0
      // the result is +0 or -0
      res = x_sign | 0x31c0000000000000ull;
      *pfpsf |= BID_INEXACT_EXCEPTION;
      BID_RETURN (res);
    }
    break;
  case BID_ROUNDING_TIES_AWAY:
    if ((q + exp) >= 0) {	// exp < 0 and 1 <= -exp <= q
      // need to shift right -exp digits from the coefficient; exp will be 0
      ind = -exp;	// 1 <= ind <= 16; ind is a synonym for 'x'
      // chop off ind digits from the lower part of C1 
      // C1 = C1 + 1/2 * 10^x where the result C1 fits in 64 bits
      // FOR ROUND_TO_NEAREST, WE ADD 1/2 ULP(y) then truncate
      C1 = C1 + bid_midpoint64[ind - 1];
      // calculate C* and f*
      // C* is actually floor(C*) in this case
      // C* and f* need shifting and masking, as shown by
      // bid_shiftright128[] and bid_maskhigh128[]
      // 1 <= x <= 16
      // kx = 10^(-x) = bid_ten2mk64[ind - 1]
      // C* = (C1 + 1/2 * 10^x) * 10^(-x)
      // the approximation of 10^(-x) was rounded up to 64 bits
      __mul_64x64_to_128 (P128, C1, bid_ten2mk64[ind - 1]);

      // if (0 < f* < 10^(-x)) then the result is a midpoint
      //   C* = floor(C*) - logical right shift; C* has p decimal digits, 
      //       correct by Prop. 1)
      // else
      //   C* = floor(C*) (logical right shift; C has p decimal digits,
      //       correct by Property 1)
      // n = C* * 10^(e+x)

      if (ind - 1 <= 2) {	// 0 <= ind - 1 <= 2 => shift = 0
	res = P128.w[1];
	fstar.w[1] = 0;
	fstar.w[0] = P128.w[0];
      } else if (ind - 1 <= 21) {	// 3 <= ind - 1 <= 21 => 3 <= shift <= 63
	shift = bid_shiftright128[ind - 1];	// 3 <= shift <= 63
	res = (P128.w[1] >> shift);
	fstar.w[1] = P128.w[1] & bid_maskhigh128[ind - 1];
	fstar.w[0] = P128.w[0];
      }
      // midpoints are already rounded correctly
      // determine inexactness of the rounding of C*
      // if (0 < f* - 1/2 < 10^(-x)) then
      //   the result is exact
      // else // if (f* - 1/2 > T*) then
      //   the result is inexact
      if (ind - 1 <= 2) {
	if (fstar.w[0] > 0x8000000000000000ull) {
	  // f* > 1/2 and the result may be exact 
	  // fstar.w[0] - 0x8000000000000000ull is f* - 1/2
	  if ((fstar.w[0] - 0x8000000000000000ull) > bid_ten2mk64[ind - 1]) {
	    // set the inexact flag
	    *pfpsf |= BID_INEXACT_EXCEPTION;
	  }	// else the result is exact
	} else {	// the result is inexact; f2* <= 1/2
	  // set the inexact flag
	  *pfpsf |= BID_INEXACT_EXCEPTION;
	}
      } else {	// if 3 <= ind - 1 <= 21
	if (fstar.w[1] > bid_onehalf128[ind - 1] ||
	    (fstar.w[1] == bid_onehalf128[ind - 1] && fstar.w[0])) {
	  // f2* > 1/2 and the result may be exact
	  // Calculate f2* - 1/2
	  if (fstar.w[1] > bid_onehalf128[ind - 1]
	      || fstar.w[0] > bid_ten2mk64[ind - 1]) {
	    // set the inexact flag
	    *pfpsf |= BID_INEXACT_EXCEPTION;
	  }	// else the result is exact
	} else {	// the result is inexact; f2* <= 1/2
	  // set the inexact flag
	  *pfpsf |= BID_INEXACT_EXCEPTION;
	}
      }
      // set exponent to zero as it was negative before.
      res = x_sign | 0x31c0000000000000ull | res;
      BID_RETURN (res);
    } else {	// if exp < 0 and q + exp < 0
      // the result is +0 or -0
      res = x_sign | 0x31c0000000000000ull;
      *pfpsf |= BID_INEXACT_EXCEPTION;
      BID_RETURN (res);
    }
    break;
  case BID_ROUNDING_DOWN:
    if ((q + exp) > 0) {	// exp < 0 and 1 <= -exp < q
      // need to shift right -exp digits from the coefficient; exp will be 0
      ind = -exp;	// 1 <= ind <= 16; ind is a synonym for 'x'
      // chop off ind digits from the lower part of C1 
      // C1 fits in 64 bits
      // calculate C* and f*
      // C* is actually floor(C*) in this case
      // C* and f* need shifting and masking, as shown by
      // bid_shiftright128[] and bid_maskhigh128[]
      // 1 <= x <= 16
      // kx = 10^(-x) = bid_ten2mk64[ind - 1]
      // C* = C1 * 10^(-x)
      // the approximation of 10^(-x) was rounded up to 64 bits
      __mul_64x64_to_128 (P128, C1, bid_ten2mk64[ind - 1]);

      // C* = floor(C*) (logical right shift; C has p decimal digits,
      //       correct by Property 1)
      // if (0 < f* < 10^(-x)) then the result is exact
      // n = C* * 10^(e+x)  

      if (ind - 1 <= 2) {	// 0 <= ind - 1 <= 2 => shift = 0
	res = P128.w[1];
	fstar.w[1] = 0;
	fstar.w[0] = P128.w[0];
      } else if (ind - 1 <= 21) {	// 3 <= ind - 1 <= 21 => 3 <= shift <= 63
	shift = bid_shiftright128[ind - 1];	// 3 <= shift <= 63
	res = (P128.w[1] >> shift);
	fstar.w[1] = P128.w[1] & bid_maskhigh128[ind - 1];
	fstar.w[0] = P128.w[0];
      }
      // if (f* > 10^(-x)) then the result is inexact
      if ((fstar.w[1] != 0) || (fstar.w[0] >= bid_ten2mk64[ind - 1])) {
	if (x_sign) {
	  // if negative and not exact, increment magnitude
	  res++;
	}
	*pfpsf |= BID_INEXACT_EXCEPTION;
      }
      // set exponent to zero as it was negative before.
      res = x_sign | 0x31c0000000000000ull | res;
      BID_RETURN (res);
    } else {	// if exp < 0 and q + exp <= 0
      // the result is +0 or -1
      if (x_sign) {
	res = 0xb1c0000000000001ull;
      } else {
	res = 0x31c0000000000000ull;
      }
      *pfpsf |= BID_INEXACT_EXCEPTION;
      BID_RETURN (res);
    }
    break;
  case BID_ROUNDING_UP:
    if ((q + exp) > 0) {	// exp < 0 and 1 <= -exp < q
      // need to shift right -exp digits from the coefficient; exp will be 0
      ind = -exp;	// 1 <= ind <= 16; ind is a synonym for 'x'
      // chop off ind digits from the lower part of C1 
      // C1 fits in 64 bits
      // calculate C* and f*
      // C* is actually floor(C*) in this case
      // C* and f* need shifting and masking, as shown by
      // bid_shiftright128[] and bid_maskhigh128[]
      // 1 <= x <= 16
      // kx = 10^(-x) = bid_ten2mk64[ind - 1]
      // C* = C1 * 10^(-x)
      // the approximation of 10^(-x) was rounded up to 64 bits
      __mul_64x64_to_128 (P128, C1, bid_ten2mk64[ind - 1]);

      // C* = floor(C*) (logical right shift; C has p decimal digits,
      //       correct by Property 1)
      // if (0 < f* < 10^(-x)) then the result is exact
      // n = C* * 10^(e+x)  

      if (ind - 1 <= 2) {	// 0 <= ind - 1 <= 2 => shift = 0
	res = P128.w[1];
	fstar.w[1] = 0;
	fstar.w[0] = P128.w[0];
      } else if (ind - 1 <= 21) {	// 3 <= ind - 1 <= 21 => 3 <= shift <= 63
	shift = bid_shiftright128[ind - 1];	// 3 <= shift <= 63
	res = (P128.w[1] >> shift);
	fstar.w[1] = P128.w[1] & bid_maskhigh128[ind - 1];
	fstar.w[0] = P128.w[0];
      }
      // if (f* > 10^(-x)) then the result is inexact
      if ((fstar.w[1] != 0) || (fstar.w[0] >= bid_ten2mk64[ind - 1])) {
	if (!x_sign) {
	  // if positive and not exact, increment magnitude
	  res++;
	}
	*pfpsf |= BID_INEXACT_EXCEPTION;
      }
      // set exponent to zero as it was negative before.
      res = x_sign | 0x31c0000000000000ull | res;
      BID_RETURN (res);
    } else {	// if exp < 0 and q + exp <= 0
      // the result is -0 or +1
      if (x_sign) {
	res = 0xb1c0000000000000ull;
      } else {
	res = 0x31c0000000000001ull;
      }
      *pfpsf |= BID_INEXACT_EXCEPTION;
      BID_RETURN (res);
    }
    break;
  case BID_ROUNDING_TO_ZERO:
    if ((q + exp) >= 0) {	// exp < 0 and 1 <= -exp <= q
      // need to shift right -exp digits from the coefficient; exp will be 0
      ind = -exp;	// 1 <= ind <= 16; ind is a synonym for 'x'
      // chop off ind digits from the lower part of C1 
      // C1 fits in 127 bits
      // calculate C* and f*
      // C* is actually floor(C*) in this case
      // C* and f* need shifting and masking, as shown by
      // bid_shiftright128[] and bid_maskhigh128[]
      // 1 <= x <= 16
      // kx = 10^(-x) = bid_ten2mk64[ind - 1]
      // C* = C1 * 10^(-x)
      // the approximation of 10^(-x) was rounded up to 64 bits
      __mul_64x64_to_128 (P128, C1, bid_ten2mk64[ind - 1]);

      // C* = floor(C*) (logical right shift; C has p decimal digits,
      //       correct by Property 1)
      // if (0 < f* < 10^(-x)) then the result is exact
      // n = C* * 10^(e+x)  

      if (ind - 1 <= 2) {	// 0 <= ind - 1 <= 2 => shift = 0
	res = P128.w[1];
	fstar.w[1] = 0;
	fstar.w[0] = P128.w[0];
      } else if (ind - 1 <= 21) {	// 3 <= ind - 1 <= 21 => 3 <= shift <= 63
	shift = bid_shiftright128[ind - 1];	// 3 <= shift <= 63
	res = (P128.w[1] >> shift);
	fstar.w[1] = P128.w[1] & bid_maskhigh128[ind - 1];
	fstar.w[0] = P128.w[0];
      }
      // if (f* > 10^(-x)) then the result is inexact
      if ((fstar.w[1] != 0) || (fstar.w[0] >= bid_ten2mk64[ind - 1])) {
	*pfpsf |= BID_INEXACT_EXCEPTION;
      }
      // set exponent to zero as it was negative before.
      res = x_sign | 0x31c0000000000000ull | res;
      BID_RETURN (res);
    } else {	// if exp < 0 and q + exp < 0
      // the result is +0 or -0
      res = x_sign | 0x31c0000000000000ull;
      *pfpsf |= BID_INEXACT_EXCEPTION;
      BID_RETURN (res);
    }
    break;
  }	// end switch ()
  BID_RETURN (res);
}

/*****************************************************************************
 *  BID64_round_integral_nearest_even
 ****************************************************************************/

BID_TYPE0_FUNCTION_ARGTYPE1_NORND_DFP(BID_UINT64, bid64_round_integral_nearest_even, BID_UINT64, x)

  BID_UINT64 res = 0xbaddbaddbaddbaddull;
  BID_UINT64 x_sign;
  int exp;			// unbiased exponent
  // Note: C1.w[1], C1.w[0] represent x_signif_hi, x_signif_lo (all are BID_UINT64)
  BID_UI64DOUBLE tmp1;
  int x_nr_bits;
  int q, ind, shift;
  BID_UINT64 C1;
  BID_UINT128 fstar= { {0x0ull, 0x0ull} };
  BID_UINT128 P128;

  x_sign = x & MASK_SIGN;	// 0 for positive, MASK_SIGN for negative

  // check for NaNs and infinities
  if ((x & MASK_NAN) == MASK_NAN) {	// check for NaN
    if ((x & 0x0003ffffffffffffull) > 999999999999999ull)
      x = x & 0xfe00000000000000ull;	// clear G6-G12 and the payload bits
    else
      x = x & 0xfe03ffffffffffffull;	// clear G6-G12 
    if ((x & MASK_SNAN) == MASK_SNAN) {	// SNaN 
      // set invalid flag 
      *pfpsf |= BID_INVALID_EXCEPTION;
      // return quiet (SNaN) 
      res = x & 0xfdffffffffffffffull;
    } else {	// QNaN
      res = x;
    }
    BID_RETURN (res);
  } else if ((x & MASK_INF) == MASK_INF) {	// check for Infinity
    res = x_sign | 0x7800000000000000ull;
    BID_RETURN (res);
  }
  // unpack x
  if ((x & MASK_STEERING_BITS) == MASK_STEERING_BITS) {
    // if the steering bits are 11 (condition will be 0), then
    // the exponent is G[0:w+1]
    exp = ((x & MASK_BINARY_EXPONENT2) >> 51) - 398;
    C1 = (x & MASK_BINARY_SIG2) | MASK_BINARY_OR2;
    if (C1 > 9999999999999999ull) {	// non-canonical
      C1 = 0;
    }
  } else {	// if ((x & MASK_STEERING_BITS) != MASK_STEERING_BITS)
    exp = ((x & MASK_BINARY_EXPONENT1) >> 53) - 398;
    C1 = (x & MASK_BINARY_SIG1);
  }

  // if x is 0 or non-canonical
  if (C1 == 0) {
    if (exp < 0)
      exp = 0;
    res = x_sign | (((BID_UINT64) exp + 398) << 53);
    BID_RETURN (res);
  }
  // x is a finite non-zero number (not 0, non-canonical, or special)

  // return 0 if (exp <= -(p+1))
  if (exp <= -17) {
    res = x_sign | 0x31c0000000000000ull;
    BID_RETURN (res);
  }
  // q = nr. of decimal digits in x (1 <= q <= 54)
  //  determine first the nr. of bits in x
  if (C1 >= 0x0020000000000000ull) {	// x >= 2^53
    q = 16;
  } else {	// if x < 2^53
    tmp1.d = (double) C1;	// exact conversion
    x_nr_bits =
      1 + ((((unsigned int) (tmp1.ui64 >> 52)) & 0x7ff) - 0x3ff);
    q = bid_nr_digits[x_nr_bits - 1].digits;
    if (q == 0) {
      q = bid_nr_digits[x_nr_bits - 1].digits1;
      if (C1 >= bid_nr_digits[x_nr_bits - 1].threshold_lo)
	q++;
    }
  }

  if (exp >= 0) {	// -exp <= 0
    // the argument is an integer already
    res = x;
    BID_RETURN (res);
  } else if ((q + exp) >= 0) {	// exp < 0 and 1 <= -exp <= q
    // need to shift right -exp digits from the coefficient; the exp will be 0
    ind = -exp;	// 1 <= ind <= 16; ind is a synonym for 'x'
    // chop off ind digits from the lower part of C1 
    // C1 = C1 + 1/2 * 10^x where the result C1 fits in 64 bits
    // FOR ROUND_TO_NEAREST, WE ADD 1/2 ULP(y) then truncate
    C1 = C1 + bid_midpoint64[ind - 1];
    // calculate C* and f*
    // C* is actually floor(C*) in this case
    // C* and f* need shifting and masking, as shown by
    // bid_shiftright128[] and bid_maskhigh128[]
    // 1 <= x <= 16
    // kx = 10^(-x) = bid_ten2mk64[ind - 1]
    // C* = (C1 + 1/2 * 10^x) * 10^(-x)
    // the approximation of 10^(-x) was rounded up to 64 bits
    __mul_64x64_to_128 (P128, C1, bid_ten2mk64[ind - 1]);

    // if (0 < f* < 10^(-x)) then the result is a midpoint
    //   if floor(C*) is even then C* = floor(C*) - logical right
    //       shift; C* has p decimal digits, correct by Prop. 1)
    //   else if floor(C*) is odd C* = floor(C*)-1 (logical right
    //       shift; C* has p decimal digits, correct by Pr. 1)
    // else  
    //   C* = floor(C*) (logical right shift; C has p decimal digits,
    //       correct by Property 1)
    // n = C* * 10^(e+x)  

    if (ind - 1 <= 2) {	// 0 <= ind - 1 <= 2 => shift = 0
      res = P128.w[1];
      fstar.w[1] = 0;
      fstar.w[0] = P128.w[0];
    } else if (ind - 1 <= 21) {	// 3 <= ind - 1 <= 21 => 3 <= shift <= 63
      shift = bid_shiftright128[ind - 1];	// 3 <= shift <= 63
      res = (P128.w[1] >> shift);
      fstar.w[1] = P128.w[1] & bid_maskhigh128[ind - 1];
      fstar.w[0] = P128.w[0];
    }
    // if (0 < f* < 10^(-x)) then the result is a midpoint
    // since round_to_even, subtract 1 if current result is odd
    if ((res & 0x0000000000000001ull) && (fstar.w[1] == 0)
	&& (fstar.w[0] < bid_ten2mk64[ind - 1])) {
      res--;
    }
    // set exponent to zero as it was negative before.
    res = x_sign | 0x31c0000000000000ull | res;
    BID_RETURN (res);
  } else {	// if exp < 0 and q + exp < 0
    // the result is +0 or -0
    res = x_sign | 0x31c0000000000000ull;
    BID_RETURN (res);
  }
}

/*****************************************************************************
 *  BID64_round_integral_negative
 *****************************************************************************/

BID_TYPE0_FUNCTION_ARGTYPE1_NORND_DFP(BID_UINT64, bid64_round_integral_negative, BID_UINT64, x)

  BID_UINT64 res = 0xbaddbaddbaddbaddull;
  BID_UINT64 x_sign;
  int exp;			// unbiased exponent
  // Note: C1.w[1], C1.w[0] represent x_signif_hi, x_signif_lo (all are BID_UINT64)
  BID_UI64DOUBLE tmp1;
  int x_nr_bits;
  int q, ind, shift;
  BID_UINT64 C1;
  // BID_UINT64 res is C* at first - represents up to 34 decimal digits ~ 113 bits
  BID_UINT128 fstar= { {0x0ull, 0x0ull} };
  BID_UINT128 P128;

  x_sign = x & MASK_SIGN;	// 0 for positive, MASK_SIGN for negative

  // check for NaNs and infinities
  if ((x & MASK_NAN) == MASK_NAN) {	// check for NaN
    if ((x & 0x0003ffffffffffffull) > 999999999999999ull)
      x = x & 0xfe00000000000000ull;	// clear G6-G12 and the payload bits
    else
      x = x & 0xfe03ffffffffffffull;	// clear G6-G12 
    if ((x & MASK_SNAN) == MASK_SNAN) {	// SNaN 
      // set invalid flag 
      *pfpsf |= BID_INVALID_EXCEPTION;
      // return quiet (SNaN) 
      res = x & 0xfdffffffffffffffull;
    } else {	// QNaN
      res = x;
    }
    BID_RETURN (res);
  } else if ((x & MASK_INF) == MASK_INF) {	// check for Infinity
    res = x_sign | 0x7800000000000000ull;
    BID_RETURN (res);
  }
  // unpack x
  if ((x & MASK_STEERING_BITS) == MASK_STEERING_BITS) {
    // if the steering bits are 11 (condition will be 0), then
    // the exponent is G[0:w+1]
    exp = ((x & MASK_BINARY_EXPONENT2) >> 51) - 398;
    C1 = (x & MASK_BINARY_SIG2) | MASK_BINARY_OR2;
    if (C1 > 9999999999999999ull) {	// non-canonical
      C1 = 0;
    }
  } else {	// if ((x & MASK_STEERING_BITS) != MASK_STEERING_BITS)
    exp = ((x & MASK_BINARY_EXPONENT1) >> 53) - 398;
    C1 = (x & MASK_BINARY_SIG1);
  }

  // if x is 0 or non-canonical
  if (C1 == 0) {
    if (exp < 0)
      exp = 0;
    res = x_sign | (((BID_UINT64) exp + 398) << 53);
    BID_RETURN (res);
  }
  // x is a finite non-zero number (not 0, non-canonical, or special)

  // return 0 if (exp <= -p)
  if (exp <= -16) {
    if (x_sign) {
      res = 0xb1c0000000000001ull;
    } else {
      res = 0x31c0000000000000ull;
    }
    BID_RETURN (res);
  }
  // q = nr. of decimal digits in x (1 <= q <= 54)
  //  determine first the nr. of bits in x
  if (C1 >= 0x0020000000000000ull) {	// x >= 2^53
    q = 16;
  } else {	// if x < 2^53
    tmp1.d = (double) C1;	// exact conversion
    x_nr_bits =
      1 + ((((unsigned int) (tmp1.ui64 >> 52)) & 0x7ff) - 0x3ff);
    q = bid_nr_digits[x_nr_bits - 1].digits;
    if (q == 0) {
      q = bid_nr_digits[x_nr_bits - 1].digits1;
      if (C1 >= bid_nr_digits[x_nr_bits - 1].threshold_lo)
	q++;
    }
  }

  if (exp >= 0) {	// -exp <= 0
    // the argument is an integer already
    res = x;
    BID_RETURN (res);
  } else if ((q + exp) > 0) {	// exp < 0 and 1 <= -exp < q
    // need to shift right -exp digits from the coefficient; the exp will be 0
    ind = -exp;	// 1 <= ind <= 16; ind is a synonym for 'x'
    // chop off ind digits from the lower part of C1 
    // C1 fits in 64 bits
    // calculate C* and f*
    // C* is actually floor(C*) in this case
    // C* and f* need shifting and masking, as shown by
    // bid_shiftright128[] and bid_maskhigh128[]
    // 1 <= x <= 16
    // kx = 10^(-x) = bid_ten2mk64[ind - 1]
    // C* = C1 * 10^(-x)
    // the approximation of 10^(-x) was rounded up to 64 bits
    __mul_64x64_to_128 (P128, C1, bid_ten2mk64[ind - 1]);

    // C* = floor(C*) (logical right shift; C has p decimal digits,
    //       correct by Property 1)
    // if (0 < f* < 10^(-x)) then the result is exact
    // n = C* * 10^(e+x)  

    if (ind - 1 <= 2) {	// 0 <= ind - 1 <= 2 => shift = 0
      res = P128.w[1];
      fstar.w[1] = 0;
      fstar.w[0] = P128.w[0];
    } else if (ind - 1 <= 21) {	// 3 <= ind - 1 <= 21 => 3 <= shift <= 63
      shift = bid_shiftright128[ind - 1];	// 3 <= shift <= 63
      res = (P128.w[1] >> shift);
      fstar.w[1] = P128.w[1] & bid_maskhigh128[ind - 1];
      fstar.w[0] = P128.w[0];
    }
    // if (f* > 10^(-x)) then the result is inexact
    if (x_sign
	&& ((fstar.w[1] != 0) || (fstar.w[0] >= bid_ten2mk64[ind - 1]))) {
      // if negative and not exact, increment magnitude
      res++;
    }
    // set exponent to zero as it was negative before.
    res = x_sign | 0x31c0000000000000ull | res;
    BID_RETURN (res);
  } else {	// if exp < 0 and q + exp <= 0
    // the result is +0 or -1
    if (x_sign) {
      res = 0xb1c0000000000001ull;
    } else {
      res = 0x31c0000000000000ull;
    }
    BID_RETURN (res);
  }
}

/*****************************************************************************
 *  BID64_round_integral_positive
 ****************************************************************************/

BID_TYPE0_FUNCTION_ARGTYPE1_NORND_DFP(BID_UINT64, bid64_round_integral_positive, BID_UINT64, x)

  BID_UINT64 res = 0xbaddbaddbaddbaddull;
  BID_UINT64 x_sign;
  int exp;			// unbiased exponent
  // Note: C1.w[1], C1.w[0] represent x_signif_hi, x_signif_lo (all are BID_UINT64)
  BID_UI64DOUBLE tmp1;
  int x_nr_bits;
  int q, ind, shift;
  BID_UINT64 C1;
  // BID_UINT64 res is C* at first - represents up to 34 decimal digits ~ 113 bits
  BID_UINT128 fstar= { {0x0ull, 0x0ull} };
  BID_UINT128 P128;

  x_sign = x & MASK_SIGN;	// 0 for positive, MASK_SIGN for negative

  // check for NaNs and infinities
  if ((x & MASK_NAN) == MASK_NAN) {	// check for NaN
    if ((x & 0x0003ffffffffffffull) > 999999999999999ull)
      x = x & 0xfe00000000000000ull;	// clear G6-G12 and the payload bits
    else
      x = x & 0xfe03ffffffffffffull;	// clear G6-G12 
    if ((x & MASK_SNAN) == MASK_SNAN) {	// SNaN 
      // set invalid flag 
      *pfpsf |= BID_INVALID_EXCEPTION;
      // return quiet (SNaN) 
      res = x & 0xfdffffffffffffffull;
    } else {	// QNaN
      res = x;
    }
    BID_RETURN (res);
  } else if ((x & MASK_INF) == MASK_INF) {	// check for Infinity
    res = x_sign | 0x7800000000000000ull;
    BID_RETURN (res);
  }
  // unpack x
  if ((x & MASK_STEERING_BITS) == MASK_STEERING_BITS) {
    // if the steering bits are 11 (condition will be 0), then
    // the exponent is G[0:w+1]
    exp = ((x & MASK_BINARY_EXPONENT2) >> 51) - 398;
    C1 = (x & MASK_BINARY_SIG2) | MASK_BINARY_OR2;
    if (C1 > 9999999999999999ull) {	// non-canonical
      C1 = 0;
    }
  } else {	// if ((x & MASK_STEERING_BITS) != MASK_STEERING_BITS)
    exp = ((x & MASK_BINARY_EXPONENT1) >> 53) - 398;
    C1 = (x & MASK_BINARY_SIG1);
  }

  // if x is 0 or non-canonical
  if (C1 == 0) {
    if (exp < 0)
      exp = 0;
    res = x_sign | (((BID_UINT64) exp + 398) << 53);
    BID_RETURN (res);
  }
  // x is a finite non-zero number (not 0, non-canonical, or special)

  // return 0 if (exp <= -p)
  if (exp <= -16) {
    if (x_sign) {
      res = 0xb1c0000000000000ull;
    } else {
      res = 0x31c0000000000001ull;
    }
    BID_RETURN (res);
  }
  // q = nr. of decimal digits in x (1 <= q <= 54)
  //  determine first the nr. of bits in x
  if (C1 >= 0x0020000000000000ull) {	// x >= 2^53
    q = 16;
  } else {	// if x < 2^53
    tmp1.d = (double) C1;	// exact conversion
    x_nr_bits =
      1 + ((((unsigned int) (tmp1.ui64 >> 52)) & 0x7ff) - 0x3ff);
    q = bid_nr_digits[x_nr_bits - 1].digits;
    if (q == 0) {
      q = bid_nr_digits[x_nr_bits - 1].digits1;
      if (C1 >= bid_nr_digits[x_nr_bits - 1].threshold_lo)
	q++;
    }
  }

  if (exp >= 0) {	// -exp <= 0
    // the argument is an integer already
    res = x;
    BID_RETURN (res);
  } else if ((q + exp) > 0) {	// exp < 0 and 1 <= -exp < q
    // need to shift right -exp digits from the coefficient; the exp will be 0
    ind = -exp;	// 1 <= ind <= 16; ind is a synonym for 'x'
    // chop off ind digits from the lower part of C1 
    // C1 fits in 64 bits
    // calculate C* and f*
    // C* is actually floor(C*) in this case
    // C* and f* need shifting and masking, as shown by
    // bid_shiftright128[] and bid_maskhigh128[]
    // 1 <= x <= 16
    // kx = 10^(-x) = bid_ten2mk64[ind - 1]
    // C* = C1 * 10^(-x)
    // the approximation of 10^(-x) was rounded up to 64 bits
    __mul_64x64_to_128 (P128, C1, bid_ten2mk64[ind - 1]);

    // C* = floor(C*) (logical right shift; C has p decimal digits,
    //       correct by Property 1)
    // if (0 < f* < 10^(-x)) then the result is exact
    // n = C* * 10^(e+x)  

    if (ind - 1 <= 2) {	// 0 <= ind - 1 <= 2 => shift = 0
      res = P128.w[1];
      fstar.w[1] = 0;
      fstar.w[0] = P128.w[0];
    } else if (ind - 1 <= 21) {	// 3 <= ind - 1 <= 21 => 3 <= shift <= 63
      shift = bid_shiftright128[ind - 1];	// 3 <= shift <= 63
      res = (P128.w[1] >> shift);
      fstar.w[1] = P128.w[1] & bid_maskhigh128[ind - 1];
      fstar.w[0] = P128.w[0];
    }
    // if (f* > 10^(-x)) then the result is inexact
    if (!x_sign
	&& ((fstar.w[1] != 0) || (fstar.w[0] >= bid_ten2mk64[ind - 1]))) {
      // if positive and not exact, increment magnitude
      res++;
    }
    // set exponent to zero as it was negative before.
    res = x_sign | 0x31c0000000000000ull | res;
    BID_RETURN (res);
  } else {	// if exp < 0 and q + exp <= 0
    // the result is -0 or +1
    if (x_sign) {
      res = 0xb1c0000000000000ull;
    } else {
      res = 0x31c0000000000001ull;
    }
    BID_RETURN (res);
  }
}

/*****************************************************************************
 *  BID64_round_integral_zero
 ****************************************************************************/

BID_TYPE0_FUNCTION_ARGTYPE1_NORND_DFP(BID_UINT64, bid64_round_integral_zero, BID_UINT64, x)

  BID_UINT64 res = 0xbaddbaddbaddbaddull;
  BID_UINT64 x_sign;
  int exp;			// unbiased exponent
  // Note: C1.w[1], C1.w[0] represent x_signif_hi, x_signif_lo (all are BID_UINT64)
  BID_UI64DOUBLE tmp1;
  int x_nr_bits;
  int q, ind, shift;
  BID_UINT64 C1;
  // BID_UINT64 res is C* at first - represents up to 34 decimal digits ~ 113 bits
  BID_UINT128 P128;

  x_sign = x & MASK_SIGN;	// 0 for positive, MASK_SIGN for negative

  // check for NaNs and infinities
  if ((x & MASK_NAN) == MASK_NAN) {	// check for NaN
    if ((x & 0x0003ffffffffffffull) > 999999999999999ull)
      x = x & 0xfe00000000000000ull;	// clear G6-G12 and the payload bits
    else
      x = x & 0xfe03ffffffffffffull;	// clear G6-G12 
    if ((x & MASK_SNAN) == MASK_SNAN) {	// SNaN 
      // set invalid flag 
      *pfpsf |= BID_INVALID_EXCEPTION;
      // return quiet (SNaN) 
      res = x & 0xfdffffffffffffffull;
    } else {	// QNaN
      res = x;
    }
    BID_RETURN (res);
  } else if ((x & MASK_INF) == MASK_INF) {	// check for Infinity
    res = x_sign | 0x7800000000000000ull;
    BID_RETURN (res);
  }
  // unpack x
  if ((x & MASK_STEERING_BITS) == MASK_STEERING_BITS) {
    // if the steering bits are 11 (condition will be 0), then
    // the exponent is G[0:w+1]
    exp = ((x & MASK_BINARY_EXPONENT2) >> 51) - 398;
    C1 = (x & MASK_BINARY_SIG2) | MASK_BINARY_OR2;
    if (C1 > 9999999999999999ull) {	// non-canonical
      C1 = 0;
    }
  } else {	// if ((x & MASK_STEERING_BITS) != MASK_STEERING_BITS)
    exp = ((x & MASK_BINARY_EXPONENT1) >> 53) - 398;
    C1 = (x & MASK_BINARY_SIG1);
  }

  // if x is 0 or non-canonical
  if (C1 == 0) {
    if (exp < 0)
      exp = 0;
    res = x_sign | (((BID_UINT64) exp + 398) << 53);
    BID_RETURN (res);
  }
  // x is a finite non-zero number (not 0, non-canonical, or special)

  // return 0 if (exp <= -p)
  if (exp <= -16) {
    res = x_sign | 0x31c0000000000000ull;
    BID_RETURN (res);
  }
  // q = nr. of decimal digits in x (1 <= q <= 54)
  //  determine first the nr. of bits in x
  if (C1 >= 0x0020000000000000ull) {	// x >= 2^53
    q = 16;
  } else {	// if x < 2^53
    tmp1.d = (double) C1;	// exact conversion
    x_nr_bits =
      1 + ((((unsigned int) (tmp1.ui64 >> 52)) & 0x7ff) - 0x3ff);
    q = bid_nr_digits[x_nr_bits - 1].digits;
    if (q == 0) {
      q = bid_nr_digits[x_nr_bits - 1].digits1;
      if (C1 >= bid_nr_digits[x_nr_bits - 1].threshold_lo)
	q++;
    }
  }

  if (exp >= 0) {	// -exp <= 0
    // the argument is an integer already
    res = x;
    BID_RETURN (res);
  } else if ((q + exp) >= 0) {	// exp < 0 and 1 <= -exp <= q
    // need to shift right -exp digits from the coefficient; the exp will be 0
    ind = -exp;	// 1 <= ind <= 16; ind is a synonym for 'x'
    // chop off ind digits from the lower part of C1 
    // C1 fits in 127 bits
    // calculate C* and f*
    // C* is actually floor(C*) in this case
    // C* and f* need shifting and masking, as shown by
    // bid_shiftright128[] and bid_maskhigh128[]
    // 1 <= x <= 16
    // kx = 10^(-x) = bid_ten2mk64[ind - 1]
    // C* = C1 * 10^(-x)
    // the approximation of 10^(-x) was rounded up to 64 bits
    __mul_64x64_to_128 (P128, C1, bid_ten2mk64[ind - 1]);

    // C* = floor(C*) (logical right shift; C has p decimal digits,
    //       correct by Property 1)
    // if (0 < f* < 10^(-x)) then the result is exact
    // n = C* * 10^(e+x)  

    if (ind - 1 <= 2) {	// 0 <= ind - 1 <= 2 => shift = 0
      res = P128.w[1];
      // redundant fstar.w[1] = 0;
      // redundant fstar.w[0] = P128.w[0];
    } else if (ind - 1 <= 21) {	// 3 <= ind - 1 <= 21 => 3 <= shift <= 63
      shift = bid_shiftright128[ind - 1];	// 3 <= shift <= 63
      res = (P128.w[1] >> shift);
      // redundant fstar.w[1] = P128.w[1] & bid_maskhigh128[ind - 1];
      // redundant fstar.w[0] = P128.w[0];
    }
    // if (f* > 10^(-x)) then the result is inexact
    // if ((fstar.w[1] != 0) || (fstar.w[0] >= bid_ten2mk64[ind-1])){
    //   // redundant
    // }
    // set exponent to zero as it was negative before.
    res = x_sign | 0x31c0000000000000ull | res;
    BID_RETURN (res);
  } else {	// if exp < 0 and q + exp < 0
    // the result is +0 or -0
    res = x_sign | 0x31c0000000000000ull;
    BID_RETURN (res);
  }
}

/*****************************************************************************
 *  BID64_round_integral_nearest_away
 ****************************************************************************/

BID_TYPE0_FUNCTION_ARGTYPE1_NORND_DFP(BID_UINT64, bid64_round_integral_nearest_away, BID_UINT64, x)

  BID_UINT64 res = 0xbaddbaddbaddbaddull;
  BID_UINT64 x_sign;
  int exp;			// unbiased exponent
  // Note: C1.w[1], C1.w[0] represent x_signif_hi, x_signif_lo (all are BID_UINT64)
  BID_UI64DOUBLE tmp1;
  int x_nr_bits;
  int q, ind, shift;
  BID_UINT64 C1;
  BID_UINT128 P128;

  x_sign = x & MASK_SIGN;	// 0 for positive, MASK_SIGN for negative

  // check for NaNs and infinities
  if ((x & MASK_NAN) == MASK_NAN) {	// check for NaN
    if ((x & 0x0003ffffffffffffull) > 999999999999999ull)
      x = x & 0xfe00000000000000ull;	// clear G6-G12 and the payload bits
    else
      x = x & 0xfe03ffffffffffffull;	// clear G6-G12 
    if ((x & MASK_SNAN) == MASK_SNAN) {	// SNaN 
      // set invalid flag 
      *pfpsf |= BID_INVALID_EXCEPTION;
      // return quiet (SNaN) 
      res = x & 0xfdffffffffffffffull;
    } else {	// QNaN
      res = x;
    }
    BID_RETURN (res);
  } else if ((x & MASK_INF) == MASK_INF) {	// check for Infinity
    res = x_sign | 0x7800000000000000ull;
    BID_RETURN (res);
  }
  // unpack x
  if ((x & MASK_STEERING_BITS) == MASK_STEERING_BITS) {
    // if the steering bits are 11 (condition will be 0), then
    // the exponent is G[0:w+1]
    exp = ((x & MASK_BINARY_EXPONENT2) >> 51) - 398;
    C1 = (x & MASK_BINARY_SIG2) | MASK_BINARY_OR2;
    if (C1 > 9999999999999999ull) {	// non-canonical
      C1 = 0;
    }
  } else {	// if ((x & MASK_STEERING_BITS) != MASK_STEERING_BITS)
    exp = ((x & MASK_BINARY_EXPONENT1) >> 53) - 398;
    C1 = (x & MASK_BINARY_SIG1);
  }

  // if x is 0 or non-canonical
  if (C1 == 0) {
    if (exp < 0)
      exp = 0;
    res = x_sign | (((BID_UINT64) exp + 398) << 53);
    BID_RETURN (res);
  }
  // x is a finite non-zero number (not 0, non-canonical, or special)

  // return 0 if (exp <= -(p+1))
  if (exp <= -17) {
    res = x_sign | 0x31c0000000000000ull;
    BID_RETURN (res);
  }
  // q = nr. of decimal digits in x (1 <= q <= 54)
  //  determine first the nr. of bits in x
  if (C1 >= 0x0020000000000000ull) {	// x >= 2^53
    q = 16;
  } else {	// if x < 2^53
    tmp1.d = (double) C1;	// exact conversion
    x_nr_bits =
      1 + ((((unsigned int) (tmp1.ui64 >> 52)) & 0x7ff) - 0x3ff);
    q = bid_nr_digits[x_nr_bits - 1].digits;
    if (q == 0) {
      q = bid_nr_digits[x_nr_bits - 1].digits1;
      if (C1 >= bid_nr_digits[x_nr_bits - 1].threshold_lo)
	q++;
    }
  }

  if (exp >= 0) {	// -exp <= 0
    // the argument is an integer already
    res = x;
    BID_RETURN (res);
  } else if ((q + exp) >= 0) {	// exp < 0 and 1 <= -exp <= q
    // need to shift right -exp digits from the coefficient; the exp will be 0
    ind = -exp;	// 1 <= ind <= 16; ind is a synonym for 'x'
    // chop off ind digits from the lower part of C1 
    // C1 = C1 + 1/2 * 10^x where the result C1 fits in 64 bits
    // FOR ROUND_TO_NEAREST, WE ADD 1/2 ULP(y) then truncate
    C1 = C1 + bid_midpoint64[ind - 1];
    // calculate C* and f*
    // C* is actually floor(C*) in this case
    // C* and f* need shifting and masking, as shown by
    // bid_shiftright128[] and bid_maskhigh128[]
    // 1 <= x <= 16
    // kx = 10^(-x) = bid_ten2mk64[ind - 1]
    // C* = (C1 + 1/2 * 10^x) * 10^(-x)
    // the approximation of 10^(-x) was rounded up to 64 bits
    __mul_64x64_to_128 (P128, C1, bid_ten2mk64[ind - 1]);

    // if (0 < f* < 10^(-x)) then the result is a midpoint
    //   C* = floor(C*) - logical right shift; C* has p decimal digits, 
    //       correct by Prop. 1)
    // else
    //   C* = floor(C*) (logical right shift; C has p decimal digits,
    //       correct by Property 1)
    // n = C* * 10^(e+x)

    if (ind - 1 <= 2) {	// 0 <= ind - 1 <= 2 => shift = 0
      res = P128.w[1];
    } else if (ind - 1 <= 21) {	// 3 <= ind - 1 <= 21 => 3 <= shift <= 63
      shift = bid_shiftright128[ind - 1];	// 3 <= shift <= 63
      res = (P128.w[1] >> shift);
    }
    // midpoints are already rounded correctly
    // set exponent to zero as it was negative before.
    res = x_sign | 0x31c0000000000000ull | res;
    BID_RETURN (res);
  } else {	// if exp < 0 and q + exp < 0
    // the result is +0 or -0
    res = x_sign | 0x31c0000000000000ull;
    BID_RETURN (res);
  }
}
