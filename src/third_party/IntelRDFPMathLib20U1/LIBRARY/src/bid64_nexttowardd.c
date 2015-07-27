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
 *  BID64 nexttowardd
 ****************************************************************************/

BID_TYPE0_FUNCTION_ARGTYPE1_ARGTYPE2_NORND(BID_UINT64, bid64_nexttoward, BID_UINT64, x, BID_UINT128, y)

  BID_UINT64 res;
  BID_UINT128 x128, tmp128;
  BID_UINT64 tmp1, tmp2;
  BID_FPSC tmp_fpsf = 0; // dummy fpsf for calls to comparison functions
  int res1 = 0, res2 = 0;
#if !DECIMAL_GLOBAL_ROUNDING
  unsigned int rnd_mode = BID_ROUNDING_TO_NEAREST; 
      // dummy; used to convert 128-bit NaN result to 64-bit
#endif 

  // check for NaNs or infinities
  if (((x & MASK_SPECIAL) == MASK_SPECIAL) ||
      ((y.w[BID_HIGH_128W] & MASK_NAN) == MASK_NAN) || 
      ((y.w[BID_HIGH_128W] & MASK_ANY_INF) == MASK_INF) ) {
    // x is NaN or infinity or y is NaN or infinity
    if ((x & MASK_NAN) == MASK_NAN) {   // x is NAN
      if ((x & 0x0003ffffffffffffull) > 999999999999999ull)
        x = x & 0xfe00000000000000ull;  // clear G6-G12 and the payload bits
      else
        x = x & 0xfe03ffffffffffffull;  // clear G6-G12
      if ((x & MASK_SNAN) == MASK_SNAN) {       // x is SNAN
        // set invalid flag
        *pfpsf |= BID_INVALID_EXCEPTION;
        // return quiet (x)
        res = x & 0xfdffffffffffffffull;
      } else {  // x is QNaN
        if ((y.w[BID_HIGH_128W] & MASK_SNAN) == MASK_SNAN) {     // y is SNAN
          // set invalid flag
          *pfpsf |= BID_INVALID_EXCEPTION;
        }
        // return x
        res = x;
      }
      BID_RETURN (res);
    } else if ((y.w[BID_HIGH_128W] & MASK_NAN) == MASK_NAN) { // y is NAN then res = Q (y)
      // check first for non-canonical NaN payload 
      if (((y.w[BID_HIGH_128W] & 0x00003fffffffffffull) > 0x0000314dc6448d93ull) ||
          (((y.w[BID_HIGH_128W] & 0x00003fffffffffffull) == 0x0000314dc6448d93ull) &&
           (y.w[BID_LOW_128W] > 0x38c15b09ffffffffull))) {
        y.w[BID_HIGH_128W] = y.w[BID_HIGH_128W] & 0xffffc00000000000ull;
        y.w[BID_LOW_128W] = 0x0ull; 
      }  
      if ((y.w[BID_HIGH_128W] & MASK_SNAN) == MASK_SNAN) { // y is SNAN
        // set invalid flag 
        *pfpsf |= BID_INVALID_EXCEPTION; 
        // return quiet (y)
        tmp128.w[BID_HIGH_128W] = y.w[BID_HIGH_128W] & 0xfc003fffffffffffull; 
            // clear out also G[6]-G[16]
        tmp128.w[BID_LOW_128W] = y.w[BID_LOW_128W];
      } else { // y is QNaN  
        // return y
        tmp128.w[BID_HIGH_128W] = y.w[BID_HIGH_128W] & 0xfc003fffffffffffull; // clear out G[6]-G[16]
        tmp128.w[BID_LOW_128W] = y.w[BID_LOW_128W];
      }
      BIDECIMAL_CALL1 (bid128_to_bid64, res, tmp128);
      BID_RETURN (res);
    } else {	// at least one is infinity
      if ((x & MASK_ANY_INF) == MASK_INF) { // x = inf
	x = x & (MASK_SIGN | MASK_INF);
      }
      if ((y.w[BID_HIGH_128W] & MASK_ANY_INF) == MASK_INF) { // y = inf
	y.w[BID_HIGH_128W] = y.w[BID_HIGH_128W] & (MASK_SIGN | MASK_INF);
        y.w[BID_LOW_128W] = 0x0ull;
      }
    }
  }
  // neither x nor y is NaN

  // if not infinity, check for non-canonical values x (treated as zero)
  if ((x & MASK_ANY_INF) != MASK_INF) { // x != inf
    // unpack x
    if ((x & MASK_STEERING_BITS) == MASK_STEERING_BITS) {
      // if the steering bits are 11 (condition will be 0), then
      // the exponent is G[0:w+1]
      if (((x & MASK_BINARY_SIG2) | MASK_BINARY_OR2) >
          9999999999999999ull) {
        // non-canonical
        x = (x & MASK_SIGN) | ((x & MASK_BINARY_EXPONENT2) << 2);
      }
    } else { // if ((x & MASK_STEERING_BITS) != MASK_STEERING_BITS) x is unch.
      ; // canonical
    }
  }
  // no need to check for non-canonical y

  // neither x nor y is NaN
  tmp_fpsf = *pfpsf; // save fpsf
  // convert x to 128-bit format
  BIDECIMAL_CALL1_NORND (bid64_to_bid128, x128, x);
  BIDECIMAL_CALL2_NORND (bid128_quiet_equal, res1, x128, y);
  BIDECIMAL_CALL2_NORND (bid128_quiet_greater, res2, x128, y);
  *pfpsf = tmp_fpsf; // restore fpsf
  if (res1) { // x = y
    // return x with the sign of y
    res = (y.w[BID_HIGH_128W] & MASK_SIGN) | (x & 0x7fffffffffffffffull);
  } else if (res2) { // x > y
    BIDECIMAL_CALL1_NORND (bid64_nextdown, res, x);
  } else {	// x < y
    BIDECIMAL_CALL1_NORND (bid64_nextup, res, x);
  }
  // if the operand x is finite but the result is infinite, signal
  // overflow and inexact
  if (((x & MASK_INF) != MASK_INF) && ((res & MASK_INF) == MASK_INF)) {
    // set the inexact flag
    *pfpsf |= BID_INEXACT_EXCEPTION;
    // set the overflow flag
    *pfpsf |= BID_OVERFLOW_EXCEPTION;
  }
  // if the result is in (-10^emin, 10^emin), and is different from the 
  // operand x, signal underflow and inexact  
  tmp1 = 0x00038d7ea4c68000ull; // +100...0[16] * 10^emin 
  tmp2 = res & 0x7fffffffffffffffull; 
  tmp_fpsf = *pfpsf;    // save fpsf 
  BIDECIMAL_CALL2_NORND (bid64_quiet_greater, res1, tmp1, tmp2);
  BIDECIMAL_CALL2_NORND (bid64_quiet_not_equal, res2, x, res);
  *pfpsf = tmp_fpsf; // restore fpsf
  if (res1 && res2) {
    // if (bid64_quiet_greater (tmp1, tmp2, &tmp_fpsf) &&
    // bid64_quiet_not_equal (x, res, &tmp_fpsf)) {
    // set the inexact flag
    *pfpsf |= BID_INEXACT_EXCEPTION;
    // set the underflow flag
    *pfpsf |= BID_UNDERFLOW_EXCEPTION;
  }
  BID_RETURN (res);
}
