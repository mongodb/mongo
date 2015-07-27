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

#define BID_128RES
#include "bid_internal.h"


BID128_FUNCTION_ARG128_CUSTOMARGTYPE2_PLAIN(bid128_frexp, x, int*, exp)
/* 
  If x is not a floating-point number, the results are unspecified (this
  implementation returns x and *exp = 0). Otherwise, the frexp function 
  returns the value res, such that res has a magnitude in the interval 
  [1/10, 1) or zero, and x = res*2^*exp. If x is zero, both parts of the 
  result are zero
  frexp does not raise any exceptions
 */

  BID_UINT128 res;
  BID_UINT128 sig_x; 
  unsigned int exp_x;
  BID_UI64DOUBLE tmp;
  int x_nr_bits, q;

  if ((x.w[1] & MASK_SPECIAL) == MASK_SPECIAL) {
    // if NaN or infinity
    *exp = 0;
    res = x;
    // the binary frexp quitetizes SNaNs, so do the same
    if ((x.w[1] & MASK_SNAN) == MASK_SNAN) { // x is SNAN 
    //   // set invalid flag
    //   *pfpsf |= BID_INVALID_EXCEPTION;
      // return quiet (x)
      res.w[1] = x.w[1] & 0xfdffffffffffffffull;
    }
    BID_RETURN (res); 
  } else {
    // x is 0, non-canonical, normal, or subnormal
    // check for non-canonical values with 114 bit-significands; can be zero too
    if ((x.w[1] & 0x6000000000000000ull) == 0x6000000000000000ull) {
      *exp = 0;
      exp_x = (x.w[1] & MASK_EXP2) >> 47; // biased
      res.w[1] = (x.w[1] & 0x8000000000000000ull) | ((BID_UINT64)exp_x << 49); 
          // zero of same sign
      res.w[0] = 0x0000000000000000ull;
      BID_RETURN (res);
    }
    // unpack x
    exp_x = (x.w[1] & MASK_EXP) >> 49; // biased
    sig_x.w[1] = x.w[1] & MASK_COEFF;   
    sig_x.w[0] = x.w[0]; 
    // check for non-canonical values or zero
    if ((sig_x.w[1] > 0x0001ed09bead87c0ull)
        || (sig_x.w[1] == 0x0001ed09bead87c0ull
            && (sig_x.w[0] > 0x378d8e63ffffffffull)) ||
          ((sig_x.w[1] == 0x0ull) && (sig_x.w[0] == 0x0ull))) {
      *exp = 0;
      res.w[1] = (x.w[1] & 0x8000000000000000ull) | ((BID_UINT64)exp_x << 49); 
          // zero of same sign
      res.w[0] = 0x0000000000000000ull;
      BID_RETURN (res);
    } else {
      ; // continue, x is neither zero nor non-canonical 
    }
    // x is normal or subnormal, with exp_x=biased exponent & sig_x=coefficient
    // determine the number of decimal digits in sig_x, which fits in 113 bits
    // q = nr. of decimal digits in sig_x (1 <= q <= 34) 
    //  determine first the nr. of bits in sig_x
    if (sig_x.w[1] == 0) {
      if (sig_x.w[0] >= 0x0020000000000000ull) { // z >= 2^53
        // split the 64-bit value in two 32-bit halves to avoid rounding errors
        if (sig_x.w[0] >= 0x0000000100000000ull) { // z >= 2^32
          tmp.d = (double) (sig_x.w[0] >> 32); // exact conversion
          x_nr_bits =
            32 + ((((unsigned int) (tmp.ui64 >> 52)) & 0x7ff) - 0x3ff);
        } else { // z < 2^32
          tmp.d = (double) sig_x.w[0]; // exact conversion
          x_nr_bits =
            ((((unsigned int) (tmp.ui64 >> 52)) & 0x7ff) - 0x3ff);
        }
      } else { // if z < 2^53
        tmp.d = (double) sig_x.w[0]; // exact conversion
        x_nr_bits = ((((unsigned int) (tmp.ui64 >> 52)) & 0x7ff) - 0x3ff);
      }
    } else { // sig_x.w[1] != 0 => nr. bits = 65 + nr_bits (sig_x.w[1])
      tmp.d = (double) sig_x.w[1]; // exact conversion
      x_nr_bits = 64 + ((((unsigned int) (tmp.ui64 >> 52)) & 0x7ff) - 0x3ff);
    }
    q = bid_nr_digits[x_nr_bits].digits;
    if (q == 0) {
      q = bid_nr_digits[x_nr_bits].digits1;
      if (sig_x.w[1] > bid_nr_digits[x_nr_bits].threshold_hi ||
          (sig_x.w[1] == bid_nr_digits[x_nr_bits].threshold_hi &&
           sig_x.w[0] >= bid_nr_digits[x_nr_bits].threshold_lo))
        q++;
    }
    // Do not add trailing zeros if q < 34; leave sig_x with q digits
    *exp = exp_x - 6176 + q;
    // assemble the result; sig_x < 2^113 so it fits in 113 bits
    res.w[1] = (x.w[1] & 0x8001ffffffffffffull) | ((-q + 6176ull) << 49); 
    res.w[0] = x.w[0]; 
      // replace exponent
    BID_RETURN (res);
  }
}
