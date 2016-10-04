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

/*****************************************************************************
 *    BID32 fma
 *****************************************************************************
 *
 *  Algorithm description:
 *
 *  if multiplication is guranteed exact (short coefficients)
 *     call the unpacked arg. equivalent of bid32_add(x*y, z)
 *  else 
 *     get full coefficient_x*coefficient_y product
 *     call subroutine to perform addition of 32-bit argument 
 *                                         to 128-bit product
 *
 ****************************************************************************/

#define BID_FUNCTION_SETS_BINARY_FLAGS
#include "bid_internal.h"


//////////////////////////////////////////////////////////////////////////
//
//    0*10^ey + cz*10^ez,   ey<ez  
//
//////////////////////////////////////////////////////////////////////////

__BID_INLINE__ BID_UINT64
add_zero32 (int exponent_y, BID_UINT32 sign_z, int exponent_z,
	    BID_UINT32 coefficient_z, unsigned *prounding_mode,
	    unsigned *fpsc) {
  int_double tempx;
  int bin_expon, scale_k, scale_cz;
  int diff_expon;

  diff_expon = exponent_z - exponent_y;

  tempx.d = (double) coefficient_z;
  bin_expon = ((tempx.i & MASK_BINARY_EXPONENT) >> 52) - 0x3ff;
  scale_cz = bid_estimate_decimal_digits[bin_expon];
  if (coefficient_z >= bid_power10_table_128[scale_cz].w[0])
    scale_cz++;

  scale_k = 7 - scale_cz;
  if (diff_expon < scale_k)
    scale_k = diff_expon;
  coefficient_z *= bid_power10_table_128[scale_k].w[0];

  return get_BID32 (sign_z, exponent_z - scale_k, coefficient_z,
		    *prounding_mode, fpsc);
}



#if DECIMAL_CALL_BY_REFERENCE
BID_EXTERN_C void bid32_mul (BID_UINT32 * pres, BID_UINT32 * px,
		       BID_UINT32 *
		       py _RND_MODE_PARAM _EXC_FLAGS_PARAM
		       _EXC_MASKS_PARAM _EXC_INFO_PARAM);
#else

BID_EXTERN_C BID_UINT32 bid32_mul (BID_UINT32 x,
			 BID_UINT32 y _RND_MODE_PARAM
			 _EXC_FLAGS_PARAM _EXC_MASKS_PARAM
			 _EXC_INFO_PARAM);
#endif

BID_TYPE0_FUNCTION_ARGTYPE1_ARGTYPE2_ARGTYPE3(BID_UINT32, bid32_fma, BID_UINT32, x, BID_UINT32, y, BID_UINT32, z)

  BID_UINT128 P, Tmp, CB, Q_high, Q_low, Stemp, C128;
  BID_UINT64 P0, C64, remainder_h, rem_l, carry, CY, coefficient_a, coefficient_b, sign_ab;
  BID_UINT32 sign_x, sign_y, coefficient_x, coefficient_y, sign_z,
    coefficient_z, R;
  BID_UINT32 sign_a, sign_b, res; 
  BID_UINT32 valid_x, valid_y, valid_z;
  int_double tempx;
  int extra_digits, exponent_x, exponent_y, exponent_z, bin_expon, rmode, inexact=0;
  int n_digits, amount, status, exponent_a, exponent_b, diff_dec_expon, d2, scale_ca;

  BID_OPT_SAVE_BINARY_FLAGS()

  valid_x = unpack_BID32 (&sign_x, &exponent_x, &coefficient_x, x);
  valid_y = unpack_BID32 (&sign_y, &exponent_y, &coefficient_y, y);
  valid_z = unpack_BID32 (&sign_z, &exponent_z, &coefficient_z, z);

  // unpack arguments, check for NaN, Infinity, or 0
  if (!valid_x || !valid_y || !valid_z) {


      if ((y & NAN_MASK32) == NAN_MASK32) {
#ifdef BID_SET_STATUS_FLAGS
      if (((x & SNAN_MASK32) == SNAN_MASK32)	// sNaN
	  || ((y & SNAN_MASK32) == SNAN_MASK32)|| ((z & SNAN_MASK32) == SNAN_MASK32))
	__set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
#endif
      res = coefficient_y & QUIET_MASK32;
      BID_RETURN (res);
    }
      if ((z & NAN_MASK32) == NAN_MASK32) {
#ifdef BID_SET_STATUS_FLAGS
      if (((x & SNAN_MASK32) == SNAN_MASK32)	// sNaN
	  || ((z & SNAN_MASK32) == SNAN_MASK32))
	__set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
#endif
      res = coefficient_z & QUIET_MASK32;
      BID_RETURN (res);
    }
      if ((x & NAN_MASK32) == NAN_MASK32) {
#ifdef BID_SET_STATUS_FLAGS
      if (((x & SNAN_MASK32) == SNAN_MASK32))	// sNaN
	__set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
#endif
      res = coefficient_x & QUIET_MASK32;
      BID_RETURN (res);
    }
    

    if (!valid_x) {
      // x is Inf. or 0

      // x is Infinity?
      if ((x & 0x78000000) == 0x78000000) {
	// check if y is 0
	if (!coefficient_y) {
	  // y==0, return NaN
#ifdef BID_SET_STATUS_FLAGS
	  if ((z & 0x7e000000) != 0x7c000000)
	    __set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
#endif
	  BID_RETURN (0x7c000000);
	}
	// test if z is Inf of oposite sign
	if (((z & 0x7c000000) == 0x78000000)
	    && (((x ^ y) ^ z) & 0x80000000)) {
	  // return NaN 
#ifdef BID_SET_STATUS_FLAGS
	  __set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
#endif
	  BID_RETURN (0x7c000000);
	}
	// otherwise return +/-Inf
	BID_RETURN (((x ^ y) & 0x80000000) |
		    0x78000000);
      }
      // x is 0
      if (((y & 0x78000000) != 0x78000000)
	  && ((z & 0x78000000) != 0x78000000)) {

	if (coefficient_z) {
	  exponent_y = exponent_x - DECIMAL_EXPONENT_BIAS_32 + exponent_y;

	  sign_z = z & 0x80000000;

	  if (exponent_y >= exponent_z)
	    BID_RETURN (z);
	  res =
	    add_zero32 (exponent_y, sign_z, exponent_z, coefficient_z,
			&rnd_mode, pfpsf);
	  BID_RETURN (res);
	}
      }
    }
    if (!valid_y) {
      // y is Inf. or 0

      // y is Infinity?
      if ((y & 0x78000000) == 0x78000000) {
	// check if x is 0
	if (!coefficient_x) {
	  // y==0, return NaN
#ifdef BID_SET_STATUS_FLAGS
	  __set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
#endif
	  BID_RETURN (0x7c000000);
	}
	// test if z is Inf of oposite sign
	if (((z & 0x7c000000) == 0x78000000)
	    && (((x ^ y) ^ z) & 0x80000000)) {
#ifdef BID_SET_STATUS_FLAGS
	  __set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
#endif
	  // return NaN
	  BID_RETURN (0x7c000000);
	}
	// otherwise return +/-Inf
	BID_RETURN (((x ^ y) & 0x80000000) |
		    0x78000000);
      }
      // y is 0 
      if (((z & 0x78000000) != 0x78000000)) {

	if (coefficient_z) {
	  exponent_y += exponent_x - DECIMAL_EXPONENT_BIAS_32;

	  sign_z = z & 0x80000000;

	  if (exponent_y >= exponent_z)
	    BID_RETURN (z);
	  res =
	    add_zero32 (exponent_y, sign_z, exponent_z, coefficient_z,
			&rnd_mode, pfpsf);
	  BID_RETURN (res);
	}
      }
    }

    if (!valid_z) {
      // y is Inf. or 0

      // test if y is NaN/Inf
      if ((z & 0x78000000) == 0x78000000) {
	BID_RETURN (coefficient_z & QUIET_MASK32);
      }
      // z is 0, return x*y
      if ((!coefficient_x) || (!coefficient_y)) {
	//0+/-0
	exponent_x += exponent_y - DECIMAL_EXPONENT_BIAS_32;
	if (exponent_x > DECIMAL_MAX_EXPON_32)
	  exponent_x = DECIMAL_MAX_EXPON_32;
	else if (exponent_x < 0)
	  exponent_x = 0;
	if (exponent_x <= exponent_z)
	  res = ((BID_UINT32) exponent_x) << 23;
	else
	  res = ((BID_UINT32) exponent_z) << 23;
	if ((sign_x ^ sign_y) == sign_z)
	  res |= sign_z;
#ifndef IEEE_ROUND_NEAREST_TIES_AWAY
#ifndef IEEE_ROUND_NEAREST
	else if (rnd_mode == BID_ROUNDING_DOWN)
	  res |= 0x80000000;
#endif
#endif
	BID_RETURN (res);
      }
      d2 = exponent_x + exponent_y - DECIMAL_EXPONENT_BIAS_32;
	  if(exponent_z>d2)
		  exponent_z = d2;
	}
  }



  P0 = (BID_UINT64)coefficient_x * (BID_UINT64)coefficient_y;
  exponent_x += exponent_y - DECIMAL_EXPONENT_BIAS_32;
  
    // sort arguments by exponent
  if (exponent_x < exponent_z) {
    sign_a = sign_z;
    exponent_a = exponent_z;
    coefficient_a = coefficient_z;
    sign_b = sign_x ^ sign_y;
    exponent_b = exponent_x;
    coefficient_b = P0;
  } else {
    sign_a = sign_x ^ sign_y;
    exponent_a = exponent_x;
    coefficient_a = P0;
    sign_b = sign_z;
    exponent_b = exponent_z;
    coefficient_b = coefficient_z;
  }

    // exponent difference
  diff_dec_expon = exponent_a - exponent_b;

  if (diff_dec_expon > 17) {

	  tempx.d = (double) coefficient_a;
	  bin_expon = ((tempx.i & MASK_BINARY_EXPONENT) >> 52) - 0x3ff;
	  scale_ca = bid_estimate_decimal_digits[bin_expon];
      
	  d2 = 31 - scale_ca;
	  if(diff_dec_expon > d2)
	  {
		  diff_dec_expon = d2;
		  exponent_b = exponent_a - diff_dec_expon;
	  }
	  if(coefficient_b) 
		  inexact=1;
  }

    sign_ab = ((BID_SINT64)(sign_a ^ sign_b))<<32;
    sign_ab = ((BID_SINT64) sign_ab) >> 63;
    CB.w[0] = (coefficient_b + sign_ab) ^ sign_ab;
	CB.w[1] = ((BID_SINT64)CB.w[0]) >> 63;

   __mul_64x128_low(Tmp, coefficient_a, bid_power10_table_128[diff_dec_expon]);
   __add_128_128(P, Tmp, CB);
   if(((BID_SINT64)P.w[1])<0) {
	   sign_a ^= 0x80000000;
	   P.w[1] = 0 - P.w[1];
	   if(P.w[0]) P.w[1]--;
	   P.w[0] = 0 - P.w[0];
   }

   if(P.w[1]) {
	  tempx.d = (double) P.w[1];
	  bin_expon = ((tempx.i & MASK_BINARY_EXPONENT) >> 52) - 0x3ff + 64;
	  n_digits = bid_estimate_decimal_digits[bin_expon];
	  if(__unsigned_compare_ge_128 (P, bid_power10_table_128[n_digits]))
		  n_digits ++;
   } else {
	   if(P.w[0]) {
			tempx.d = (double) P.w[0];
			bin_expon = ((tempx.i & MASK_BINARY_EXPONENT) >> 52) - 0x3ff;
			n_digits = bid_estimate_decimal_digits[bin_expon];
			if(P.w[0] >= bid_power10_table_128[n_digits].w[0])
				n_digits++;
	   } else { // result = 0
			sign_a = 0;
			if(rnd_mode == BID_ROUNDING_DOWN) sign_a = 0x80000000;
			if(!coefficient_a) sign_a = sign_x;
			n_digits=0;
	   }}

   if(n_digits <= MAX_FORMAT_DIGITS_32) {
	  res = 	get_BID32_UF (sign_a, exponent_b, (BID_UINT32)P.w[0], 0, rnd_mode,
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

	 if(exponent_b+extra_digits<0) rmode=3;  // RZ

      // add a constant to P, depending on rounding mode
      // 0.5*10^(digits_p - 16) for round-to-nearest
  if(extra_digits <= 18) {
      __add_128_64 (P, P, bid_round_const_table[rmode][extra_digits]);
  } 
  else {
	  __mul_64x64_to_128(Stemp, bid_round_const_table[rmode][18], bid_power10_table_128[extra_digits-18].w[0]);
      __add_128_128 (P, P, Stemp);
	  if(rmode == BID_ROUNDING_UP) {
         __add_128_64 (P, P, bid_round_const_table[rmode][extra_digits-18]);
	  }
  } 

      // get P*(2^M[extra_digits])/10^extra_digits
      __mul_128x128_full (Q_high, Q_low, P,
			  bid_reciprocals10_128[extra_digits]);
      // now get P/10^extra_digits: shift Q_high right by M[extra_digits]-128
      amount = bid_recip_scale[extra_digits];
      __shr_128_long (C128, Q_high, amount);

      C64 = __low_64 (C128);


#ifndef IEEE_ROUND_NEAREST_TIES_AWAY
#ifndef IEEE_ROUND_NEAREST
      if (rmode == 0)	//BID_ROUNDING_TO_NEAREST
#endif
	if ((C64 & 1)) {
	  // check whether fractional part of initial_P/10^extra_digits 
	  // is exactly .5
	  // this is the same as fractional part of 
	  // (initial_P + 0.5*10^extra_digits)/10^extra_digits is exactly zero

	  // get remainder
	  rem_l = Q_high.w[0];
	if(amount<64)
	{ remainder_h = Q_high.w[0] << (64 - amount); rem_l = 0;}
	else remainder_h = Q_high.w[1] << (128 - amount);

	  // test whether fractional part is 0
	  if (!(remainder_h | rem_l)
	      && (Q_low.w[1] < bid_reciprocals10_128[extra_digits].w[1]
		  || (Q_low.w[1] == bid_reciprocals10_128[extra_digits].w[1]
		      && Q_low.w[0] <
		      bid_reciprocals10_128[extra_digits].w[0]))) {
	    C64--;
	  }
	}
#endif

      status = BID_INEXACT_EXCEPTION;

      // get remainder
	  rem_l = Q_high.w[0];
	if(amount<64)
	{ remainder_h = Q_high.w[0] << (64 - amount); rem_l = 0;}
	else remainder_h = Q_high.w[1] << (128 - amount);

      switch (rmode) {
      case BID_ROUNDING_TO_NEAREST:
      case BID_ROUNDING_TIES_AWAY:
	// test whether fractional part is 0
	if ((remainder_h == 0x8000000000000000ull && !rem_l)
	    && (Q_low.w[1] < bid_reciprocals10_128[extra_digits].w[1]
		|| (Q_low.w[1] == bid_reciprocals10_128[extra_digits].w[1]
		    && Q_low.w[0] <
		    bid_reciprocals10_128[extra_digits].w[0])))
	  status = BID_EXACT_STATUS;
	break;
      case BID_ROUNDING_DOWN:
      case BID_ROUNDING_TO_ZERO:
	if (!(remainder_h|rem_l)
	    && (Q_low.w[1] < bid_reciprocals10_128[extra_digits].w[1]
		|| (Q_low.w[1] == bid_reciprocals10_128[extra_digits].w[1]
		    && Q_low.w[0] <
		    bid_reciprocals10_128[extra_digits].w[0])))
	  status = BID_EXACT_STATUS;
	break;
      default:
	// round up
	__add_carry_out (Stemp.w[0], CY, Q_low.w[0],
			 bid_reciprocals10_128[extra_digits].w[0]);
	__add_carry_in_out (Stemp.w[1], carry, Q_low.w[1],
			    bid_reciprocals10_128[extra_digits].w[1], CY);
	if(amount<64) {
	if ((remainder_h >> (64 - amount)) + carry >=
	    (((BID_UINT64) 1) << amount))
		if(!inexact)
		  status = BID_EXACT_STATUS;
	}
	else {
		rem_l += carry;
		remainder_h >>= (128 - amount);
		if(carry && (!rem_l)) remainder_h++;
		if((remainder_h >= (((BID_UINT64) 1) << (amount-64))) && !inexact)
		  status = BID_EXACT_STATUS;
	}
      }

#ifdef BID_SET_STATUS_FLAGS
      __set_status_flags (pfpsf, status);
#endif

     R = (status!=BID_EXACT_STATUS);

#if DECIMAL_TINY_DETECTION_AFTER_ROUNDING 
	 if(((BID_UINT32)C64==9999999) && (exponent_b+extra_digits==-1) && (rnd_mode!=BID_ROUNDING_TO_ZERO))
	 {
		 rmode = rnd_mode;
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
  if(extra_digits <= 18) {
      __add_128_64 (P, P, bid_round_const_table[rmode][extra_digits]);
  } 
  else {
	  __mul_64x64_to_128(Stemp, bid_round_const_table[rmode][18], bid_power10_table_128[extra_digits-18].w[0]);
      __add_128_128 (P, P, Stemp);
	  if(rmode == BID_ROUNDING_UP) {
         __add_128_64 (P, P, bid_round_const_table[rmode][extra_digits-18]);
	  }
  } 

      // get P*(2^M[extra_digits])/10^extra_digits
      __mul_128x128_full (Q_high, Q_low, P,
			  bid_reciprocals10_128[extra_digits]);
      // now get P/10^extra_digits: shift Q_high right by M[extra_digits]-128
      amount = bid_recip_scale[extra_digits];
      __shr_128_long (C128, Q_high, amount);

      C64 = __low_64 (C128);
	  if(C64==10000000) {
		  res = sign_a | 1000000;
		  BID_RETURN (res); 
	  }
	 }
#endif

	 res = get_BID32_UF (sign_a, exponent_b+extra_digits, (BID_UINT32)C64, (BID_UINT32)R, rnd_mode, pfpsf);

     BID_RETURN (res);
  
}
