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

BID_TYPE_FUNCTION_ARG2(BID_UINT32, bid32_mul, x, y)
  
  BID_UINT128 Tmp;
  BID_UINT64 P, Q, R;
  BID_UINT32 sign_x, sign_y, coefficient_x, coefficient_y, res;
  BID_UINT32 valid_x, valid_y;
  int exponent_x, exponent_y, bin_expon_p, amount, n_digits, extra_digits, status, rmode;
  int_double tempx;

  valid_x = unpack_BID32 (&sign_x, &exponent_x, &coefficient_x, x);
  valid_y = unpack_BID32 (&sign_y, &exponent_y, &coefficient_y, y);

  // unpack arguments, check for NaN or Infinity
  if (!valid_x) {

#ifdef BID_SET_STATUS_FLAGS
    if ((y & SNAN_MASK32) == SNAN_MASK32)	// y is sNaN
      __set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
#endif
    // x is Inf. or NaN

    // test if x is NaN
    if ((x & NAN_MASK32) == NAN_MASK32) {
#ifdef BID_SET_STATUS_FLAGS
      if ((x & SNAN_MASK32) == SNAN_MASK32)	// sNaN
	__set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
#endif
      BID_RETURN (coefficient_x & QUIET_MASK32);
    }
    // x is Infinity?
    if ((x & INFINITY_MASK32) == INFINITY_MASK32) {
      // check if y is 0
      if (((y & INFINITY_MASK32) != INFINITY_MASK32)
	  && !coefficient_y) {
#ifdef BID_SET_STATUS_FLAGS
	__set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
#endif
	// y==0 , return NaN
	BID_RETURN (NAN_MASK32);
      }
      // check if y is NaN
      if ((y & NAN_MASK32) == NAN_MASK32)
	// y==NaN , return NaN
	BID_RETURN (coefficient_y & QUIET_MASK32);
      // otherwise return +/-Inf
      BID_RETURN (((x ^ y) & 0x80000000) | INFINITY_MASK32);
    }
    // x is 0
    if (((y & INFINITY_MASK32) != INFINITY_MASK32)) {
      if ((y & SPECIAL_ENCODING_MASK32) == SPECIAL_ENCODING_MASK32)
	exponent_y = ((BID_UINT32) (y >> 21)) & 0xff;
      else
	exponent_y = ((BID_UINT32) (y >> 23)) & 0xff;
      sign_y = y & 0x80000000;

      exponent_x += exponent_y - DECIMAL_EXPONENT_BIAS_32;
      if (exponent_x > DECIMAL_MAX_EXPON_32)
	exponent_x = DECIMAL_MAX_EXPON_32;
      else if (exponent_x < 0)
	exponent_x = 0;
      BID_RETURN ((sign_x ^ sign_y) | (((BID_UINT64) exponent_x) << 23));
    }
  }
  if (!valid_y) {
    // y is Inf. or NaN

    // test if y is NaN
    if ((y & NAN_MASK32) == NAN_MASK32) {
#ifdef BID_SET_STATUS_FLAGS
      if ((y & SNAN_MASK32) == SNAN_MASK32)	// sNaN
	__set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
#endif
      BID_RETURN (coefficient_y & QUIET_MASK32);
    }
    // y is Infinity?
    if ((y & INFINITY_MASK32) == INFINITY_MASK32) {
      // check if x is 0
      if (!coefficient_x) {
	__set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
	// x==0, return NaN
	BID_RETURN (NAN_MASK32);
      }
      // otherwise return +/-Inf
      BID_RETURN (((x ^ y) & 0x80000000) | INFINITY_MASK32);
    }
    // y is 0
    exponent_x += exponent_y - DECIMAL_EXPONENT_BIAS_32;
    if (exponent_x > DECIMAL_MAX_EXPON_32)
      exponent_x = DECIMAL_MAX_EXPON_32;
    else if (exponent_x < 0)
      exponent_x = 0;
    BID_RETURN ((sign_x ^ sign_y) | (((BID_UINT64) exponent_x) << 23));
  }
 
  P = (BID_UINT64)coefficient_x * (BID_UINT64)coefficient_y;
  
  //--- get number of bits in C64 ---
  // version 2 (original)
  tempx.d = (double) P;
  bin_expon_p = ((tempx.i & MASK_BINARY_EXPONENT) >> 52)-0x3ff;
  n_digits = bid_estimate_decimal_digits[bin_expon_p];
  if(P >=bid_power10_table_128[n_digits].w[0])
	  n_digits++;

  exponent_x += exponent_y - DECIMAL_EXPONENT_BIAS_32;

  extra_digits = (n_digits<=7)? 0 : (n_digits - 7);

  exponent_x += extra_digits;

  if(!extra_digits) {
	  res = 	get_BID32 (sign_x ^ sign_y, exponent_x, P, rnd_mode,
		   pfpsf);
      BID_RETURN (res);
    }

#ifndef IEEE_ROUND_NEAREST_TIES_AWAY
#ifndef IEEE_ROUND_NEAREST
    rmode = rnd_mode;
    if ((sign_x ^ sign_y) && (unsigned) (rmode - 1) < 2)
      rmode = 3 - rmode;
#else
    rmode = 0;
#endif
#else
    rmode = 0;
#endif

	 if(exponent_x<0) rmode=3;  // RZ

      // add a constant to P, depending on rounding mode
      // 0.5*10^(digits_p - 16) for round-to-nearest
      P += bid_round_const_table[rmode][extra_digits];
      __mul_64x64_to_128(Tmp, P, bid_reciprocals10_64[extra_digits]);

      // now get P/10^extra_digits: shift Q_high right by M[extra_digits]-64
      amount = bid_short_recip_scale[extra_digits];
	  Q = Tmp.w[1] >> amount;

	  // remainder
	  R = P - Q * bid_power10_table_128[extra_digits].w[0];

	  if(R==bid_round_const_table[rmode][extra_digits])
		  status = 0;
	  else status = BID_INEXACT_EXCEPTION;

#ifdef BID_SET_STATUS_FLAGS
      __set_status_flags (pfpsf, status);
#endif

#ifndef IEEE_ROUND_NEAREST_TIES_AWAY
#ifndef IEEE_ROUND_NEAREST
      if (rmode == 0)	//BID_ROUNDING_TO_NEAREST
#endif
       if(R==0)
		   Q &= 0xfffffffe;
#endif

#if DECIMAL_TINY_DETECTION_AFTER_ROUNDING 
	  if((exponent_x==-1) && (Q==9999999) && (rnd_mode!=BID_ROUNDING_TO_ZERO))
	  {
		 rmode = rnd_mode;
#ifndef IEEE_ROUND_NEAREST_TIES_AWAY
#ifndef IEEE_ROUND_NEAREST
    rmode = rnd_mode;
    if ((sign_x^sign_y) && (unsigned) (rmode - 1) < 2)
      rmode = 3 - rmode;
#else
    rmode = 0;
#endif
#else
    rmode = 0;
#endif
            
	     if((R && (rmode==BID_ROUNDING_UP)) || ((!(rmode&3)) && (R+R>=bid_power10_table_128[extra_digits].w[0])))
		 {
			 res = very_fast_get_BID32(sign_x^sign_y, 0, 1000000);
			 BID_RETURN (res);
		 }
	  }
#endif

	  res = get_BID32_UF (sign_x^sign_y, exponent_x, Q, (BID_UINT32)R, rnd_mode, pfpsf);

    BID_RETURN (res);
  
}
