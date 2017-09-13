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

#define BID_FUNCTION_SETS_BINARY_FLAGS
#include "bid_internal.h"

BID_TYPE_FUNCTION_ARG2(BID_UINT32, bid32_add, x, y)

  BID_UINT128 Tmp;
  BID_SINT64 S, sign_ab;
  BID_UINT64 SU, CB, P, Q, R;
  BID_UINT32 sign_x, sign_y, coefficient_x, coefficient_y, res;
  BID_UINT32 sign_a, sign_b, coefficient_a, coefficient_b;
  BID_UINT32 valid_x, valid_y;
  int exponent_x, exponent_y, bin_expon, amount, n_digits, extra_digits, status, rmode;
  int exponent_a, exponent_b, scale_ca, diff_dec_expon, d2;
  int_double tempx;

  BID_OPT_SAVE_BINARY_FLAGS()

  valid_x = unpack_BID32 (&sign_x, &exponent_x, &coefficient_x, x);
  valid_y = unpack_BID32 (&sign_y, &exponent_y, &coefficient_y, y);

  // unpack arguments, check for NaN or Infinity
  if (!valid_x) {
    // x is Inf. or NaN

    // test if x is NaN
    if ((x & NAN_MASK32) == NAN_MASK32) {
#ifdef BID_SET_STATUS_FLAGS
      if (((x & SNAN_MASK32) == SNAN_MASK32)	// sNaN
	  || ((y & SNAN_MASK32) == SNAN_MASK32))
	__set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
#endif
      res = coefficient_x & QUIET_MASK32;
      BID_RETURN (res);
    }
    // x is Infinity?
    if ((x & INFINITY_MASK32) == INFINITY_MASK32) {
      // check if y is Inf
      if (((y & NAN_MASK32) == INFINITY_MASK32)) {
	if (sign_x == (y & 0x80000000)) {
	  res = coefficient_x;
	  BID_RETURN (res);
	}
	// return NaN
	{
#ifdef BID_SET_STATUS_FLAGS
	  __set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
#endif
	  res = NAN_MASK32;
	  BID_RETURN (res);
	}
      }
      // check if y is NaN
      if (((y & NAN_MASK32) == NAN_MASK32)) {
	res = coefficient_y & QUIET_MASK32;
#ifdef BID_SET_STATUS_FLAGS
	if (((y & SNAN_MASK32) == SNAN_MASK32))
	  __set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
#endif
	BID_RETURN (res);
      }
      // otherwise return +/-Inf
      {
	res = coefficient_x;
	BID_RETURN (res);
      }
    }
    // x is 0
    {
      if (((y & INFINITY_MASK32) != INFINITY_MASK32) && coefficient_y) {
	if (exponent_y <= exponent_x) {
	  res = y;
	  BID_RETURN (res);
	}
      }
    }

  }
  if (!valid_y) {
    // y is Inf. or NaN?
    if (((y & INFINITY_MASK32) == INFINITY_MASK32)) {
#ifdef BID_SET_STATUS_FLAGS
      if ((y & SNAN_MASK32) == SNAN_MASK32)	// sNaN
	__set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
#endif
      res = coefficient_y & QUIET_MASK32;
      BID_RETURN (res);
    }
    // y is 0
    if (!coefficient_x) {	// x==0
      if (exponent_x <= exponent_y)
	res = ((BID_UINT32) exponent_x) << 23;
      else
	res = ((BID_UINT32) exponent_y) << 23;
      if (sign_x == sign_y)
	res |= sign_x;
#ifndef IEEE_ROUND_NEAREST_TIES_AWAY
#ifndef IEEE_ROUND_NEAREST
      if (rnd_mode == BID_ROUNDING_DOWN && sign_x != sign_y)
	res |= 0x80000000;
#endif
#endif
      BID_RETURN (res);
    } else if (exponent_y >= exponent_x) {
      res = x;
      BID_RETURN (res);
    }
  }
  // sort arguments by exponent
  if (exponent_x < exponent_y) {
    sign_a = sign_y;
    exponent_a = exponent_y;
    coefficient_a = coefficient_y;
    sign_b = sign_x;
    exponent_b = exponent_x;
    coefficient_b = coefficient_x;
  } else {
    sign_a = sign_x;
    exponent_a = exponent_x;
    coefficient_a = coefficient_x;
    sign_b = sign_y;
    exponent_b = exponent_y;
    coefficient_b = coefficient_y;
  }

  // exponent difference
  diff_dec_expon = exponent_a - exponent_b;

  if (diff_dec_expon > MAX_FORMAT_DIGITS_32) {

	  tempx.d = (double) coefficient_a;
	  bin_expon = ((tempx.i & MASK_BINARY_EXPONENT) >> 52) - 0x3ff;
	  scale_ca = bid_estimate_decimal_digits[bin_expon];
      
	  d2 = 16 - scale_ca;
	  if(diff_dec_expon > d2)
	  {
		  diff_dec_expon = d2;
		  exponent_b = exponent_a - diff_dec_expon;
	  }
  }

    sign_ab = ((BID_SINT64)(sign_a ^ sign_b))<<32;
    sign_ab = ((BID_SINT64) sign_ab) >> 63;
    CB = ((BID_UINT64)coefficient_b + sign_ab) ^ sign_ab;

	SU = (BID_UINT64)coefficient_a * bid_power10_table_128[diff_dec_expon].w[0];
	S = SU + CB;

	if(S<0) {
		sign_a ^= 0x80000000;
		S = -S;
	}
	P = S;

	if(!P) {
		sign_a = 0;
		if(rnd_mode == BID_ROUNDING_DOWN) sign_a = 0x80000000;
		if(!coefficient_a) sign_a = sign_x;
		n_digits=0;
	}
	else {
		tempx.d = (double) P;
		bin_expon = ((tempx.i & MASK_BINARY_EXPONENT) >> 52) - 0x3ff;
		n_digits = bid_estimate_decimal_digits[bin_expon];
		if(P >=bid_power10_table_128[n_digits].w[0])
			n_digits++;
	}

	if(n_digits <= MAX_FORMAT_DIGITS_32) {
	  res = 	get_BID32 (sign_a, exponent_b, (BID_UINT32)P, rnd_mode,
		   pfpsf);
      BID_RETURN (res);
    }

	extra_digits = n_digits - 7;


	#ifndef IEEE_ROUND_NEAREST_TIES_AWAY
#ifndef IEEE_ROUND_NEAREST
    rmode = rnd_mode;
    if (sign_a && (unsigned) (rmode - 1) < 2)
      rmode = 3 - rmode;
#else
    rmode = 0;
#endif
#else
    rmode = 0;
#endif

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

	  res = get_BID32 (sign_a, exponent_b+extra_digits, Q, rnd_mode, pfpsf);

    BID_RETURN (res);
  
}





