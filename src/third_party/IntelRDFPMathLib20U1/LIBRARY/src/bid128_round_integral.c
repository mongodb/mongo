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

#include "bid_internal.h"

/*****************************************************************************
 *  BID128_round_integral_exact
 ****************************************************************************/

BID128_FUNCTION_ARG1 (bid128_round_integral_exact, x)

     BID_UINT128 res = { {0xbaddbaddbaddbaddull, 0xbaddbaddbaddbaddull}
     };
BID_UINT64 x_sign;
BID_UINT64 x_exp;
int exp;			// unbiased exponent
  // Note: C1.w[1], C1.w[0] represent x_signif_hi, x_signif_lo (all are BID_UINT64)
BID_UINT64 tmp64;
BID_UI64DOUBLE tmp1;
unsigned int x_nr_bits = 0;
int q, ind, shift;
BID_UINT128 C1;
BID_UINT256 fstar;
BID_UINT256 P256;

  // check for NaN or Infinity
if ((x.w[1] & MASK_SPECIAL) == MASK_SPECIAL) {
  // x is special
  if ((x.w[1] & MASK_NAN) == MASK_NAN) {	// x is NAN
    // if x = NaN, then res = Q (x)
    // check first for non-canonical NaN payload
    if (((x.w[1] & 0x00003fffffffffffull) > 0x0000314dc6448d93ull) ||
	(((x.w[1] & 0x00003fffffffffffull) == 0x0000314dc6448d93ull) &&
	 (x.w[0] > 0x38c15b09ffffffffull))) {
      x.w[1] = x.w[1] & 0xffffc00000000000ull;
      x.w[0] = 0x0ull;
    }
    if ((x.w[1] & MASK_SNAN) == MASK_SNAN) {	// x is SNAN
      // set invalid flag
      *pfpsf |= BID_INVALID_EXCEPTION;
      // return quiet (x)
      res.w[1] = x.w[1] & 0xfc003fffffffffffull;	// clear out also G[6]-G[16]
      res.w[0] = x.w[0];
    } else {	// x is QNaN
      // return x
      res.w[1] = x.w[1] & 0xfc003fffffffffffull;	// clear out G[6]-G[16]
      res.w[0] = x.w[0];
    }
    BID_RETURN (res)
  } else {	// x is not a NaN, so it must be infinity
    if ((x.w[1] & MASK_SIGN) == 0x0ull) {	// x is +inf
      // return +inf
      res.w[1] = 0x7800000000000000ull;
      res.w[0] = 0x0000000000000000ull;
    } else {	// x is -inf 
      // return -inf
      res.w[1] = 0xf800000000000000ull;
      res.w[0] = 0x0000000000000000ull;
    }
    BID_RETURN (res);
  }
}
  // unpack x
x_sign = x.w[1] & MASK_SIGN;	// 0 for positive, MASK_SIGN for negative
C1.w[1] = x.w[1] & MASK_COEFF;
C1.w[0] = x.w[0];

  // check for non-canonical values (treated as zero)
if ((x.w[1] & 0x6000000000000000ull) == 0x6000000000000000ull) {	// G0_G1=11
  // non-canonical
  x_exp = (x.w[1] << 2) & MASK_EXP;	// biased and shifted left 49 bits
  C1.w[1] = 0;	// significand high
  C1.w[0] = 0;	// significand low
} else {	// G0_G1 != 11
  x_exp = x.w[1] & MASK_EXP;	// biased and shifted left 49 bits
  if (C1.w[1] > 0x0001ed09bead87c0ull ||
      (C1.w[1] == 0x0001ed09bead87c0ull
       && C1.w[0] > 0x378d8e63ffffffffull)) {
    // x is non-canonical if coefficient is larger than 10^34 -1
    C1.w[1] = 0;
    C1.w[0] = 0;
  } else {	// canonical
    ;
  }
}

  // test for input equal to zero
if ((C1.w[1] == 0x0ull) && (C1.w[0] == 0x0ull)) {
  // x is 0
  // return 0 preserving the sign bit and the preferred exponent
  // of MAX(Q(x), 0)
  if (x_exp <= (0x1820ull << 49)) {
    res.w[1] = (x.w[1] & 0x8000000000000000ull) | 0x3040000000000000ull;
  } else {
    res.w[1] = x_sign | x_exp;
  }
  res.w[0] = 0x0000000000000000ull;
  BID_RETURN (res);
}
  // x is not special and is not zero

switch (rnd_mode) {
case BID_ROUNDING_TO_NEAREST:
case BID_ROUNDING_TIES_AWAY:
  // if (exp <= -(p+1)) return 0.0
  if (x_exp <= 0x2ffa000000000000ull) {	// 0x2ffa000000000000ull == -35
    res.w[1] = x_sign | 0x3040000000000000ull;
    res.w[0] = 0x0000000000000000ull;
    *pfpsf |= BID_INEXACT_EXCEPTION;
    BID_RETURN (res);
  }
  break;
case BID_ROUNDING_DOWN:
  // if (exp <= -p) return -1.0 or +0.0
  if (x_exp <= 0x2ffc000000000000ull) {	// 0x2ffa000000000000ull == -34
    if (x_sign) {
      // if negative, return negative 1, because we know coefficient
      // is non-zero (would have been caught above)
      res.w[1] = 0xb040000000000000ull;
      res.w[0] = 0x0000000000000001ull;
    } else {
      // if positive, return positive 0, because we know coefficient is
      // non-zero (would have been caught above)
      res.w[1] = 0x3040000000000000ull;
      res.w[0] = 0x0000000000000000ull;
    }
    *pfpsf |= BID_INEXACT_EXCEPTION;
    BID_RETURN (res);
  }
  break;
case BID_ROUNDING_UP:
  // if (exp <= -p) return -0.0 or +1.0
  if (x_exp <= 0x2ffc000000000000ull) {	// 0x2ffc000000000000ull == -34
    if (x_sign) {
      // if negative, return negative 0, because we know the coefficient
      // is non-zero (would have been caught above)
      res.w[1] = 0xb040000000000000ull;
      res.w[0] = 0x0000000000000000ull;
    } else {
      // if positive, return positive 1, because we know coefficient is
      // non-zero (would have been caught above)
      res.w[1] = 0x3040000000000000ull;
      res.w[0] = 0x0000000000000001ull;
    }
    *pfpsf |= BID_INEXACT_EXCEPTION;
    BID_RETURN (res);
  }
  break;
case BID_ROUNDING_TO_ZERO:
  // if (exp <= -p) return -0.0 or +0.0
  if (x_exp <= 0x2ffc000000000000ull) {	// 0x2ffc000000000000ull == -34
    res.w[1] = x_sign | 0x3040000000000000ull;
    res.w[0] = 0x0000000000000000ull;
    *pfpsf |= BID_INEXACT_EXCEPTION;
    BID_RETURN (res);
  }
  break;
}

  // q = nr. of decimal digits in x
  //  determine first the nr. of bits in x
if (C1.w[1] == 0) {
  if (C1.w[0] >= 0x0020000000000000ull) {	// x >= 2^53
    // split the 64-bit value in two 32-bit halves to avoid rounding errors
    tmp1.d = (double) (C1.w[0] >> 32);	// exact conversion
    x_nr_bits = 33 + ((((unsigned int) (tmp1.ui64 >> 52)) & 0x7ff) - 0x3ff);
  } else {	// if x < 2^53
    tmp1.d = (double) C1.w[0];	// exact conversion
    x_nr_bits =
      1 + ((((unsigned int) (tmp1.ui64 >> 52)) & 0x7ff) - 0x3ff);
  }
} else {	// C1.w[1] != 0 => nr. bits = 64 + nr_bits (C1.w[1])
  tmp1.d = (double) C1.w[1];	// exact conversion
  x_nr_bits =
    65 + ((((unsigned int) (tmp1.ui64 >> 52)) & 0x7ff) - 0x3ff);
}

q = bid_nr_digits[x_nr_bits - 1].digits;
if (q == 0) {
  q = bid_nr_digits[x_nr_bits - 1].digits1;
  if (C1.w[1] > bid_nr_digits[x_nr_bits - 1].threshold_hi ||
      (C1.w[1] == bid_nr_digits[x_nr_bits - 1].threshold_hi &&
       C1.w[0] >= bid_nr_digits[x_nr_bits - 1].threshold_lo))
    q++;
}
exp = (x_exp >> 49) - 6176;
if (exp >= 0) {	// -exp <= 0
  // the argument is an integer already
  res.w[1] = x.w[1];
  res.w[0] = x.w[0];
  BID_RETURN (res);
}
  // exp < 0
switch (rnd_mode) {
case BID_ROUNDING_TO_NEAREST:
  if ((q + exp) >= 0) {	// exp < 0 and 1 <= -exp <= q
    // need to shift right -exp digits from the coefficient; exp will be 0
    ind = -exp;	// 1 <= ind <= 34; ind is a synonym for 'x'
    // chop off ind digits from the lower part of C1 
    // C1 = C1 + 1/2 * 10^x where the result C1 fits in 127 bits
    tmp64 = C1.w[0];
    if (ind <= 19) {
      C1.w[0] = C1.w[0] + bid_midpoint64[ind - 1];
    } else {
      C1.w[0] = C1.w[0] + bid_midpoint128[ind - 20].w[0];
      C1.w[1] = C1.w[1] + bid_midpoint128[ind - 20].w[1];
    }
    if (C1.w[0] < tmp64)
      C1.w[1]++;
    // calculate C* and f*
    // C* is actually floor(C*) in this case
    // C* and f* need shifting and masking, as shown by
    // bid_shiftright128[] and bid_maskhigh128[]
    // 1 <= x <= 34
    // kx = 10^(-x) = bid_ten2mk128[ind - 1]
    // C* = (C1 + 1/2 * 10^x) * 10^(-x)
    // the approximation of 10^(-x) was rounded up to 118 bits
    __mul_128x128_to_256 (P256, C1, bid_ten2mk128[ind - 1]);
    // determine the value of res and fstar

    // determine inexactness of the rounding of C*
    // if (0 < f* - 1/2 < 10^(-x)) then
    //   the result is exact
    // else // if (f* - 1/2 > T*) then
    //   the result is inexact
    // Note: we are going to use bid_ten2mk128[] instead of bid_ten2mk128trunc[]

    if (ind - 1 <= 2) {	// 0 <= ind - 1 <= 2 => shift = 0
      // redundant shift = bid_shiftright128[ind - 1]; // shift = 0
      res.w[1] = P256.w[3];
      res.w[0] = P256.w[2];
      // redundant fstar.w[3] = 0;
      // redundant fstar.w[2] = 0;
      fstar.w[1] = P256.w[1];
      fstar.w[0] = P256.w[0];
      // fraction f* < 10^(-x) <=> midpoint
      // f* is in the right position to be compared with
      // 10^(-x) from bid_ten2mk128[]
      // if 0 < fstar < 10^(-x), subtract 1 if odd (for rounding to even)
      if ((res.w[0] & 0x0000000000000001ull) &&	// is result odd, and from MP?
	  ((fstar.w[1] < (bid_ten2mk128[ind - 1].w[1]))
	   || ((fstar.w[1] == bid_ten2mk128[ind - 1].w[1])
	       && (fstar.w[0] < bid_ten2mk128[ind - 1].w[0])))) {
	// subtract 1 to make even
	res.w[0]--;
      }
      if (fstar.w[1] > 0x8000000000000000ull ||
	  (fstar.w[1] == 0x8000000000000000ull
	   && fstar.w[0] > 0x0ull)) {
	// f* > 1/2 and the result may be exact
	tmp64 = fstar.w[1] - 0x8000000000000000ull;	// f* - 1/2
	if (tmp64 > bid_ten2mk128[ind - 1].w[1] ||
	    (tmp64 == bid_ten2mk128[ind - 1].w[1] &&
	     fstar.w[0] >= bid_ten2mk128[ind - 1].w[0])) {
	  // set the inexact flag
	  *pfpsf |= BID_INEXACT_EXCEPTION;
	}	// else the result is exact 
      } else {	// the result is inexact; f2* <= 1/2  
	// set the inexact flag 
	*pfpsf |= BID_INEXACT_EXCEPTION;
      }
    } else if (ind - 1 <= 21) {	// 3 <= ind - 1 <= 21 => 3 <= shift <= 63
      shift = bid_shiftright128[ind - 1];	// 3 <= shift <= 63
      res.w[1] = (P256.w[3] >> shift);
      res.w[0] = (P256.w[3] << (64 - shift)) | (P256.w[2] >> shift);
      // redundant fstar.w[3] = 0;
      fstar.w[2] = P256.w[2] & bid_maskhigh128[ind - 1];
      fstar.w[1] = P256.w[1];
      fstar.w[0] = P256.w[0];
      // fraction f* < 10^(-x) <=> midpoint
      // f* is in the right position to be compared with
      // 10^(-x) from bid_ten2mk128[]
      if ((res.w[0] & 0x0000000000000001ull) &&	// is result odd, and from MP?
	  fstar.w[2] == 0 && (fstar.w[1] < bid_ten2mk128[ind - 1].w[1] ||
			      (fstar.w[1] == bid_ten2mk128[ind - 1].w[1] &&
			       fstar.w[0] < bid_ten2mk128[ind - 1].w[0]))) {
	// subtract 1 to make even
	res.w[0]--;
      }
      if (fstar.w[2] > bid_onehalf128[ind - 1] ||
	  (fstar.w[2] == bid_onehalf128[ind - 1]
	   && (fstar.w[1] || fstar.w[0]))) {
	// f2* > 1/2 and the result may be exact
	// Calculate f2* - 1/2
	tmp64 = fstar.w[2] - bid_onehalf128[ind - 1];
	if (tmp64 || fstar.w[1] > bid_ten2mk128[ind - 1].w[1] ||
	    (fstar.w[1] == bid_ten2mk128[ind - 1].w[1] &&
	     fstar.w[0] >= bid_ten2mk128[ind - 1].w[0])) {
	  // set the inexact flag
	  *pfpsf |= BID_INEXACT_EXCEPTION;
	}	// else the result is exact
      } else {	// the result is inexact; f2* <= 1/2
	// set the inexact flag
	*pfpsf |= BID_INEXACT_EXCEPTION;
      }
    } else {	// 22 <= ind - 1 <= 33
      shift = bid_shiftright128[ind - 1] - 64;	// 2 <= shift <= 38
      res.w[1] = 0;
      res.w[0] = P256.w[3] >> shift;
      fstar.w[3] = P256.w[3] & bid_maskhigh128[ind - 1];
      fstar.w[2] = P256.w[2];
      fstar.w[1] = P256.w[1];
      fstar.w[0] = P256.w[0];
      // fraction f* < 10^(-x) <=> midpoint
      // f* is in the right position to be compared with
      // 10^(-x) from bid_ten2mk128[]
      if ((res.w[0] & 0x0000000000000001ull) &&	// is result odd, and from MP?
	  fstar.w[3] == 0 && fstar.w[2] == 0 &&
	  (fstar.w[1] < bid_ten2mk128[ind - 1].w[1] ||
	   (fstar.w[1] == bid_ten2mk128[ind - 1].w[1] &&
	    fstar.w[0] < bid_ten2mk128[ind - 1].w[0]))) {
	// subtract 1 to make even
	res.w[0]--;
      }
      if (fstar.w[3] > bid_onehalf128[ind - 1] ||
	  (fstar.w[3] == bid_onehalf128[ind - 1] &&
	   (fstar.w[2] || fstar.w[1] || fstar.w[0]))) {
	// f2* > 1/2 and the result may be exact
	// Calculate f2* - 1/2
	tmp64 = fstar.w[3] - bid_onehalf128[ind - 1];
	if (tmp64 || fstar.w[2] || fstar.w[1] > bid_ten2mk128[ind - 1].w[1]
	    || (fstar.w[1] == bid_ten2mk128[ind - 1].w[1]
		&& fstar.w[0] >= bid_ten2mk128[ind - 1].w[0])) {
	  // set the inexact flag
	  *pfpsf |= BID_INEXACT_EXCEPTION;
	}	// else the result is exact
      } else {	// the result is inexact; f2* <= 1/2
	// set the inexact flag
	*pfpsf |= BID_INEXACT_EXCEPTION;
      }
    }
    res.w[1] = x_sign | 0x3040000000000000ull | res.w[1];
    BID_RETURN (res);
  } else {	// if ((q + exp) < 0) <=> q < -exp
    // the result is +0 or -0
    res.w[1] = x_sign | 0x3040000000000000ull;
    res.w[0] = 0x0000000000000000ull;
    *pfpsf |= BID_INEXACT_EXCEPTION;
    BID_RETURN (res);
  }
  break;
case BID_ROUNDING_TIES_AWAY:
  if ((q + exp) >= 0) {	// exp < 0 and 1 <= -exp <= q
    // need to shift right -exp digits from the coefficient; exp will be 0
    ind = -exp;	// 1 <= ind <= 34; ind is a synonym for 'x'
    // chop off ind digits from the lower part of C1 
    // C1 = C1 + 1/2 * 10^x where the result C1 fits in 127 bits
    tmp64 = C1.w[0];
    if (ind <= 19) {
      C1.w[0] = C1.w[0] + bid_midpoint64[ind - 1];
    } else {
      C1.w[0] = C1.w[0] + bid_midpoint128[ind - 20].w[0];
      C1.w[1] = C1.w[1] + bid_midpoint128[ind - 20].w[1];
    }
    if (C1.w[0] < tmp64)
      C1.w[1]++;
    // calculate C* and f*
    // C* is actually floor(C*) in this case
    // C* and f* need shifting and masking, as shown by
    // bid_shiftright128[] and bid_maskhigh128[]
    // 1 <= x <= 34
    // kx = 10^(-x) = bid_ten2mk128[ind - 1]
    // C* = (C1 + 1/2 * 10^x) * 10^(-x)
    // the approximation of 10^(-x) was rounded up to 118 bits
    __mul_128x128_to_256 (P256, C1, bid_ten2mk128[ind - 1]);
    // the top Ex bits of 10^(-x) are T* = bid_ten2mk128trunc[ind], e.g.
    // if x=1, T*=bid_ten2mk128trunc[0]=0x19999999999999999999999999999999
    // if (0 < f* < 10^(-x)) then the result is a midpoint
    //   if floor(C*) is even then C* = floor(C*) - logical right
    //       shift; C* has p decimal digits, correct by Prop. 1)
    //   else if floor(C*) is odd C* = floor(C*)-1 (logical right
    //       shift; C* has p decimal digits, correct by Pr. 1)
    // else
    //   C* = floor(C*) (logical right shift; C has p decimal digits,
    //       correct by Property 1)
    // n = C* * 10^(e+x)

    // determine also the inexactness of the rounding of C*
    // if (0 < f* - 1/2 < 10^(-x)) then
    //   the result is exact
    // else // if (f* - 1/2 > T*) then
    //   the result is inexact
    // Note: we are going to use bid_ten2mk128[] instead of bid_ten2mk128trunc[]
    // shift right C* by Ex-128 = bid_shiftright128[ind]
    if (ind - 1 <= 2) {	// 0 <= ind - 1 <= 2 => shift = 0
      // redundant shift = bid_shiftright128[ind - 1]; // shift = 0
      res.w[1] = P256.w[3];
      res.w[0] = P256.w[2];
      // redundant fstar.w[3] = 0;
      // redundant fstar.w[2] = 0;
      fstar.w[1] = P256.w[1];
      fstar.w[0] = P256.w[0];
      if (fstar.w[1] > 0x8000000000000000ull ||
	  (fstar.w[1] == 0x8000000000000000ull
	   && fstar.w[0] > 0x0ull)) {
	// f* > 1/2 and the result may be exact
	tmp64 = fstar.w[1] - 0x8000000000000000ull;	// f* - 1/2
	if ((tmp64 > bid_ten2mk128[ind - 1].w[1] ||
	     (tmp64 == bid_ten2mk128[ind - 1].w[1] &&
	      fstar.w[0] >= bid_ten2mk128[ind - 1].w[0]))) {
	  // set the inexact flag
	  *pfpsf |= BID_INEXACT_EXCEPTION;
	}	// else the result is exact
      } else {	// the result is inexact; f2* <= 1/2
	// set the inexact flag
	*pfpsf |= BID_INEXACT_EXCEPTION;
      }
    } else if (ind - 1 <= 21) {	// 3 <= ind - 1 <= 21 => 3 <= shift <= 63
      shift = bid_shiftright128[ind - 1];	// 3 <= shift <= 63
      res.w[1] = (P256.w[3] >> shift);
      res.w[0] = (P256.w[3] << (64 - shift)) | (P256.w[2] >> shift);
      // redundant fstar.w[3] = 0;
      fstar.w[2] = P256.w[2] & bid_maskhigh128[ind - 1];
      fstar.w[1] = P256.w[1];
      fstar.w[0] = P256.w[0];
      if (fstar.w[2] > bid_onehalf128[ind - 1] ||
	  (fstar.w[2] == bid_onehalf128[ind - 1]
	   && (fstar.w[1] || fstar.w[0]))) {
	// f2* > 1/2 and the result may be exact
	// Calculate f2* - 1/2
	tmp64 = fstar.w[2] - bid_onehalf128[ind - 1];
	if (tmp64 || fstar.w[1] > bid_ten2mk128[ind - 1].w[1] ||
	    (fstar.w[1] == bid_ten2mk128[ind - 1].w[1] &&
	     fstar.w[0] >= bid_ten2mk128[ind - 1].w[0])) {
	  // set the inexact flag
	  *pfpsf |= BID_INEXACT_EXCEPTION;
	}	// else the result is exact
      } else {	// the result is inexact; f2* <= 1/2
	// set the inexact flag
	*pfpsf |= BID_INEXACT_EXCEPTION;
      }
    } else {	// 22 <= ind - 1 <= 33
      shift = bid_shiftright128[ind - 1] - 64;	// 2 <= shift <= 38
      res.w[1] = 0;
      res.w[0] = P256.w[3] >> shift;
      fstar.w[3] = P256.w[3] & bid_maskhigh128[ind - 1];
      fstar.w[2] = P256.w[2];
      fstar.w[1] = P256.w[1];
      fstar.w[0] = P256.w[0];
      if (fstar.w[3] > bid_onehalf128[ind - 1] ||
	  (fstar.w[3] == bid_onehalf128[ind - 1] &&
	   (fstar.w[2] || fstar.w[1] || fstar.w[0]))) {
	// f2* > 1/2 and the result may be exact
	// Calculate f2* - 1/2
	tmp64 = fstar.w[3] - bid_onehalf128[ind - 1];
	if (tmp64 || fstar.w[2] || fstar.w[1] > bid_ten2mk128[ind - 1].w[1]
	    || (fstar.w[1] == bid_ten2mk128[ind - 1].w[1]
		&& fstar.w[0] >= bid_ten2mk128[ind - 1].w[0])) {
	  // set the inexact flag
	  *pfpsf |= BID_INEXACT_EXCEPTION;
	}	// else the result is exact
      } else {	// the result is inexact; f2* <= 1/2
	// set the inexact flag
	*pfpsf |= BID_INEXACT_EXCEPTION;
      }
    }
    // if the result was a midpoint, it was already rounded away from zero
    res.w[1] |= x_sign | 0x3040000000000000ull;
    BID_RETURN (res);
  } else {	// if ((q + exp) < 0) <=> q < -exp
    // the result is +0 or -0
    res.w[1] = x_sign | 0x3040000000000000ull;
    res.w[0] = 0x0000000000000000ull;
    *pfpsf |= BID_INEXACT_EXCEPTION;
    BID_RETURN (res);
  }
  break;
case BID_ROUNDING_DOWN:
  if ((q + exp) > 0) {	// exp < 0 and 1 <= -exp < q
    // need to shift right -exp digits from the coefficient; exp will be 0
    ind = -exp;	// 1 <= ind <= 34; ind is a synonym for 'x' 
    // (number of digits to be chopped off)
    // chop off ind digits from the lower part of C1 
    // FOR ROUND_TO_NEAREST, WE ADD 1/2 ULP(y) then truncate
    // FOR ROUND_TO_ZERO, WE DON'T NEED TO ADD 1/2 ULP
    // FOR ROUND_TO_POSITIVE_INFINITY, WE TRUNCATE, THEN ADD 1 IF POSITIVE
    // FOR ROUND_TO_NEGATIVE_INFINITY, WE TRUNCATE, THEN ADD 1 IF NEGATIVE
    // tmp64 = C1.w[0];
    // if (ind <= 19) {
    //   C1.w[0] = C1.w[0] + bid_midpoint64[ind - 1];
    // } else {
    //   C1.w[0] = C1.w[0] + bid_midpoint128[ind - 20].w[0];
    //   C1.w[1] = C1.w[1] + bid_midpoint128[ind - 20].w[1];
    // }
    // if (C1.w[0] < tmp64) C1.w[1]++;
    // if carry-out from C1.w[0], increment C1.w[1]
    // calculate C* and f*
    // C* is actually floor(C*) in this case
    // C* and f* need shifting and masking, as shown by
    // bid_shiftright128[] and bid_maskhigh128[]
    // 1 <= x <= 34
    // kx = 10^(-x) = bid_ten2mk128[ind - 1]
    // C* = (C1 + 1/2 * 10^x) * 10^(-x)
    // the approximation of 10^(-x) was rounded up to 118 bits
    __mul_128x128_to_256 (P256, C1, bid_ten2mk128[ind - 1]);
    if (ind - 1 <= 2) {	// 0 <= ind - 1 <= 2 => shift = 0
      res.w[1] = P256.w[3];
      res.w[0] = P256.w[2];
      // redundant fstar.w[3] = 0;
      // redundant fstar.w[2] = 0;
      // redundant fstar.w[1] = P256.w[1];
      // redundant fstar.w[0] = P256.w[0];
      // fraction f* > 10^(-x) <=> inexact
      // f* is in the right position to be compared with
      // 10^(-x) from bid_ten2mk128[]
      if ((P256.w[1] > bid_ten2mk128[ind - 1].w[1])
	  || (P256.w[1] == bid_ten2mk128[ind - 1].w[1]
	      && (P256.w[0] >= bid_ten2mk128[ind - 1].w[0]))) {
	*pfpsf |= BID_INEXACT_EXCEPTION;
	// if positive, the truncated value is already the correct result
	if (x_sign) {	// if negative
	  if (++res.w[0] == 0) {
	    res.w[1]++;
	  }
	}
      }
    } else if (ind - 1 <= 21) {	// 3 <= ind - 1 <= 21 => 3 <= shift <= 63
      shift = bid_shiftright128[ind - 1];	// 0 <= shift <= 102
      res.w[1] = (P256.w[3] >> shift);
      res.w[0] = (P256.w[3] << (64 - shift)) | (P256.w[2] >> shift);
      // redundant fstar.w[3] = 0;
      fstar.w[2] = P256.w[2] & bid_maskhigh128[ind - 1];
      fstar.w[1] = P256.w[1];
      fstar.w[0] = P256.w[0];
      // fraction f* > 10^(-x) <=> inexact
      // f* is in the right position to be compared with
      // 10^(-x) from bid_ten2mk128[]
      if (fstar.w[2] || fstar.w[1] > bid_ten2mk128[ind - 1].w[1] ||
	  (fstar.w[1] == bid_ten2mk128[ind - 1].w[1] &&
	   fstar.w[0] >= bid_ten2mk128[ind - 1].w[0])) {
	*pfpsf |= BID_INEXACT_EXCEPTION;
	// if positive, the truncated value is already the correct result
	if (x_sign) {	// if negative
	  if (++res.w[0] == 0) {
	    res.w[1]++;
	  }
	}
      }
    } else {	// 22 <= ind - 1 <= 33
      shift = bid_shiftright128[ind - 1] - 64;	// 2 <= shift <= 38
      res.w[1] = 0;
      res.w[0] = P256.w[3] >> shift;
      fstar.w[3] = P256.w[3] & bid_maskhigh128[ind - 1];
      fstar.w[2] = P256.w[2];
      fstar.w[1] = P256.w[1];
      fstar.w[0] = P256.w[0];
      // fraction f* > 10^(-x) <=> inexact
      // f* is in the right position to be compared with
      // 10^(-x) from bid_ten2mk128[]
      if (fstar.w[3] || fstar.w[2]
	  || fstar.w[1] > bid_ten2mk128[ind - 1].w[1]
	  || (fstar.w[1] == bid_ten2mk128[ind - 1].w[1]
	      && fstar.w[0] >= bid_ten2mk128[ind - 1].w[0])) {
	*pfpsf |= BID_INEXACT_EXCEPTION;
	// if positive, the truncated value is already the correct result
	if (x_sign) {	// if negative
	  if (++res.w[0] == 0) {
	    res.w[1]++;
	  }
	}
      }
    }
    res.w[1] = x_sign | 0x3040000000000000ull | res.w[1];
    BID_RETURN (res);
  } else {	// if exp < 0 and q + exp <= 0
    if (x_sign) {	// negative rounds down to -1.0
      res.w[1] = 0xb040000000000000ull;
      res.w[0] = 0x0000000000000001ull;
    } else {	// positive rpunds down to +0.0
      res.w[1] = 0x3040000000000000ull;
      res.w[0] = 0x0000000000000000ull;
    }
    *pfpsf |= BID_INEXACT_EXCEPTION;
    BID_RETURN (res);
  }
  break;
case BID_ROUNDING_UP:
  if ((q + exp) > 0) {	// exp < 0 and 1 <= -exp < q
    // need to shift right -exp digits from the coefficient; exp will be 0
    ind = -exp;	// 1 <= ind <= 34; ind is a synonym for 'x' 
    // (number of digits to be chopped off)
    // chop off ind digits from the lower part of C1 
    // FOR ROUND_TO_NEAREST, WE ADD 1/2 ULP(y) then truncate
    // FOR ROUND_TO_ZERO, WE DON'T NEED TO ADD 1/2 ULP
    // FOR ROUND_TO_POSITIVE_INFINITY, WE TRUNCATE, THEN ADD 1 IF POSITIVE
    // FOR ROUND_TO_NEGATIVE_INFINITY, WE TRUNCATE, THEN ADD 1 IF NEGATIVE
    // tmp64 = C1.w[0];
    // if (ind <= 19) {
    //   C1.w[0] = C1.w[0] + bid_midpoint64[ind - 1];
    // } else {
    //   C1.w[0] = C1.w[0] + bid_midpoint128[ind - 20].w[0];
    //   C1.w[1] = C1.w[1] + bid_midpoint128[ind - 20].w[1];
    // }
    // if (C1.w[0] < tmp64) C1.w[1]++;  
    // if carry-out from C1.w[0], increment C1.w[1]
    // calculate C* and f*
    // C* is actually floor(C*) in this case
    // C* and f* need shifting and masking, as shown by
    // bid_shiftright128[] and bid_maskhigh128[]
    // 1 <= x <= 34
    // kx = 10^(-x) = bid_ten2mk128[ind - 1]
    // C* = C1 * 10^(-x)
    // the approximation of 10^(-x) was rounded up to 118 bits
    __mul_128x128_to_256 (P256, C1, bid_ten2mk128[ind - 1]);
    if (ind - 1 <= 2) {	// 0 <= ind - 1 <= 2 => shift = 0
      res.w[1] = P256.w[3];
      res.w[0] = P256.w[2];
      // redundant fstar.w[3] = 0;
      // redundant fstar.w[2] = 0;
      // redundant fstar.w[1] = P256.w[1]; 
      // redundant fstar.w[0] = P256.w[0];
      // fraction f* > 10^(-x) <=> inexact
      // f* is in the right position to be compared with 
      // 10^(-x) from bid_ten2mk128[]
      if ((P256.w[1] > bid_ten2mk128[ind - 1].w[1])
	  || (P256.w[1] == bid_ten2mk128[ind - 1].w[1]
	      && (P256.w[0] >= bid_ten2mk128[ind - 1].w[0]))) {
	*pfpsf |= BID_INEXACT_EXCEPTION;
	// if negative, the truncated value is already the correct result
	if (!x_sign) {	// if positive
	  if (++res.w[0] == 0) {
	    res.w[1]++;
	  }
	}
      }
    } else if (ind - 1 <= 21) {	// 3 <= ind - 1 <= 21 => 3 <= shift <= 63
      shift = bid_shiftright128[ind - 1];	// 3 <= shift <= 63
      res.w[1] = (P256.w[3] >> shift);
      res.w[0] = (P256.w[3] << (64 - shift)) | (P256.w[2] >> shift);
      // redundant fstar.w[3] = 0;
      fstar.w[2] = P256.w[2] & bid_maskhigh128[ind - 1];
      fstar.w[1] = P256.w[1];
      fstar.w[0] = P256.w[0];
      // fraction f* > 10^(-x) <=> inexact
      // f* is in the right position to be compared with 
      // 10^(-x) from bid_ten2mk128[]
      if (fstar.w[2] || fstar.w[1] > bid_ten2mk128[ind - 1].w[1] ||
	  (fstar.w[1] == bid_ten2mk128[ind - 1].w[1] &&
	   fstar.w[0] >= bid_ten2mk128[ind - 1].w[0])) {
	*pfpsf |= BID_INEXACT_EXCEPTION;
	// if negative, the truncated value is already the correct result
	if (!x_sign) {	// if positive
	  if (++res.w[0] == 0) {
	    res.w[1]++;
	  }
	}
      }
    } else {	// 22 <= ind - 1 <= 33
      shift = bid_shiftright128[ind - 1] - 64;	// 2 <= shift <= 38
      res.w[1] = 0;
      res.w[0] = P256.w[3] >> shift;
      fstar.w[3] = P256.w[3] & bid_maskhigh128[ind - 1];
      fstar.w[2] = P256.w[2];
      fstar.w[1] = P256.w[1];
      fstar.w[0] = P256.w[0];
      // fraction f* > 10^(-x) <=> inexact
      // f* is in the right position to be compared with 
      // 10^(-x) from bid_ten2mk128[]
      if (fstar.w[3] || fstar.w[2]
	  || fstar.w[1] > bid_ten2mk128[ind - 1].w[1]
	  || (fstar.w[1] == bid_ten2mk128[ind - 1].w[1]
	      && fstar.w[0] >= bid_ten2mk128[ind - 1].w[0])) {
	*pfpsf |= BID_INEXACT_EXCEPTION;
	// if negative, the truncated value is already the correct result
	if (!x_sign) {	// if positive
	  if (++res.w[0] == 0) {
	    res.w[1]++;
	  }
	}
      }
    }
    res.w[1] = x_sign | 0x3040000000000000ull | res.w[1];
    BID_RETURN (res);
  } else {	// if exp < 0 and q + exp <= 0
    if (x_sign) {	// negative rounds up to -0.0
      res.w[1] = 0xb040000000000000ull;
      res.w[0] = 0x0000000000000000ull;
    } else {	// positive rpunds up to +1.0
      res.w[1] = 0x3040000000000000ull;
      res.w[0] = 0x0000000000000001ull;
    }
    *pfpsf |= BID_INEXACT_EXCEPTION;
    BID_RETURN (res);
  }
  break;
case BID_ROUNDING_TO_ZERO:
  if ((q + exp) > 0) {	// exp < 0 and 1 <= -exp < q
    // need to shift right -exp digits from the coefficient; exp will be 0
    ind = -exp;	// 1 <= ind <= 34; ind is a synonym for 'x'
    // (number of digits to be chopped off)
    // chop off ind digits from the lower part of C1 
    // FOR ROUND_TO_NEAREST, WE ADD 1/2 ULP(y) then truncate
    // FOR ROUND_TO_ZERO, WE DON'T NEED TO ADD 1/2 ULP
    // FOR ROUND_TO_POSITIVE_INFINITY, WE TRUNCATE, THEN ADD 1 IF POSITIVE
    // FOR ROUND_TO_NEGATIVE_INFINITY, WE TRUNCATE, THEN ADD 1 IF NEGATIVE
    //tmp64 = C1.w[0];
    // if (ind <= 19) {
    //   C1.w[0] = C1.w[0] + bid_midpoint64[ind - 1];
    // } else {
    //   C1.w[0] = C1.w[0] + bid_midpoint128[ind - 20].w[0];
    //   C1.w[1] = C1.w[1] + bid_midpoint128[ind - 20].w[1];
    // }
    // if (C1.w[0] < tmp64) C1.w[1]++;  
    // if carry-out from C1.w[0], increment C1.w[1]
    // calculate C* and f*
    // C* is actually floor(C*) in this case
    // C* and f* need shifting and masking, as shown by
    // bid_shiftright128[] and bid_maskhigh128[]
    // 1 <= x <= 34
    // kx = 10^(-x) = bid_ten2mk128[ind - 1]
    // C* = (C1 + 1/2 * 10^x) * 10^(-x)
    // the approximation of 10^(-x) was rounded up to 118 bits
    __mul_128x128_to_256 (P256, C1, bid_ten2mk128[ind - 1]);
    if (ind - 1 <= 2) {	// 0 <= ind - 1 <= 2 => shift = 0
      res.w[1] = P256.w[3];
      res.w[0] = P256.w[2];
      // redundant fstar.w[3] = 0;
      // redundant fstar.w[2] = 0;
      // redundant fstar.w[1] = P256.w[1]; 
      // redundant fstar.w[0] = P256.w[0];
      // fraction f* > 10^(-x) <=> inexact
      // f* is in the right position to be compared with 
      // 10^(-x) from bid_ten2mk128[]
      if ((P256.w[1] > bid_ten2mk128[ind - 1].w[1])
	  || (P256.w[1] == bid_ten2mk128[ind - 1].w[1]
	      && (P256.w[0] >= bid_ten2mk128[ind - 1].w[0]))) {
	*pfpsf |= BID_INEXACT_EXCEPTION;
      }
    } else if (ind - 1 <= 21) {	// 3 <= ind - 1 <= 21 => 3 <= shift <= 63
      shift = bid_shiftright128[ind - 1];	// 3 <= shift <= 63
      res.w[1] = (P256.w[3] >> shift);
      res.w[0] = (P256.w[3] << (64 - shift)) | (P256.w[2] >> shift);
      // redundant fstar.w[3] = 0;
      fstar.w[2] = P256.w[2] & bid_maskhigh128[ind - 1];
      fstar.w[1] = P256.w[1];
      fstar.w[0] = P256.w[0];
      // fraction f* > 10^(-x) <=> inexact
      // f* is in the right position to be compared with 
      // 10^(-x) from bid_ten2mk128[]
      if (fstar.w[2] || fstar.w[1] > bid_ten2mk128[ind - 1].w[1] ||
	  (fstar.w[1] == bid_ten2mk128[ind - 1].w[1] &&
	   fstar.w[0] >= bid_ten2mk128[ind - 1].w[0])) {
	*pfpsf |= BID_INEXACT_EXCEPTION;
      }
    } else {	// 22 <= ind - 1 <= 33
      shift = bid_shiftright128[ind - 1] - 64;	// 2 <= shift <= 38
      res.w[1] = 0;
      res.w[0] = P256.w[3] >> shift;
      fstar.w[3] = P256.w[3] & bid_maskhigh128[ind - 1];
      fstar.w[2] = P256.w[2];
      fstar.w[1] = P256.w[1];
      fstar.w[0] = P256.w[0];
      // fraction f* > 10^(-x) <=> inexact
      // f* is in the right position to be compared with 
      // 10^(-x) from bid_ten2mk128[]
      if (fstar.w[3] || fstar.w[2]
	  || fstar.w[1] > bid_ten2mk128[ind - 1].w[1]
	  || (fstar.w[1] == bid_ten2mk128[ind - 1].w[1]
	      && fstar.w[0] >= bid_ten2mk128[ind - 1].w[0])) {
	*pfpsf |= BID_INEXACT_EXCEPTION;
      }
    }
    res.w[1] = x_sign | 0x3040000000000000ull | res.w[1];
    BID_RETURN (res);
  } else {	// if exp < 0 and q + exp <= 0 the result is +0 or -0
    res.w[1] = x_sign | 0x3040000000000000ull;
    res.w[0] = 0x0000000000000000ull;
    *pfpsf |= BID_INEXACT_EXCEPTION;
    BID_RETURN (res);
  }
  break;
}

BID_RETURN (res);
}

/*****************************************************************************
 *  BID128_round_integral_nearest_even
 ****************************************************************************/

BID128_FUNCTION_ARG1_NORND (bid128_round_integral_nearest_even, x)

     BID_UINT128 res;
     BID_UINT64 x_sign;
     BID_UINT64 x_exp;
     int exp;			// unbiased exponent
  // Note: C1.w[1], C1.w[0] represent x_signif_hi, x_signif_lo (all are BID_UINT64)
     BID_UINT64 tmp64;
     BID_UI64DOUBLE tmp1;
     unsigned int x_nr_bits = 0;
     int q, ind, shift;
     BID_UINT128 C1;
  // BID_UINT128 res is C* at first - represents up to 34 decimal digits ~ 113 bits
     BID_UINT256 fstar;
     BID_UINT256 P256;

  // check for NaN or Infinity
if ((x.w[1] & MASK_SPECIAL) == MASK_SPECIAL) {
    // x is special
if ((x.w[1] & MASK_NAN) == MASK_NAN) {	// x is NAN
  // if x = NaN, then res = Q (x)
  // check first for non-canonical NaN payload
  if (((x.w[1] & 0x00003fffffffffffull) > 0x0000314dc6448d93ull) ||
      (((x.w[1] & 0x00003fffffffffffull) == 0x0000314dc6448d93ull) &&
       (x.w[0] > 0x38c15b09ffffffffull))) {
    x.w[1] = x.w[1] & 0xffffc00000000000ull;
    x.w[0] = 0x0ull;
  }
  if ((x.w[1] & MASK_SNAN) == MASK_SNAN) {	// x is SNAN
    // set invalid flag
    *pfpsf |= BID_INVALID_EXCEPTION;
    // return quiet (x)
    res.w[1] = x.w[1] & 0xfc003fffffffffffull;	// clear out also G[6]-G[16]
    res.w[0] = x.w[0];
  } else {	// x is QNaN
    // return x
    res.w[1] = x.w[1] & 0xfc003fffffffffffull;	// clear out G[6]-G[16]
    res.w[0] = x.w[0];
  }
  BID_RETURN (res)
} else {	// x is not a NaN, so it must be infinity
  if ((x.w[1] & MASK_SIGN) == 0x0ull) {	// x is +inf
    // return +inf
    res.w[1] = 0x7800000000000000ull;
    res.w[0] = 0x0000000000000000ull;
  } else {	// x is -inf 
    // return -inf
    res.w[1] = 0xf800000000000000ull;
    res.w[0] = 0x0000000000000000ull;
  }
  BID_RETURN (res);
}
}
  // unpack x
x_sign = x.w[1] & MASK_SIGN;	// 0 for positive, MASK_SIGN for negative
C1.w[1] = x.w[1] & MASK_COEFF;
C1.w[0] = x.w[0];

  // check for non-canonical values (treated as zero)
if ((x.w[1] & 0x6000000000000000ull) == 0x6000000000000000ull) {	// G0_G1=11
  // non-canonical
  x_exp = (x.w[1] << 2) & MASK_EXP;	// biased and shifted left 49 bits
  C1.w[1] = 0;	// significand high
  C1.w[0] = 0;	// significand low
} else {	// G0_G1 != 11
  x_exp = x.w[1] & MASK_EXP;	// biased and shifted left 49 bits
  if (C1.w[1] > 0x0001ed09bead87c0ull ||
      (C1.w[1] == 0x0001ed09bead87c0ull
       && C1.w[0] > 0x378d8e63ffffffffull)) {
    // x is non-canonical if coefficient is larger than 10^34 -1
    C1.w[1] = 0;
    C1.w[0] = 0;
  } else {	// canonical
    ;
  }
}

  // test for input equal to zero
if ((C1.w[1] == 0x0ull) && (C1.w[0] == 0x0ull)) {
  // x is 0
  // return 0 preserving the sign bit and the preferred exponent
  // of MAX(Q(x), 0)
  if (x_exp <= (0x1820ull << 49)) {
    res.w[1] = (x.w[1] & 0x8000000000000000ull) | 0x3040000000000000ull;
  } else {
    res.w[1] = x_sign | x_exp;
  }
  res.w[0] = 0x0000000000000000ull;
  BID_RETURN (res);
}
  // x is not special and is not zero

  // if (exp <= -(p+1)) return 0
if (x_exp <= 0x2ffa000000000000ull) {	// 0x2ffa000000000000ull == -35
  res.w[1] = x_sign | 0x3040000000000000ull;
  res.w[0] = 0x0000000000000000ull;
  BID_RETURN (res);
}
  // q = nr. of decimal digits in x
  //  determine first the nr. of bits in x
if (C1.w[1] == 0) {
  if (C1.w[0] >= 0x0020000000000000ull) {	// x >= 2^53
    // split the 64-bit value in two 32-bit halves to avoid rounding errors
    tmp1.d = (double) (C1.w[0] >> 32);	// exact conversion
    x_nr_bits = 33 + ((((unsigned int) (tmp1.ui64 >> 52)) & 0x7ff) - 0x3ff);
  } else {	// if x < 2^53
    tmp1.d = (double) C1.w[0];	// exact conversion
    x_nr_bits =
      1 + ((((unsigned int) (tmp1.ui64 >> 52)) & 0x7ff) - 0x3ff);
  }
} else {	// C1.w[1] != 0 => nr. bits = 64 + nr_bits (C1.w[1])
  tmp1.d = (double) C1.w[1];	// exact conversion
  x_nr_bits =
    65 + ((((unsigned int) (tmp1.ui64 >> 52)) & 0x7ff) - 0x3ff);
}

q = bid_nr_digits[x_nr_bits - 1].digits;
if (q == 0) {
  q = bid_nr_digits[x_nr_bits - 1].digits1;
  if (C1.w[1] > bid_nr_digits[x_nr_bits - 1].threshold_hi
      || (C1.w[1] == bid_nr_digits[x_nr_bits - 1].threshold_hi &&
	  C1.w[0] >= bid_nr_digits[x_nr_bits - 1].threshold_lo))
    q++;
}
exp = (x_exp >> 49) - 6176;
if (exp >= 0) {	// -exp <= 0
  // the argument is an integer already
  res.w[1] = x.w[1];
  res.w[0] = x.w[0];
  BID_RETURN (res);
} else if ((q + exp) >= 0) {	// exp < 0 and 1 <= -exp <= q
  // need to shift right -exp digits from the coefficient; the exp will be 0
  ind = -exp;	// 1 <= ind <= 34; ind is a synonym for 'x'
  // chop off ind digits from the lower part of C1 
  // C1 = C1 + 1/2 * 10^x where the result C1 fits in 127 bits
  tmp64 = C1.w[0];
  if (ind <= 19) {
    C1.w[0] = C1.w[0] + bid_midpoint64[ind - 1];
  } else {
    C1.w[0] = C1.w[0] + bid_midpoint128[ind - 20].w[0];
    C1.w[1] = C1.w[1] + bid_midpoint128[ind - 20].w[1];
  }
  if (C1.w[0] < tmp64)
    C1.w[1]++;
  // calculate C* and f*
  // C* is actually floor(C*) in this case
  // C* and f* need shifting and masking, as shown by
  // bid_shiftright128[] and bid_maskhigh128[]
  // 1 <= x <= 34
  // kx = 10^(-x) = bid_ten2mk128[ind - 1]
  // C* = (C1 + 1/2 * 10^x) * 10^(-x)
  // the approximation of 10^(-x) was rounded up to 118 bits
  __mul_128x128_to_256 (P256, C1, bid_ten2mk128[ind - 1]);
  // determine the value of res and fstar
  if (ind - 1 <= 2) {	// 0 <= ind - 1 <= 2 => shift = 0
    // redundant shift = bid_shiftright128[ind - 1]; // shift = 0
    res.w[1] = P256.w[3];
    res.w[0] = P256.w[2];
    // redundant fstar.w[3] = 0;
    // redundant fstar.w[2] = 0;
    // redundant fstar.w[1] = P256.w[1];
    // redundant fstar.w[0] = P256.w[0];
    // fraction f* < 10^(-x) <=> midpoint
    // f* is in the right position to be compared with
    // 10^(-x) from bid_ten2mk128[]
    // if 0 < fstar < 10^(-x), subtract 1 if odd (for rounding to even)
    if ((res.w[0] & 0x0000000000000001ull) &&	// is result odd, and from MP?
	((P256.w[1] < (bid_ten2mk128[ind - 1].w[1]))
	 || ((P256.w[1] == bid_ten2mk128[ind - 1].w[1])
	     && (P256.w[0] < bid_ten2mk128[ind - 1].w[0])))) {
      // subtract 1 to make even
      res.w[0]--;
    }
  } else if (ind - 1 <= 21) {	// 3 <= ind - 1 <= 21 => 3 <= shift <= 63
    shift = bid_shiftright128[ind - 1];	// 3 <= shift <= 63
    res.w[1] = (P256.w[3] >> shift);
    res.w[0] = (P256.w[3] << (64 - shift)) | (P256.w[2] >> shift);
    // redundant fstar.w[3] = 0;
    fstar.w[2] = P256.w[2] & bid_maskhigh128[ind - 1];
    fstar.w[1] = P256.w[1];
    fstar.w[0] = P256.w[0];
    // fraction f* < 10^(-x) <=> midpoint
    // f* is in the right position to be compared with
    // 10^(-x) from bid_ten2mk128[]
    if ((res.w[0] & 0x0000000000000001ull) &&	// is result odd, and from MP?
	fstar.w[2] == 0 && (fstar.w[1] < bid_ten2mk128[ind - 1].w[1] ||
			    (fstar.w[1] == bid_ten2mk128[ind - 1].w[1] &&
			     fstar.w[0] < bid_ten2mk128[ind - 1].w[0]))) {
      // subtract 1 to make even
      res.w[0]--;
    }
  } else {	// 22 <= ind - 1 <= 33
    shift = bid_shiftright128[ind - 1] - 64;	// 2 <= shift <= 38
    res.w[1] = 0;
    res.w[0] = P256.w[3] >> shift;
    fstar.w[3] = P256.w[3] & bid_maskhigh128[ind - 1];
    fstar.w[2] = P256.w[2];
    fstar.w[1] = P256.w[1];
    fstar.w[0] = P256.w[0];
    // fraction f* < 10^(-x) <=> midpoint
    // f* is in the right position to be compared with
    // 10^(-x) from bid_ten2mk128[]
    if ((res.w[0] & 0x0000000000000001ull) &&	// is result odd, and from MP?
	fstar.w[3] == 0 && fstar.w[2] == 0
	&& (fstar.w[1] < bid_ten2mk128[ind - 1].w[1]
	    || (fstar.w[1] == bid_ten2mk128[ind - 1].w[1]
		&& fstar.w[0] < bid_ten2mk128[ind - 1].w[0]))) {
      // subtract 1 to make even
      res.w[0]--;
    }
  }
  res.w[1] = x_sign | 0x3040000000000000ull | res.w[1];
  BID_RETURN (res);
} else {	// if ((q + exp) < 0) <=> q < -exp
  // the result is +0 or -0
  res.w[1] = x_sign | 0x3040000000000000ull;
  res.w[0] = 0x0000000000000000ull;
  BID_RETURN (res);
}
}

/*****************************************************************************
 *  BID128_round_integral_negative
 ****************************************************************************/

BID128_FUNCTION_ARG1_NORND (bid128_round_integral_negative, x)

     BID_UINT128 res;
     BID_UINT64 x_sign;
     BID_UINT64 x_exp;
     int exp;			// unbiased exponent
  // Note: C1.w[1], C1.w[0] represent x_signif_hi, x_signif_lo 
  // (all are BID_UINT64)
     BID_UI64DOUBLE tmp1;
     unsigned int x_nr_bits = 0;
     int q, ind, shift;
     BID_UINT128 C1;
  // BID_UINT128 res is C* at first - represents up to 34 decimal digits ~ 
  // 113 bits
     BID_UINT256 fstar;
     BID_UINT256 P256;

  // check for NaN or Infinity
if ((x.w[1] & MASK_SPECIAL) == MASK_SPECIAL) {
    // x is special
if ((x.w[1] & MASK_NAN) == MASK_NAN) {	// x is NAN
  // if x = NaN, then res = Q (x)
  // check first for non-canonical NaN payload
  if (((x.w[1] & 0x00003fffffffffffull) > 0x0000314dc6448d93ull) ||
      (((x.w[1] & 0x00003fffffffffffull) == 0x0000314dc6448d93ull) &&
       (x.w[0] > 0x38c15b09ffffffffull))) {
    x.w[1] = x.w[1] & 0xffffc00000000000ull;
    x.w[0] = 0x0ull;
  }
  if ((x.w[1] & MASK_SNAN) == MASK_SNAN) {	// x is SNAN
    // set invalid flag
    *pfpsf |= BID_INVALID_EXCEPTION;
    // return quiet (x)
    res.w[1] = x.w[1] & 0xfc003fffffffffffull;	// clear out also G[6]-G[16]
    res.w[0] = x.w[0];
  } else {	// x is QNaN
    // return x
    res.w[1] = x.w[1] & 0xfc003fffffffffffull;	// clear out G[6]-G[16]
    res.w[0] = x.w[0];
  }
  BID_RETURN (res)
} else {	// x is not a NaN, so it must be infinity
  if ((x.w[1] & MASK_SIGN) == 0x0ull) {	// x is +inf
    // return +inf
    res.w[1] = 0x7800000000000000ull;
    res.w[0] = 0x0000000000000000ull;
  } else {	// x is -inf 
    // return -inf
    res.w[1] = 0xf800000000000000ull;
    res.w[0] = 0x0000000000000000ull;
  }
  BID_RETURN (res);
}
}
  // unpack x
x_sign = x.w[1] & MASK_SIGN;	// 0 for positive, MASK_SIGN for negative
C1.w[1] = x.w[1] & MASK_COEFF;
C1.w[0] = x.w[0];

  // check for non-canonical values (treated as zero)
if ((x.w[1] & 0x6000000000000000ull) == 0x6000000000000000ull) {	// G0_G1=11
  // non-canonical
  x_exp = (x.w[1] << 2) & MASK_EXP;	// biased and shifted left 49 bits
  C1.w[1] = 0;	// significand high
  C1.w[0] = 0;	// significand low
} else {	// G0_G1 != 11
  x_exp = x.w[1] & MASK_EXP;	// biased and shifted left 49 bits
  if (C1.w[1] > 0x0001ed09bead87c0ull ||
      (C1.w[1] == 0x0001ed09bead87c0ull
       && C1.w[0] > 0x378d8e63ffffffffull)) {
    // x is non-canonical if coefficient is larger than 10^34 -1
    C1.w[1] = 0;
    C1.w[0] = 0;
  } else {	// canonical
    ;
  }
}

  // test for input equal to zero
if ((C1.w[1] == 0x0ull) && (C1.w[0] == 0x0ull)) {
  // x is 0
  // return 0 preserving the sign bit and the preferred exponent
  // of MAX(Q(x), 0)
  if (x_exp <= (0x1820ull << 49)) {
    res.w[1] = (x.w[1] & 0x8000000000000000ull) | 0x3040000000000000ull;
  } else {
    res.w[1] = x_sign | x_exp;
  }
  res.w[0] = 0x0000000000000000ull;
  BID_RETURN (res);
}
  // x is not special and is not zero

  // if (exp <= -p) return -1.0 or +0.0
if (x_exp <= 0x2ffc000000000000ull) {	// 0x2ffc000000000000ull == -34
  if (x_sign) {
    // if negative, return negative 1, because we know the coefficient
    // is non-zero (would have been caught above)
    res.w[1] = 0xb040000000000000ull;
    res.w[0] = 0x0000000000000001ull;
  } else {
    // if positive, return positive 0, because we know coefficient is
    // non-zero (would have been caught above)
    res.w[1] = 0x3040000000000000ull;
    res.w[0] = 0x0000000000000000ull;
  }
  BID_RETURN (res);
}
  // q = nr. of decimal digits in x
  // determine first the nr. of bits in x
if (C1.w[1] == 0) {
  if (C1.w[0] >= 0x0020000000000000ull) {	// x >= 2^53
    // split the 64-bit value in two 32-bit halves to avoid rounding errors
    tmp1.d = (double) (C1.w[0] >> 32);	// exact conversion
    x_nr_bits = 33 + ((((unsigned int) (tmp1.ui64 >> 52)) & 0x7ff) - 0x3ff);
  } else {	// if x < 2^53
    tmp1.d = (double) C1.w[0];	// exact conversion
    x_nr_bits =
      1 + ((((unsigned int) (tmp1.ui64 >> 52)) & 0x7ff) - 0x3ff);
  }
} else {	// C1.w[1] != 0 => nr. bits = 64 + nr_bits (C1.w[1])
  tmp1.d = (double) C1.w[1];	// exact conversion
  x_nr_bits =
    65 + ((((unsigned int) (tmp1.ui64 >> 52)) & 0x7ff) - 0x3ff);
}

q = bid_nr_digits[x_nr_bits - 1].digits;
if (q == 0) {
  q = bid_nr_digits[x_nr_bits - 1].digits1;
  if (C1.w[1] > bid_nr_digits[x_nr_bits - 1].threshold_hi ||
      (C1.w[1] == bid_nr_digits[x_nr_bits - 1].threshold_hi &&
       C1.w[0] >= bid_nr_digits[x_nr_bits - 1].threshold_lo))
    q++;
}
exp = (x_exp >> 49) - 6176;
if (exp >= 0) {	// -exp <= 0
  // the argument is an integer already
  res.w[1] = x.w[1];
  res.w[0] = x.w[0];
  BID_RETURN (res);
} else if ((q + exp) > 0) {	// exp < 0 and 1 <= -exp < q
  // need to shift right -exp digits from the coefficient; the exp will be 0
  ind = -exp;	// 1 <= ind <= 34; ind is a synonym for 'x' 
  // (number of digits to be chopped off)
  // chop off ind digits from the lower part of C1 
  // FOR ROUND_TO_NEAREST, WE ADD 1/2 ULP(y) then truncate
  // FOR ROUND_TO_ZERO, WE DON'T NEED TO ADD 1/2 ULP
  // FOR ROUND_TO_POSITIVE_INFINITY, WE TRUNCATE, THEN ADD 1 IF POSITIVE
  // FOR ROUND_TO_NEGATIVE_INFINITY, WE TRUNCATE, THEN ADD 1 IF NEGATIVE
  //tmp64 = C1.w[0];
  // if (ind <= 19) {
  //   C1.w[0] = C1.w[0] + bid_midpoint64[ind - 1];
  // } else {
  //   C1.w[0] = C1.w[0] + bid_midpoint128[ind - 20].w[0];
  //   C1.w[1] = C1.w[1] + bid_midpoint128[ind - 20].w[1];
  // }
  // if (C1.w[0] < tmp64) C1.w[1]++;
  // if carry-out from C1.w[0], increment C1.w[1]
  // calculate C* and f*
  // C* is actually floor(C*) in this case
  // C* and f* need shifting and masking, as shown by
  // bid_shiftright128[] and bid_maskhigh128[]
  // 1 <= x <= 34
  // kx = 10^(-x) = bid_ten2mk128[ind - 1]
  // C* = (C1 + 1/2 * 10^x) * 10^(-x)
  // the approximation of 10^(-x) was rounded up to 118 bits
  __mul_128x128_to_256 (P256, C1, bid_ten2mk128[ind - 1]);
  if (ind - 1 <= 2) {	// 0 <= ind - 1 <= 2 => shift = 0
    res.w[1] = P256.w[3];
    res.w[0] = P256.w[2];
    // if positive, the truncated value is already the correct result
    if (x_sign) {	// if negative
      // redundant fstar.w[3] = 0;
      // redundant fstar.w[2] = 0;
      // redundant fstar.w[1] = P256.w[1];
      // redundant fstar.w[0] = P256.w[0];
      // fraction f* > 10^(-x) <=> inexact
      // f* is in the right position to be compared with
      // 10^(-x) from bid_ten2mk128[]
      if ((P256.w[1] > bid_ten2mk128[ind - 1].w[1])
	  || (P256.w[1] == bid_ten2mk128[ind - 1].w[1]
	      && (P256.w[0] >= bid_ten2mk128[ind - 1].w[0]))) {
	if (++res.w[0] == 0) {
	  res.w[1]++;
	}
      }
    }
  } else if (ind - 1 <= 21) {	// 3 <= ind - 1 <= 21 => 3 <= shift <= 63
    shift = bid_shiftright128[ind - 1];	// 0 <= shift <= 102
    res.w[1] = (P256.w[3] >> shift);
    res.w[0] = (P256.w[3] << (64 - shift)) | (P256.w[2] >> shift);
    // if positive, the truncated value is already the correct result
    if (x_sign) {	// if negative
      // redundant fstar.w[3] = 0;
      fstar.w[2] = P256.w[2] & bid_maskhigh128[ind - 1];
      fstar.w[1] = P256.w[1];
      fstar.w[0] = P256.w[0];
      // fraction f* > 10^(-x) <=> inexact
      // f* is in the right position to be compared with
      // 10^(-x) from bid_ten2mk128[]
      if (fstar.w[2] || fstar.w[1] > bid_ten2mk128[ind - 1].w[1] ||
	  (fstar.w[1] == bid_ten2mk128[ind - 1].w[1] &&
	   fstar.w[0] >= bid_ten2mk128[ind - 1].w[0])) {
	if (++res.w[0] == 0) {
	  res.w[1]++;
	}
      }
    }
  } else {	// 22 <= ind - 1 <= 33
    shift = bid_shiftright128[ind - 1] - 64;	// 2 <= shift <= 38
    res.w[1] = 0;
    res.w[0] = P256.w[3] >> shift;
    // if positive, the truncated value is already the correct result
    if (x_sign) {	// if negative
      fstar.w[3] = P256.w[3] & bid_maskhigh128[ind - 1];
      fstar.w[2] = P256.w[2];
      fstar.w[1] = P256.w[1];
      fstar.w[0] = P256.w[0];
      // fraction f* > 10^(-x) <=> inexact
      // f* is in the right position to be compared with
      // 10^(-x) from bid_ten2mk128[]
      if (fstar.w[3] || fstar.w[2]
	  || fstar.w[1] > bid_ten2mk128[ind - 1].w[1]
	  || (fstar.w[1] == bid_ten2mk128[ind - 1].w[1]
	      && fstar.w[0] >= bid_ten2mk128[ind - 1].w[0])) {
	if (++res.w[0] == 0) {
	  res.w[1]++;
	}
      }
    }
  }
  res.w[1] = x_sign | 0x3040000000000000ull | res.w[1];
  BID_RETURN (res);
} else {	// if exp < 0 and q + exp <= 0
  if (x_sign) {	// negative rounds down to -1.0
    res.w[1] = 0xb040000000000000ull;
    res.w[0] = 0x0000000000000001ull;
  } else {	// positive rpunds down to +0.0
    res.w[1] = 0x3040000000000000ull;
    res.w[0] = 0x0000000000000000ull;
  }
  BID_RETURN (res);
}
}

/*****************************************************************************
 *  BID128_round_integral_positive
 ****************************************************************************/

BID128_FUNCTION_ARG1_NORND (bid128_round_integral_positive, x)

     BID_UINT128 res;
     BID_UINT64 x_sign;
     BID_UINT64 x_exp;
     int exp;			// unbiased exponent
  // Note: C1.w[1], C1.w[0] represent x_signif_hi, x_signif_lo 
  // (all are BID_UINT64)
     BID_UI64DOUBLE tmp1;
     unsigned int x_nr_bits = 0;
     int q, ind, shift;
     BID_UINT128 C1;
  // BID_UINT128 res is C* at first - represents up to 34 decimal digits ~ 
  // 113 bits
     BID_UINT256 fstar;
     BID_UINT256 P256;

  // check for NaN or Infinity
if ((x.w[1] & MASK_SPECIAL) == MASK_SPECIAL) {
    // x is special
if ((x.w[1] & MASK_NAN) == MASK_NAN) {	// x is NAN
  // if x = NaN, then res = Q (x)
  // check first for non-canonical NaN payload
  if (((x.w[1] & 0x00003fffffffffffull) > 0x0000314dc6448d93ull) ||
      (((x.w[1] & 0x00003fffffffffffull) == 0x0000314dc6448d93ull) &&
       (x.w[0] > 0x38c15b09ffffffffull))) {
    x.w[1] = x.w[1] & 0xffffc00000000000ull;
    x.w[0] = 0x0ull;
  }
  if ((x.w[1] & MASK_SNAN) == MASK_SNAN) {	// x is SNAN
    // set invalid flag
    *pfpsf |= BID_INVALID_EXCEPTION;
    // return quiet (x)
    res.w[1] = x.w[1] & 0xfc003fffffffffffull;	// clear out also G[6]-G[16]
    res.w[0] = x.w[0];
  } else {	// x is QNaN
    // return x
    res.w[1] = x.w[1] & 0xfc003fffffffffffull;	// clear out G[6]-G[16]
    res.w[0] = x.w[0];
  }
  BID_RETURN (res)
} else {	// x is not a NaN, so it must be infinity
  if ((x.w[1] & MASK_SIGN) == 0x0ull) {	// x is +inf
    // return +inf
    res.w[1] = 0x7800000000000000ull;
    res.w[0] = 0x0000000000000000ull;
  } else {	// x is -inf 
    // return -inf
    res.w[1] = 0xf800000000000000ull;
    res.w[0] = 0x0000000000000000ull;
  }
  BID_RETURN (res);
}
}
  // unpack x
x_sign = x.w[1] & MASK_SIGN;	// 0 for positive, MASK_SIGN for negative
C1.w[1] = x.w[1] & MASK_COEFF;
C1.w[0] = x.w[0];

  // check for non-canonical values (treated as zero)
if ((x.w[1] & 0x6000000000000000ull) == 0x6000000000000000ull) {	// G0_G1=11
  // non-canonical
  x_exp = (x.w[1] << 2) & MASK_EXP;	// biased and shifted left 49 bits
  C1.w[1] = 0;	// significand high
  C1.w[0] = 0;	// significand low
} else {	// G0_G1 != 11
  x_exp = x.w[1] & MASK_EXP;	// biased and shifted left 49 bits
  if (C1.w[1] > 0x0001ed09bead87c0ull ||
      (C1.w[1] == 0x0001ed09bead87c0ull
       && C1.w[0] > 0x378d8e63ffffffffull)) {
    // x is non-canonical if coefficient is larger than 10^34 -1
    C1.w[1] = 0;
    C1.w[0] = 0;
  } else {	// canonical
    ;
  }
}

  // test for input equal to zero
if ((C1.w[1] == 0x0ull) && (C1.w[0] == 0x0ull)) {
  // x is 0
  // return 0 preserving the sign bit and the preferred exponent 
  // of MAX(Q(x), 0)
  if (x_exp <= (0x1820ull << 49)) {
    res.w[1] = (x.w[1] & 0x8000000000000000ull) | 0x3040000000000000ull;
  } else {
    res.w[1] = x_sign | x_exp;
  }
  res.w[0] = 0x0000000000000000ull;
  BID_RETURN (res);
}
  // x is not special and is not zero

  // if (exp <= -p) return -0.0 or +1.0
if (x_exp <= 0x2ffc000000000000ull) {	// 0x2ffc000000000000ull == -34
  if (x_sign) {
    // if negative, return negative 0, because we know the coefficient 
    // is non-zero (would have been caught above)
    res.w[1] = 0xb040000000000000ull;
    res.w[0] = 0x0000000000000000ull;
  } else {
    // if positive, return positive 1, because we know coefficient is 
    // non-zero (would have been caught above)
    res.w[1] = 0x3040000000000000ull;
    res.w[0] = 0x0000000000000001ull;
  }
  BID_RETURN (res);
}
  // q = nr. of decimal digits in x
  // determine first the nr. of bits in x
if (C1.w[1] == 0) {
  if (C1.w[0] >= 0x0020000000000000ull) {	// x >= 2^53
    // split 64-bit value in two 32-bit halves to avoid rounding errors
    tmp1.d = (double) (C1.w[0] >> 32);	// exact conversion
    x_nr_bits = 33 + ((((unsigned int) (tmp1.ui64 >> 52)) & 0x7ff) - 0x3ff);
  } else {	// if x < 2^53
    tmp1.d = (double) C1.w[0];	// exact conversion
    x_nr_bits =
      1 + ((((unsigned int) (tmp1.ui64 >> 52)) & 0x7ff) - 0x3ff);
  }
} else {	// C1.w[1] != 0 => nr. bits = 64 + nr_bits (C1.w[1])
  tmp1.d = (double) C1.w[1];	// exact conversion
  x_nr_bits =
    65 + ((((unsigned int) (tmp1.ui64 >> 52)) & 0x7ff) - 0x3ff);
}

q = bid_nr_digits[x_nr_bits - 1].digits;
if (q == 0) {
  q = bid_nr_digits[x_nr_bits - 1].digits1;
  if (C1.w[1] > bid_nr_digits[x_nr_bits - 1].threshold_hi ||
      (C1.w[1] == bid_nr_digits[x_nr_bits - 1].threshold_hi &&
       C1.w[0] >= bid_nr_digits[x_nr_bits - 1].threshold_lo))
    q++;
}
exp = (x_exp >> 49) - 6176;
if (exp >= 0) {	// -exp <= 0
  // the argument is an integer already
  res.w[1] = x.w[1];
  res.w[0] = x.w[0];
  BID_RETURN (res);
} else if ((q + exp) > 0) {	// exp < 0 and 1 <= -exp < q
  // need to shift right -exp digits from the coefficient; exp will be 0
  ind = -exp;	// 1 <= ind <= 34; ind is a synonym for 'x' 
  // (number of digits to be chopped off)
  // chop off ind digits from the lower part of C1 
  // FOR ROUND_TO_NEAREST, WE ADD 1/2 ULP(y) then truncate
  // FOR ROUND_TO_ZERO, WE DON'T NEED TO ADD 1/2 ULP
  // FOR ROUND_TO_POSITIVE_INFINITY, WE TRUNCATE, THEN ADD 1 IF POSITIVE
  // FOR ROUND_TO_NEGATIVE_INFINITY, WE TRUNCATE, THEN ADD 1 IF NEGATIVE
  // tmp64 = C1.w[0];
  // if (ind <= 19) {
  //   C1.w[0] = C1.w[0] + bid_midpoint64[ind - 1];
  // } else {
  //   C1.w[0] = C1.w[0] + bid_midpoint128[ind - 20].w[0];
  //   C1.w[1] = C1.w[1] + bid_midpoint128[ind - 20].w[1];
  // }
  // if (C1.w[0] < tmp64) C1.w[1]++;  
  // if carry-out from C1.w[0], increment C1.w[1]
  // calculate C* and f*
  // C* is actually floor(C*) in this case
  // C* and f* need shifting and masking, as shown by
  // bid_shiftright128[] and bid_maskhigh128[]
  // 1 <= x <= 34
  // kx = 10^(-x) = bid_ten2mk128[ind - 1]
  // C* = C1 * 10^(-x)
  // the approximation of 10^(-x) was rounded up to 118 bits
  __mul_128x128_to_256 (P256, C1, bid_ten2mk128[ind - 1]);
  if (ind - 1 <= 2) {	// 0 <= ind - 1 <= 2 => shift = 0
    res.w[1] = P256.w[3];
    res.w[0] = P256.w[2];
    // if negative, the truncated value is already the correct result
    if (!x_sign) {	// if positive
      // redundant fstar.w[3] = 0;
      // redundant fstar.w[2] = 0;
      // redundant fstar.w[1] = P256.w[1]; 
      // redundant fstar.w[0] = P256.w[0];
      // fraction f* > 10^(-x) <=> inexact
      // f* is in the right position to be compared with 
      // 10^(-x) from bid_ten2mk128[]
      if ((P256.w[1] > bid_ten2mk128[ind - 1].w[1])
	  || (P256.w[1] == bid_ten2mk128[ind - 1].w[1]
	      && (P256.w[0] >= bid_ten2mk128[ind - 1].w[0]))) {
	if (++res.w[0] == 0) {
	  res.w[1]++;
	}
      }
    }
  } else if (ind - 1 <= 21) {	// 3 <= ind - 1 <= 21 => 3 <= shift <= 63
    shift = bid_shiftright128[ind - 1];	// 3 <= shift <= 63
    res.w[1] = (P256.w[3] >> shift);
    res.w[0] = (P256.w[3] << (64 - shift)) | (P256.w[2] >> shift);
    // if negative, the truncated value is already the correct result
    if (!x_sign) {	// if positive
      // redundant fstar.w[3] = 0;
      fstar.w[2] = P256.w[2] & bid_maskhigh128[ind - 1];
      fstar.w[1] = P256.w[1];
      fstar.w[0] = P256.w[0];
      // fraction f* > 10^(-x) <=> inexact
      // f* is in the right position to be compared with 
      // 10^(-x) from bid_ten2mk128[]
      if (fstar.w[2] || fstar.w[1] > bid_ten2mk128[ind - 1].w[1] ||
	  (fstar.w[1] == bid_ten2mk128[ind - 1].w[1] &&
	   fstar.w[0] >= bid_ten2mk128[ind - 1].w[0])) {
	if (++res.w[0] == 0) {
	  res.w[1]++;
	}
      }
    }
  } else {	// 22 <= ind - 1 <= 33
    shift = bid_shiftright128[ind - 1] - 64;	// 2 <= shift <= 38
    res.w[1] = 0;
    res.w[0] = P256.w[3] >> shift;
    // if negative, the truncated value is already the correct result
    if (!x_sign) {	// if positive
      fstar.w[3] = P256.w[3] & bid_maskhigh128[ind - 1];
      fstar.w[2] = P256.w[2];
      fstar.w[1] = P256.w[1];
      fstar.w[0] = P256.w[0];
      // fraction f* > 10^(-x) <=> inexact
      // f* is in the right position to be compared with 
      // 10^(-x) from bid_ten2mk128[]
      if (fstar.w[3] || fstar.w[2]
	  || fstar.w[1] > bid_ten2mk128[ind - 1].w[1]
	  || (fstar.w[1] == bid_ten2mk128[ind - 1].w[1]
	      && fstar.w[0] >= bid_ten2mk128[ind - 1].w[0])) {
	if (++res.w[0] == 0) {
	  res.w[1]++;
	}
      }
    }
  }
  res.w[1] = x_sign | 0x3040000000000000ull | res.w[1];
  BID_RETURN (res);
} else {	// if exp < 0 and q + exp <= 0
  if (x_sign) {	// negative rounds up to -0.0
    res.w[1] = 0xb040000000000000ull;
    res.w[0] = 0x0000000000000000ull;
  } else {	// positive rpunds up to +1.0
    res.w[1] = 0x3040000000000000ull;
    res.w[0] = 0x0000000000000001ull;
  }
  BID_RETURN (res);
}
}

/*****************************************************************************
 *  BID128_round_integral_zero
 ****************************************************************************/

BID128_FUNCTION_ARG1_NORND (bid128_round_integral_zero, x)

     BID_UINT128 res;
     BID_UINT64 x_sign;
     BID_UINT64 x_exp;
     int exp;			// unbiased exponent
  // Note: C1.w[1], C1.w[0] represent x_signif_hi, x_signif_lo
  // (all are BID_UINT64)
     BID_UI64DOUBLE tmp1;
     unsigned int x_nr_bits = 0;
     int q, ind, shift;
     BID_UINT128 C1;
  // BID_UINT128 res is C* at first - represents up to 34 decimal digits ~
  // 113 bits
     BID_UINT256 P256;

  // check for NaN or Infinity
if ((x.w[1] & MASK_SPECIAL) == MASK_SPECIAL) {
    // x is special
if ((x.w[1] & MASK_NAN) == MASK_NAN) {	// x is NAN
  // if x = NaN, then res = Q (x)
  // check first for non-canonical NaN payload
  if (((x.w[1] & 0x00003fffffffffffull) > 0x0000314dc6448d93ull) ||
      (((x.w[1] & 0x00003fffffffffffull) == 0x0000314dc6448d93ull) &&
       (x.w[0] > 0x38c15b09ffffffffull))) {
    x.w[1] = x.w[1] & 0xffffc00000000000ull;
    x.w[0] = 0x0ull;
  }
  if ((x.w[1] & MASK_SNAN) == MASK_SNAN) {	// x is SNAN
    // set invalid flag
    *pfpsf |= BID_INVALID_EXCEPTION;
    // return quiet (x)
    res.w[1] = x.w[1] & 0xfc003fffffffffffull;	// clear out also G[6]-G[16]
    res.w[0] = x.w[0];
  } else {	// x is QNaN
    // return x
    res.w[1] = x.w[1] & 0xfc003fffffffffffull;	// clear out G[6]-G[16]
    res.w[0] = x.w[0];
  }
  BID_RETURN (res)
} else {	// x is not a NaN, so it must be infinity
  if ((x.w[1] & MASK_SIGN) == 0x0ull) {	// x is +inf
    // return +inf
    res.w[1] = 0x7800000000000000ull;
    res.w[0] = 0x0000000000000000ull;
  } else {	// x is -inf 
    // return -inf
    res.w[1] = 0xf800000000000000ull;
    res.w[0] = 0x0000000000000000ull;
  }
  BID_RETURN (res);
}
}
  // unpack x
x_sign = x.w[1] & MASK_SIGN;	// 0 for positive, MASK_SIGN for negative
C1.w[1] = x.w[1] & MASK_COEFF;
C1.w[0] = x.w[0];

  // check for non-canonical values (treated as zero)
if ((x.w[1] & 0x6000000000000000ull) == 0x6000000000000000ull) {	// G0_G1=11
  // non-canonical
  x_exp = (x.w[1] << 2) & MASK_EXP;	// biased and shifted left 49 bits
  C1.w[1] = 0;	// significand high
  C1.w[0] = 0;	// significand low
} else {	// G0_G1 != 11
  x_exp = x.w[1] & MASK_EXP;	// biased and shifted left 49 bits
  if (C1.w[1] > 0x0001ed09bead87c0ull ||
      (C1.w[1] == 0x0001ed09bead87c0ull
       && C1.w[0] > 0x378d8e63ffffffffull)) {
    // x is non-canonical if coefficient is larger than 10^34 -1
    C1.w[1] = 0;
    C1.w[0] = 0;
  } else {	// canonical
    ;
  }
}

  // test for input equal to zero
if ((C1.w[1] == 0x0ull) && (C1.w[0] == 0x0ull)) {
  // x is 0
  // return 0 preserving the sign bit and the preferred exponent
  // of MAX(Q(x), 0)
  if (x_exp <= (0x1820ull << 49)) {
    res.w[1] = (x.w[1] & 0x8000000000000000ull) | 0x3040000000000000ull;
  } else {
    res.w[1] = x_sign | x_exp;
  }
  res.w[0] = 0x0000000000000000ull;
  BID_RETURN (res);
}
  // x is not special and is not zero

  // if (exp <= -p) return -0.0 or +0.0
if (x_exp <= 0x2ffc000000000000ull) {	// 0x2ffc000000000000ull == -34
  res.w[1] = x_sign | 0x3040000000000000ull;
  res.w[0] = 0x0000000000000000ull;
  BID_RETURN (res);
}
  // q = nr. of decimal digits in x
  // determine first the nr. of bits in x
if (C1.w[1] == 0) {
  if (C1.w[0] >= 0x0020000000000000ull) {	// x >= 2^53
    // split the 64-bit value in two 32-bit halves to avoid rounding errors
    tmp1.d = (double) (C1.w[0] >> 32);	// exact conversion
    x_nr_bits = 33 + ((((unsigned int) (tmp1.ui64 >> 52)) & 0x7ff) - 0x3ff);
  } else {	// if x < 2^53
    tmp1.d = (double) C1.w[0];	// exact conversion
    x_nr_bits =
      1 + ((((unsigned int) (tmp1.ui64 >> 52)) & 0x7ff) - 0x3ff);
  }
} else {	// C1.w[1] != 0 => nr. bits = 64 + nr_bits (C1.w[1])
  tmp1.d = (double) C1.w[1];	// exact conversion
  x_nr_bits =
    65 + ((((unsigned int) (tmp1.ui64 >> 52)) & 0x7ff) - 0x3ff);
}

q = bid_nr_digits[x_nr_bits - 1].digits;
if (q == 0) {
  q = bid_nr_digits[x_nr_bits - 1].digits1;
  if (C1.w[1] > bid_nr_digits[x_nr_bits - 1].threshold_hi ||
      (C1.w[1] == bid_nr_digits[x_nr_bits - 1].threshold_hi &&
       C1.w[0] >= bid_nr_digits[x_nr_bits - 1].threshold_lo))
    q++;
}
exp = (x_exp >> 49) - 6176;
if (exp >= 0) {	// -exp <= 0
  // the argument is an integer already
  res.w[1] = x.w[1];
  res.w[0] = x.w[0];
  BID_RETURN (res);
} else if ((q + exp) > 0) {	// exp < 0 and 1 <= -exp < q
  // need to shift right -exp digits from the coefficient; the exp will be 0
  ind = -exp;	// 1 <= ind <= 34; ind is a synonym for 'x'
  // (number of digits to be chopped off)
  // chop off ind digits from the lower part of C1 
  // FOR ROUND_TO_NEAREST, WE ADD 1/2 ULP(y) then truncate
  // FOR ROUND_TO_ZERO, WE DON'T NEED TO ADD 1/2 ULP
  // FOR ROUND_TO_POSITIVE_INFINITY, WE TRUNCATE, THEN ADD 1 IF POSITIVE
  // FOR ROUND_TO_NEGATIVE_INFINITY, WE TRUNCATE, THEN ADD 1 IF NEGATIVE
  //tmp64 = C1.w[0];
  // if (ind <= 19) {
  //   C1.w[0] = C1.w[0] + bid_midpoint64[ind - 1];
  // } else {
  //   C1.w[0] = C1.w[0] + bid_midpoint128[ind - 20].w[0];
  //   C1.w[1] = C1.w[1] + bid_midpoint128[ind - 20].w[1];
  // }
  // if (C1.w[0] < tmp64) C1.w[1]++;  
  // if carry-out from C1.w[0], increment C1.w[1]
  // calculate C* and f*
  // C* is actually floor(C*) in this case
  // C* and f* need shifting and masking, as shown by
  // bid_shiftright128[] and bid_maskhigh128[]
  // 1 <= x <= 34
  // kx = 10^(-x) = bid_ten2mk128[ind - 1]
  // C* = (C1 + 1/2 * 10^x) * 10^(-x)
  // the approximation of 10^(-x) was rounded up to 118 bits
  __mul_128x128_to_256 (P256, C1, bid_ten2mk128[ind - 1]);
  if (ind - 1 <= 2) {	// 0 <= ind - 1 <= 2 => shift = 0
    res.w[1] = P256.w[3];
    res.w[0] = P256.w[2];
  } else if (ind - 1 <= 21) {	// 3 <= ind - 1 <= 21 => 3 <= shift <= 63
    shift = bid_shiftright128[ind - 1];	// 3 <= shift <= 63
    res.w[1] = (P256.w[3] >> shift);
    res.w[0] = (P256.w[3] << (64 - shift)) | (P256.w[2] >> shift);
  } else {	// 22 <= ind - 1 <= 33
    shift = bid_shiftright128[ind - 1] - 64;	// 2 <= shift <= 38
    res.w[1] = 0;
    res.w[0] = P256.w[3] >> shift;
  }
  res.w[1] = x_sign | 0x3040000000000000ull | res.w[1];
  BID_RETURN (res);
} else {	// if exp < 0 and q + exp <= 0 the result is +0 or -0
  res.w[1] = x_sign | 0x3040000000000000ull;
  res.w[0] = 0x0000000000000000ull;
  BID_RETURN (res);
}
}

/*****************************************************************************
 *  BID128_round_integral_nearest_away
 ****************************************************************************/

BID128_FUNCTION_ARG1_NORND (bid128_round_integral_nearest_away, x)

     BID_UINT128 res;
     BID_UINT64 x_sign;
     BID_UINT64 x_exp;
     int exp;			// unbiased exponent
  // Note: C1.w[1], C1.w[0] represent x_signif_hi, x_signif_lo 
  // (all are BID_UINT64)
     BID_UINT64 tmp64;
     BID_UI64DOUBLE tmp1;
     unsigned int x_nr_bits = 0;
     int q, ind, shift;
     BID_UINT128 C1;
  // BID_UINT128 res is C* at first - represents up to 34 decimal digits ~ 
  // 113 bits
  // BID_UINT256 fstar;
     BID_UINT256 P256;

  // check for NaN or Infinity
if ((x.w[1] & MASK_SPECIAL) == MASK_SPECIAL) {
    // x is special
if ((x.w[1] & MASK_NAN) == MASK_NAN) {	// x is NAN
  // if x = NaN, then res = Q (x)
  // check first for non-canonical NaN payload
  if (((x.w[1] & 0x00003fffffffffffull) > 0x0000314dc6448d93ull) ||
      (((x.w[1] & 0x00003fffffffffffull) == 0x0000314dc6448d93ull) &&
       (x.w[0] > 0x38c15b09ffffffffull))) {
    x.w[1] = x.w[1] & 0xffffc00000000000ull;
    x.w[0] = 0x0ull;
  }
  if ((x.w[1] & MASK_SNAN) == MASK_SNAN) {	// x is SNAN
    // set invalid flag
    *pfpsf |= BID_INVALID_EXCEPTION;
    // return quiet (x)
    res.w[1] = x.w[1] & 0xfc003fffffffffffull;	// clear out also G[6]-G[16]
    res.w[0] = x.w[0];
  } else {	// x is QNaN
    // return x
    res.w[1] = x.w[1] & 0xfc003fffffffffffull;	// clear out G[6]-G[16]
    res.w[0] = x.w[0];
  }
  BID_RETURN (res)
} else {	// x is not a NaN, so it must be infinity
  if ((x.w[1] & MASK_SIGN) == 0x0ull) {	// x is +inf
    // return +inf
    res.w[1] = 0x7800000000000000ull;
    res.w[0] = 0x0000000000000000ull;
  } else {	// x is -inf 
    // return -inf
    res.w[1] = 0xf800000000000000ull;
    res.w[0] = 0x0000000000000000ull;
  }
  BID_RETURN (res);
}
}
  // unpack x
x_sign = x.w[1] & MASK_SIGN;	// 0 for positive, MASK_SIGN for negative
C1.w[1] = x.w[1] & MASK_COEFF;
C1.w[0] = x.w[0];

  // check for non-canonical values (treated as zero)
if ((x.w[1] & 0x6000000000000000ull) == 0x6000000000000000ull) {	// G0_G1=11
  // non-canonical
  x_exp = (x.w[1] << 2) & MASK_EXP;	// biased and shifted left 49 bits
  C1.w[1] = 0;	// significand high
  C1.w[0] = 0;	// significand low
} else {	// G0_G1 != 11
  x_exp = x.w[1] & MASK_EXP;	// biased and shifted left 49 bits
  if (C1.w[1] > 0x0001ed09bead87c0ull ||
      (C1.w[1] == 0x0001ed09bead87c0ull
       && C1.w[0] > 0x378d8e63ffffffffull)) {
    // x is non-canonical if coefficient is larger than 10^34 -1
    C1.w[1] = 0;
    C1.w[0] = 0;
  } else {	// canonical
    ;
  }
}

  // test for input equal to zero
if ((C1.w[1] == 0x0ull) && (C1.w[0] == 0x0ull)) {
  // x is 0
  // return 0 preserving the sign bit and the preferred exponent
  // of MAX(Q(x), 0)
  if (x_exp <= (0x1820ull << 49)) {
    res.w[1] = (x.w[1] & 0x8000000000000000ull) | 0x3040000000000000ull;
  } else {
    res.w[1] = x_sign | x_exp;
  }
  res.w[0] = 0x0000000000000000ull;
  BID_RETURN (res);
}
  // x is not special and is not zero

  // if (exp <= -(p+1)) return 0.0
if (x_exp <= 0x2ffa000000000000ull) {	// 0x2ffa000000000000ull == -35
  res.w[1] = x_sign | 0x3040000000000000ull;
  res.w[0] = 0x0000000000000000ull;
  BID_RETURN (res);
}
  // q = nr. of decimal digits in x
  //  determine first the nr. of bits in x
if (C1.w[1] == 0) {
  if (C1.w[0] >= 0x0020000000000000ull) {	// x >= 2^53
    // split the 64-bit value in two 32-bit halves to avoid rounding errors
    tmp1.d = (double) (C1.w[0] >> 32);	// exact conversion
    x_nr_bits = 33 + ((((unsigned int) (tmp1.ui64 >> 52)) & 0x7ff) - 0x3ff);
  } else {	// if x < 2^53
    tmp1.d = (double) C1.w[0];	// exact conversion
    x_nr_bits =
      1 + ((((unsigned int) (tmp1.ui64 >> 52)) & 0x7ff) - 0x3ff);
  }
} else {	// C1.w[1] != 0 => nr. bits = 64 + nr_bits (C1.w[1])
  tmp1.d = (double) C1.w[1];	// exact conversion
  x_nr_bits =
    65 + ((((unsigned int) (tmp1.ui64 >> 52)) & 0x7ff) - 0x3ff);
}

q = bid_nr_digits[x_nr_bits - 1].digits;
if (q == 0) {
  q = bid_nr_digits[x_nr_bits - 1].digits1;
  if (C1.w[1] > bid_nr_digits[x_nr_bits - 1].threshold_hi ||
      (C1.w[1] == bid_nr_digits[x_nr_bits - 1].threshold_hi &&
       C1.w[0] >= bid_nr_digits[x_nr_bits - 1].threshold_lo))
    q++;
}
exp = (x_exp >> 49) - 6176;
if (exp >= 0) {	// -exp <= 0
  // the argument is an integer already
  res.w[1] = x.w[1];
  res.w[0] = x.w[0];
  BID_RETURN (res);
} else if ((q + exp) >= 0) {	// exp < 0 and 1 <= -exp <= q
  // need to shift right -exp digits from the coefficient; the exp will be 0
  ind = -exp;	// 1 <= ind <= 34; ind is a synonym for 'x'
  // chop off ind digits from the lower part of C1 
  // C1 = C1 + 1/2 * 10^x where the result C1 fits in 127 bits
  tmp64 = C1.w[0];
  if (ind <= 19) {
    C1.w[0] = C1.w[0] + bid_midpoint64[ind - 1];
  } else {
    C1.w[0] = C1.w[0] + bid_midpoint128[ind - 20].w[0];
    C1.w[1] = C1.w[1] + bid_midpoint128[ind - 20].w[1];
  }
  if (C1.w[0] < tmp64)
    C1.w[1]++;
  // calculate C* and f*
  // C* is actually floor(C*) in this case
  // C* and f* need shifting and masking, as shown by
  // bid_shiftright128[] and bid_maskhigh128[]
  // 1 <= x <= 34
  // kx = 10^(-x) = bid_ten2mk128[ind - 1]
  // C* = (C1 + 1/2 * 10^x) * 10^(-x)
  // the approximation of 10^(-x) was rounded up to 118 bits
  __mul_128x128_to_256 (P256, C1, bid_ten2mk128[ind - 1]);
  // the top Ex bits of 10^(-x) are T* = bid_ten2mk128trunc[ind], e.g.
  // if x=1, T*=bid_ten2mk128trunc[0]=0x19999999999999999999999999999999
  // if (0 < f* < 10^(-x)) then the result is a midpoint
  //   if floor(C*) is even then C* = floor(C*) - logical right
  //       shift; C* has p decimal digits, correct by Prop. 1)
  //   else if floor(C*) is odd C* = floor(C*)-1 (logical right
  //       shift; C* has p decimal digits, correct by Pr. 1)
  // else
  //   C* = floor(C*) (logical right shift; C has p decimal digits,
  //       correct by Property 1)
  // n = C* * 10^(e+x)

  // shift right C* by Ex-128 = bid_shiftright128[ind]
  if (ind - 1 <= 2) {	// 0 <= ind - 1 <= 2 => shift = 0
    res.w[1] = P256.w[3];
    res.w[0] = P256.w[2];
  } else if (ind - 1 <= 21) {	// 3 <= ind - 1 <= 21 => 3 <= shift <= 63
    shift = bid_shiftright128[ind - 1];	// 3 <= shift <= 63
    res.w[0] = (P256.w[3] << (64 - shift)) | (P256.w[2] >> shift);
    res.w[1] = (P256.w[3] >> shift);
  } else {	// 22 <= ind - 1 <= 33
    shift = bid_shiftright128[ind - 1];	// 2 <= shift <= 38
    res.w[1] = 0;
    res.w[0] = (P256.w[3] >> (shift - 64));	// 2 <= shift - 64 <= 38
  }
  // if the result was a midpoint, it was already rounded away from zero
  res.w[1] |= x_sign | 0x3040000000000000ull;
  BID_RETURN (res);
} else {	// if ((q + exp) < 0) <=> q < -exp
  // the result is +0 or -0
  res.w[1] = x_sign | 0x3040000000000000ull;
  res.w[0] = 0x0000000000000000ull;
  BID_RETURN (res);
}
}
