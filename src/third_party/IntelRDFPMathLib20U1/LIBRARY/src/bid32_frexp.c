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

/* 
  If x is not a floating-point number, the results are unspecified (this
  implementation returns x and *exp = 0). Otherwise, the frexp function 
  returns the value res, such that res has a magnitude in the interval 
  [1/10, 1) or zero, and x = res*2^*exp. If x is zero, both parts of the 
  result are zero
  frexp does not raise any exceptions
 */

#if DECIMAL_CALL_BY_REFERENCE
void bid32_frexp (BID_UINT32 *pres, BID_UINT32 *px, int *exp) { 
  BID_UINT32 x = *px;
#else
DFP_WRAPFN_DFP_OTHERTYPE(32, bid32_frexp, 32, int*)                 
BID_UINT32 bid32_frexp (BID_UINT32 x, int *exp) {
#endif

  BID_UINT32 res;
  BID_UINT32 sig_x; 
  unsigned int exp_x;
  BID_UI32FLOAT tmp;
  int x_nr_bits, q;

  if ((x & MASK_INF32) == MASK_INF32) {
    // if NaN or infinity
    *exp = 0;
    res = x;
    // the binary frexp quitetizes SNaNs, so do the same
    if ((x & MASK_SNAN32) == MASK_SNAN32) { // x is SNAN 
    //   // set invalid flag
    //   *pfpsf |= BID_INVALID_EXCEPTION;
      // return quiet (x)
      res = x & 0xfdffffff;
    // } else {
    //   res = x;
    }
    BID_RETURN (res); 
  } else {
    // x is 0, non-canonical, normal, or subnormal
    // decode number into exponent and significand
    if ((x & MASK_STEERING_BITS32) == MASK_STEERING_BITS32) {
      exp_x = (x & MASK_BINARY_EXPONENT2_32) >> 21;
      sig_x = (x & MASK_BINARY_SIG2_32) | MASK_BINARY_OR2_32;
      // check for zero or non-canonical 
      if (sig_x > 9999999 || sig_x == 0) {
        *exp = 0;
        res = (x & 0x80000000) | (exp_x << 23); // zero of the same sign
        BID_RETURN (res); 
      }  
    } else { 
      exp_x = (x & MASK_BINARY_EXPONENT1_32) >> 23;
      sig_x = (x & MASK_BINARY_SIG1_32);
      if (sig_x == 0) { 
        *exp = 0;
        res = (x & 0x80000000) | (exp_x << 23); // zero of the same sign
        BID_RETURN (res);
      }  
    }
    // x is normal or subnormal, with exp_x=biased exponent & sig_x=coefficient
    // determine the number of decimal digits in sig_x, which fits in 24 bits
    // q = nr. of decimal digits in sig_x (1 <= q <= 7) 
    //  determine first the nr. of bits in sig_x
    tmp.f = (float) sig_x; // exact conversion
    x_nr_bits = 
      1 + ((((unsigned int) (tmp.ui32 >> 23)) & 0xff) - 0x7f);
    q = bid_nr_digits[x_nr_bits - 1].digits;
    if (q == 0) {
      q = bid_nr_digits[x_nr_bits - 1].digits1; 
      if ((BID_UINT64)sig_x >= bid_nr_digits[x_nr_bits - 1].threshold_lo)
        q++; 
    }
    // Do not add trailing zeros if q < 7; leave sig_x with q digits
    // sig_x = sig_x * bid_mult_factor[7 - q]; // sig_x has now 7 digits
    *exp = exp_x - 101 + q;
    // assemble the result
    if (sig_x < 0x00800000) { // sig_x < 2^23 (fits in 23 bits)
      // res = (x & 0x80000000) | ((-q + 101) << 23) | sig_x; 
      res = (x & 0x807fffff) | ((-q + 101) << 23); // replace exponent
    } else { // sig_x fits in 24 bits, but not in 23
      // res = (x & 0x80000000) | 0x60000000 | 
      //     ((-q + 101) << 21) | (sig_x & 0x001fffff); 
      res = (x & 0xe01fffff) | ((-q + 101) << 21); // replace exponent
    }
    BID_RETURN (res);
  }
}
