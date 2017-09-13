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
 *  BID32 nextup
 ****************************************************************************/

#if DECIMAL_CALL_BY_REFERENCE
void
bid32_nextup (BID_UINT32 * pres, BID_UINT32 * px 
    _EXC_FLAGS_PARAM _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
  BID_UINT32 x = *px;
#else
DFP_WRAPFN_DFP(32, bid32_nextup, 32)
BID_UINT32
bid32_nextup (BID_UINT32 x 
    _EXC_FLAGS_PARAM _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
#endif
  BID_UINT32 res;
  BID_UINT32 x_sign;
  BID_UINT32 x_exp;
  BID_UI32FLOAT tmp1;
  int x_nr_bits;
  int q1, ind;
  BID_UINT32 C1; // C1 represents x_signif (BID_UINT32)

  // check for NaNs and infinities
  if ((x & MASK_NAN32) == MASK_NAN32) { // check for NaN
    if ((x & 0x000fffff) > 999999)
      x = x & 0xfe000000; // clear G6-G10 and the payload bits
    else
      x = x & 0xfe0fffff; // clear G6-G10
    if ((x & MASK_SNAN32) == MASK_SNAN32) { // SNaN
      // set invalid flag
      *pfpsf |= BID_INVALID_EXCEPTION;
      // return quiet (SNaN)
      res = x & 0xfdffffff;
    } else {	// QNaN
      res = x;
    }
    BID_RETURN (res);
  } else if ((x & MASK_INF32) == MASK_INF32) { // check for Infinity
    if (!(x & 0x80000000)) { // x is +inf
      res = 0x78000000;
    } else { // x is -inf
      res = 0xf7f8967f;	// -MAXFP = -9999999 * 10^emax
    }
    BID_RETURN (res);
  }
  // unpack the argument
  x_sign = x & MASK_SIGN32; // 0 for positive, MASK_SIGN32 for negative
  // if steering bits are 11 (condition will be 0), then exponent is G[0:7]
  if ((x & MASK_STEERING_BITS32) == MASK_STEERING_BITS32) {
    x_exp = (x & MASK_BINARY_EXPONENT2_32) >> 21; // biased
    C1 = (x & MASK_BINARY_SIG2_32) | MASK_BINARY_OR2_32;
    if (C1 > 9999999) {	// non-canonical
      x_exp = 0;
      C1 = 0;
    }
  } else {
    x_exp = (x & MASK_BINARY_EXPONENT1_32) >> 23; // biased
    C1 = x & MASK_BINARY_SIG1_32;
  }

  // check for zeros (possibly from non-canonical values)
  if (C1 == 0x0) {
    // x is 0
    res = 0x00000001; // MINFP = 1 * 10^emin
  } else { // x is not special and is not zero
    if (x == 0x77f8967f) {
      // x = +MAXFP = 9999999 * 10^emax
      res = 0x78000000; // +inf
    } else if (x == 0x80000001) {
      // x = -MINFP = 1...99 * 10^emin
      res = 0x80000000; // -0
    } else {	// -MAXFP <= x <= -MINFP - 1 ulp OR MINFP <= x <= MAXFP - 1 ulp
      // can add/subtract 1 ulp to the significand

      // Note: we could check here if x >= 10^7 to speed up the case q1 = 7
      // q1 = nr. of decimal digits in x (1 <= q1 <= 7)
      //  determine first the nr. of bits in x
      tmp1.f = (float) C1; // exact conversion
      x_nr_bits = 1 + ((tmp1.ui32 >> 23) & 0xff) - 0x7f;
      q1 = bid_nr_digits[x_nr_bits - 1].digits;
      if (q1 == 0) {
	q1 = bid_nr_digits[x_nr_bits - 1].digits1;
	if (C1 >= bid_nr_digits[x_nr_bits - 1].threshold_lo)
	  q1++;
      }
      // if q1 < P7 then pad the significand with zeros
      if (q1 < P7) {
	if (x_exp > (BID_UINT32)(P7 - q1)) {
	  ind = P7 - q1; // 1 <= ind <= P7 - 1
	  // pad with P7 - q1 zeros, until exponent = emin
	  // C1 = C1 * 10^ind
	  C1 = C1 * bid_ten2k64[ind];
	  x_exp = x_exp - ind;
	} else { // pad with zeros until the exponent reaches emin
	  ind = x_exp;
	  C1 = C1 * bid_ten2k64[ind];
	  x_exp = EXP_MIN32;
	}
      }
      if (!x_sign) {	// x > 0
	// add 1 ulp (add 1 to the significand)
	C1++;
	if (C1 == 0x989680) { // if  C1 = 10^7
	  C1 = 0x0f4240; // C1 = 10^6
	  x_exp++;
	}
	// Ok, because MAXFP = 9999999 * 10^emax was caught already
      } else {	// x < 0
	// subtract 1 ulp (subtract 1 from the significand)
	C1--;
	if (C1 == 0x0f423f && x_exp != 0) { // if  C1 = 10^6 - 1
	  C1 = 0x98967f; // C1 = 10^7 - 1
	  x_exp--;
	}
      }
      // assemble the result
      // if significand has 24 bits
      if (C1 & MASK_BINARY_OR2_32) {
	res = x_sign | (x_exp << 21) | MASK_STEERING_BITS32 | 
            (C1 & MASK_BINARY_SIG2_32);
      } else {	// significand fits in 23 bits
	res = x_sign | (x_exp << 23) | C1;
      }
    } // end -MAXFP <= x <= -MINFP - 1 ulp OR MINFP <= x <= MAXFP - 1 ulp
  } // end x is not special and is not zero
  BID_RETURN (res);
}

/*****************************************************************************
 *  BID32 nextdown
 ****************************************************************************/

#if DECIMAL_CALL_BY_REFERENCE
void
bid32_nextdown (BID_UINT32 * pres, BID_UINT32 * px 
    _EXC_FLAGS_PARAM _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
  BID_UINT32 x = *px;
#else
DFP_WRAPFN_DFP(32, bid32_nextdown, 32)
BID_UINT32
bid32_nextdown (BID_UINT32 x 
    _EXC_FLAGS_PARAM _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
#endif
  BID_UINT32 res;
  BID_UINT32 x_sign;
  BID_UINT32 x_exp;
  BID_UI32FLOAT tmp1;
  int x_nr_bits;
  int q1, ind;
  BID_UINT32 C1; // C1 represents x_signif (BID_UINT32)

  // check for NaNs and infinities
  if ((x & MASK_NAN32) == MASK_NAN32) {	// check for NaN 
    if ((x & 0x000fffff) > 999999)
      x = x & 0xfe000000; // clear G6-G10 and the payload bits 
    else
      x = x & 0xfe0fffff; // clear G6-G10 
    if ((x & MASK_SNAN32) == MASK_SNAN32) { // SNaN 
      // set invalid flag
      *pfpsf |= BID_INVALID_EXCEPTION;
      // return quiet (SNaN)
      res = x & 0xfdffffff;
    } else { // QNaN 
      res = x;
    }
    BID_RETURN (res);
  } else if ((x & MASK_INF32) == MASK_INF32) { // check for Infinity
    if (x & 0x80000000) { // x is -inf
      res = 0xf8000000;
    } else { // x is +inf
      res = 0x77f8967f;	// +MAXFP = +9999999 * 10^emax
    }
    BID_RETURN (res);
  }
  // unpack the argument
  x_sign = x & MASK_SIGN32; // 0 for positive, MASK_SIGN32 for negative
  // if steering bits are 11 (condition will be 0), then exponent is G[0:7]
  if ((x & MASK_STEERING_BITS32) == MASK_STEERING_BITS32) {
    x_exp = (x & MASK_BINARY_EXPONENT2_32) >> 21; // biased
    C1 = (x & MASK_BINARY_SIG2_32) | MASK_BINARY_OR2_32;
    if (C1 > 9999999) {	// non-canonical
      x_exp = 0;
      C1 = 0;
    }
  } else {
    x_exp = (x & MASK_BINARY_EXPONENT1_32) >> 23; // biased
    C1 = x & MASK_BINARY_SIG1_32;
  }

  // check for zeros (possibly from non-canonical values)
  if (C1 == 0x0) {
    // x is 0
    res = 0x80000001; // -MINFP = -1 * 10^emin
  } else { // x is not special and is not zero
    if (x == 0xf7f8967f) {
      // x = -MAXFP = -9999999 * 10^emax
      res = 0xf8000000;	// -inf
    } else if (x == 0x00000001) {
      // x = +MINFP = 1 * 10^emin
      res = 0x00000000; // +0
    } else { // -MAXFP + 1ulp <= x <= -MINFP OR MINFP + 1 ulp <= x <= MAXFP
      // can add/subtract 1 ulp to the significand

      // Note: we could check here if x >= 10^7 to speed up the case q1 = 7
      // q1 = nr. of decimal digits in x (1 <= q1 <= 7)
      //  determine first the nr. of bits in x
      tmp1.f = (float) C1; // exact conversion
      x_nr_bits = 1 + ((tmp1.ui32 >> 23) & 0xff) - 0x7f;
      q1 = bid_nr_digits[x_nr_bits - 1].digits;
      if (q1 == 0) {
	q1 = bid_nr_digits[x_nr_bits - 1].digits1;
	if (C1 >= bid_nr_digits[x_nr_bits - 1].threshold_lo)
	  q1++;
      }
      // if q1 < P7 then pad the significand with zeros
      if (q1 < P7) {
	if (x_exp > (BID_UINT32)(P7 - q1)) {
	  ind = P7 - q1; // 1 <= ind <= P7 - 1
	  // pad with P7 - q1 zeros, until exponent = emin
	  // C1 = C1 * 10^ind
	  C1 = C1 * bid_ten2k64[ind];
	  x_exp = x_exp - ind;
	} else {	// pad with zeros until the exponent reaches emin
	  ind = x_exp;
	  C1 = C1 * bid_ten2k64[ind];
	  x_exp = EXP_MIN32;
	}
      }
      if (x_sign) {	// x < 0
	// add 1 ulp (add 1 to the significand)
	C1++;
	if (C1 == 0x989680) { // if  C1 = 10^7
	  C1 = 0x0f4240; // C1 = 10^6
	  x_exp++;
	}
        // Ok, because -MAXFP = -9999999 * 10^emax was caught already
      } else {	// x > 0
	// subtract 1 ulp (subtract 1 from the significand)
	C1--;
	if (C1 == 0x0f423f && x_exp != 0) { // if  C1 = 10^6 - 1
	  C1 = 0x98967f; // C1 = 10^7 - 1
	  x_exp--;
	}
      }
      // assemble the result
      // if significand has 24 bits
      if (C1 & MASK_BINARY_OR2_32) {
	res = x_sign | (x_exp << 21) | MASK_STEERING_BITS32 | 
            (C1 & MASK_BINARY_SIG2_32);
      } else {	// significand fits in 23 bits
	res = x_sign | (x_exp << 23) | C1;
      }
    }	// end -MAXFP <= x <= -MINFP - 1 ulp OR MINFP <= x <= MAXFP - 1 ulp
  }	// end x is not special and is not zero
  BID_RETURN (res);

}

/*****************************************************************************
 *  BID32 nextafter
 ****************************************************************************/

BID_TYPE0_FUNCTION_ARGTYPE1_ARGTYPE2_NORND(BID_UINT32, bid32_nextafter, BID_UINT32, x, BID_UINT32, y)

  BID_UINT32 res;
  BID_UINT32 tmp1, tmp2;
  BID_FPSC tmp_fpsf = 0; // dummy fpsf for calls to comparison functions
  int res1, res2;

  // check for NaNs or infinities
  if (((x & MASK_SPECIAL32) == MASK_SPECIAL32) ||
      ((y & MASK_SPECIAL32) == MASK_SPECIAL32)) {
    // x is NaN or infinity or y is NaN or infinity

    if ((x & MASK_NAN32) == MASK_NAN32) { // x is NAN
      if ((x & 0x000fffff) > 999999)
	x = x & 0xfe000000; // clear G6-G10 and the payload bits
      else
	x = x & 0xfe0fffff; // clear G6-G10
      if ((x & MASK_SNAN32) == MASK_SNAN32) { // x is SNAN
	// set invalid flag
	*pfpsf |= BID_INVALID_EXCEPTION;
	// return quiet (x)
	res = x & 0xfdffffff;
      } else {	// x is QNaN
	if ((y & MASK_SNAN32) == MASK_SNAN32) {	// y is SNAN
	  // set invalid flag
	  *pfpsf |= BID_INVALID_EXCEPTION;
	}
	// return x
	res = x;
      }
      BID_RETURN (res);
    } else if ((y & MASK_NAN32) == MASK_NAN32) {	// y is NAN
      if ((y & 0x000fffff) > 999999)
	y = y & 0xfe000000; // clear G6-G10 and the payload bits
      else
	y = y & 0xfe0fffff; // clear G6-G10
      if ((y & MASK_SNAN32) == MASK_SNAN32) { // y is SNAN
	// set invalid flag
	*pfpsf |= BID_INVALID_EXCEPTION;
	// return quiet (y)
	res = y & 0xfdffffff;
      } else {	// y is QNaN
	// return y
	res = y;
      }
      BID_RETURN (res);
    } else {	// at least one is infinity
      if ((x & MASK_ANY_INF32) == MASK_INF32) { // x = inf
	x = x & (MASK_SIGN32 | MASK_INF32);
      }
      if ((y & MASK_ANY_INF32) == MASK_INF32) { // y = inf
	y = y & (MASK_SIGN32 | MASK_INF32);
      }
    }
  }
  // neither x nor y is NaN

  // if not infinity, check for non-canonical values x (treated as zero)
  if ((x & MASK_ANY_INF32) != MASK_INF32) { // x != inf
    // unpack x
    if ((x & MASK_STEERING_BITS32) == MASK_STEERING_BITS32) {
      // if the steering bits are 11 (condition will be 0), then
      // the exponent is G[0:7]
      if (((x & MASK_BINARY_SIG2_32) | MASK_BINARY_OR2_32) > 9999999) {
	// non-canonical
	x = (x & MASK_SIGN32) | ((x & MASK_BINARY_EXPONENT2_32) << 2);
      }
    } else { 
      // if ((x & MASK_STEERING_BITS32) != MASK_STEERING_BITS32) x is unchanged
      ;	// canonical
    }
  }
  // no need to check for non-canonical y

  // neither x nor y is NaN
  tmp_fpsf = *pfpsf; // save fpsf
#if DECIMAL_CALL_BY_REFERENCE
  bid32_quiet_equal (&res1, &x, &y 
      _EXC_FLAGS_ARG _EXC_MASKS_ARG _EXC_INFO_ARG);
  bid32_quiet_greater (&res2, &x, &y 
      _EXC_FLAGS_ARG _EXC_MASKS_ARG _EXC_INFO_ARG);
#else
  res1 = bid32_quiet_equal (x, y 
      _EXC_FLAGS_ARG _EXC_MASKS_ARG _EXC_INFO_ARG);
  res2 = bid32_quiet_greater (x, y 
      _EXC_FLAGS_ARG _EXC_MASKS_ARG _EXC_INFO_ARG);
#endif
  *pfpsf = tmp_fpsf; // restore fpsf
  if (res1) { // x = y
    // return x with the sign of y
    res = (y & 0x80000000) | (x & 0x7fffffff);
  } else if (res2) { // x > y
#if DECIMAL_CALL_BY_REFERENCE
    bid32_nextdown (&res, &x _EXC_FLAGS_ARG _EXC_MASKS_ARG _EXC_INFO_ARG);
#else
    res = bid32_nextdown (x _EXC_FLAGS_ARG _EXC_MASKS_ARG _EXC_INFO_ARG);
#endif
  } else {	// x < y
#if DECIMAL_CALL_BY_REFERENCE
    bid32_nextup (&res, &x _EXC_FLAGS_ARG _EXC_MASKS_ARG _EXC_INFO_ARG);
#else
    res = bid32_nextup (x _EXC_FLAGS_ARG _EXC_MASKS_ARG _EXC_INFO_ARG);
#endif
  }
  // if the operand x is finite but the result is infinite, signal
  // overflow and inexact
  if (((x & MASK_INF32) != MASK_INF32) && ((res & MASK_INF32) == MASK_INF32)) {
    // set the inexact flag
    *pfpsf |= BID_INEXACT_EXCEPTION;
    // set the overflow flag
    *pfpsf |= BID_OVERFLOW_EXCEPTION;
  }
  // if the result is in (-10^emin, 10^emin), and is different from the
  // operand x, signal underflow and inexact 
  tmp1 = 0x0f4240; // +1000000 * 10^emin
  tmp2 = res & 0x7fffffff;
  tmp_fpsf = *pfpsf; // save fpsf
#if DECIMAL_CALL_BY_REFERENCE
  bid32_quiet_greater (&res1, &tmp1, &tmp2 
      _EXC_FLAGS_ARG _EXC_MASKS_ARG _EXC_INFO_ARG);
  bid32_quiet_not_equal (&res2, &x, &res 
      _EXC_FLAGS_ARG _EXC_MASKS_ARG _EXC_INFO_ARG);
#else
  res1 = bid32_quiet_greater (tmp1, tmp2 
      _EXC_FLAGS_ARG _EXC_MASKS_ARG _EXC_INFO_ARG);
  res2 = bid32_quiet_not_equal (x, res 
      _EXC_FLAGS_ARG _EXC_MASKS_ARG _EXC_INFO_ARG);
#endif
  *pfpsf = tmp_fpsf; // restore fpsf
  if (res1 && res2) {
    // if (bid32_quiet_greater (tmp1, tmp2, &tmp_fpsf) &&
    // bid32_quiet_not_equal (x, res, &tmp_fpsf)) {
    // set the inexact flag
    *pfpsf |= BID_INEXACT_EXCEPTION;
    // set the underflow flag
    *pfpsf |= BID_UNDERFLOW_EXCEPTION;
  }
  BID_RETURN (res);
}
