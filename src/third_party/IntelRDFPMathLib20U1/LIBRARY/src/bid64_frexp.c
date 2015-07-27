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

#if DECIMAL_CALL_BY_REFERENCE
void bid64_frexp (BID_UINT64 *pres, BID_UINT64 *px, int *exp) {
  BID_UINT64 x = *px;
#else
DFP_WRAPFN_DFP_OTHERTYPE(64, bid64_frexp, 64, int*)                 
BID_UINT64 bid64_frexp (BID_UINT64 x, int *exp) {
#endif

/* 
  If x is not a floating-point number, the results are unspecified (this
  implementation returns x and *exp = 0). Otherwise, the frexp function 
  returns the value res, such that res has a magnitude in the interval 
  [1/10, 1) or zero, and x = res*2^*exp. If x is zero, both parts of the 
  result are zero
  frexp does not raise any exceptions
 */

  BID_UINT64 res;
  BID_UINT64 sig_x; 
  unsigned int exp_x;
  BID_UI64DOUBLE tmp;
  int x_nr_bits, q;

  if ((x & MASK_NAN) == MASK_NAN || (x & MASK_INF) == MASK_INF) {
    // if NaN or infinity
    *exp = 0;
    res = x;
    // the binary frexp quitetizes SNaNs, so do the same
    if ((x & MASK_SNAN) == MASK_SNAN) { // x is SNAN 
    //   // set invalid flag
    //   *pfpsf |= BID_INVALID_EXCEPTION;
      // return quiet (x)
      res = x & 0xfdffffffffffffffull;
    }
    BID_RETURN (res); 
  } else {
    // x is 0, non-canonical, normal, or subnormal
    // unpack x
    // if steering bits are 11 (condition will be 0), then exponent is G[0:w+1]
    if ((x & MASK_STEERING_BITS) == MASK_STEERING_BITS) {
      sig_x = (x & MASK_BINARY_SIG2) | MASK_BINARY_OR2;
      exp_x = (x & MASK_BINARY_EXPONENT2) >> 51;  // biased
      if (sig_x > 9999999999999999ull || sig_x == 0) { // non-canonical or zero
        *exp = 0;
        res = (x & 0x8000000000000000ull) | ((BID_UINT64)exp_x << 53); // zero of same sign
        BID_RETURN (res);
      }
    } else {
      sig_x = x & MASK_BINARY_SIG1;
      exp_x = (x & MASK_BINARY_EXPONENT1) >> 53;  // biased
      if (sig_x == 0x0ull) {
        *exp = 0;
        res = x; // same zero
        BID_RETURN (res);
      }
    }
    // x is normal or subnormal, with exp_x=biased exponent & sig_x=coefficient
    // determine the number of decimal digits in sig_x, which fits in 54 bits
    // q = nr. of decimal digits in sig_x (1 <= q <= 16) 
    //  determine first the nr. of bits in sig_x
    //  determine first the nr. of bits in x 
    if (sig_x >= 0x0020000000000000ull) { // x >= 2^53
      q = 16;  
    } else { // if x < 2^53
      tmp.d = (double) sig_x; // exact conversion
      x_nr_bits = 1 + ((((unsigned int) (tmp.ui64 >> 52)) & 0x7ff) - 0x3ff);
      q = bid_nr_digits[x_nr_bits - 1].digits; 
      if (q == 0) { 
        q = bid_nr_digits[x_nr_bits - 1].digits1;
        if (sig_x >= bid_nr_digits[x_nr_bits - 1].threshold_lo)
          q++;  
      }    
    }  
    // Do not add trailing zeros if q < 16; leave sig_x with q digits
    *exp = exp_x - 398 + q;
    // assemble the result
    if (sig_x < 0x0020000000000000ull) { // sig_x < 2^53 (fits in 53 bits)
      res = (x & 0x801fffffffffffffull) | ((-q + 398ull) << 53); // replace exp.
    } else { // sig_x fits in 54 bits, but not in 53
      res = (x & 0xe007ffffffffffffull) | ((-q + 398ull) << 51); // replace exp.
    }
    BID_RETURN (res);
  }
}

