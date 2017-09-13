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
 *  BID32_to_int32_rnint
 ****************************************************************************/

#if DECIMAL_CALL_BY_REFERENCE
void
bid32_to_int32_rnint (int *pres,
		      BID_UINT32*
		      px _EXC_FLAGS_PARAM _EXC_MASKS_PARAM
		      _EXC_INFO_PARAM) {
  BID_UINT32 x = *px;
#else
RES_WRAPFN_DFP(int, bid32_to_int32_rnint, 32)
int
bid32_to_int32_rnint (BID_UINT32 x _EXC_FLAGS_PARAM _EXC_MASKS_PARAM
		      _EXC_INFO_PARAM) {
#endif
  int res;
 BID_UINT32 x_sign;
 BID_UINT32 x_exp;
  int exp; // unbiased exponent
  // Note: C1 represents x_significand (BID_UINT32)
  BID_UINT64 tmp64;
  BID_UI32FLOAT tmp1;
  unsigned int x_nr_bits;
  int q, ind, shift;
  BID_UINT32 C1;
  BID_UINT64 Cstar; // C* represents up to 7 decimal digits ~ 24 bits
  BID_UINT128 fstar;
  BID_UINT128 P128;

  // check for NaN or Infinity
  if ((x & MASK_NAN32) == MASK_NAN32 || (x & MASK_INF32) == MASK_INF32) {
    // set invalid flag
    *pfpsf |= BID_INVALID_EXCEPTION;
    // return Integer Indefinite
    res = 0x80000000;
    BID_RETURN (res);
  }
  // unpack x
  x_sign = x & MASK_SIGN32;	// 0 for positive, MASK_SIGN32 for negative
  // if steering bits are 11 (condition will be 0), then exponent is G[0:w+1] =>
  if ((x & MASK_STEERING_BITS32) == MASK_STEERING_BITS32) {
    x_exp = (x & MASK_BINARY_EXPONENT2_32) >> 21;	// biased
    C1 = (x & MASK_BINARY_SIG2_32) | MASK_BINARY_OR2_32;
    if (C1 > 9999999) {	// non-canonical
      x_exp = 0;
      C1 = 0;
    }
  } else {
    x_exp = (x & MASK_BINARY_EXPONENT1_32) >> 23;	// biased
    C1 = x & MASK_BINARY_SIG1_32;
  }

  // check for zeros (possibly from non-canonical values)
  if (C1 == 0x0) {
    // x is 0
    res = 0x00000000;
    BID_RETURN (res);
  }
  // x is not special and is not zero

  // q = nr. of decimal digits in x (1 <= q <= 7)
  //  determine first the nr. of bits in x
  tmp1.f = (float) C1; // exact conversion
  x_nr_bits = 1 + ((tmp1.ui32 >> 23) & 0xff) - 0x7f;
  q = bid_nr_digits[x_nr_bits - 1].digits;
  if (q == 0) {
    q = bid_nr_digits[x_nr_bits - 1].digits1;
    if (C1 >= bid_nr_digits[x_nr_bits - 1].threshold_lo)
      q++;
  }
  exp = x_exp - 101;	// unbiased exponent

  if ((q + exp) > 10) {	// x >= 10^10 ~= 2^33.2... (cannot fit in 32 bits)
    // set invalid flag
    *pfpsf |= BID_INVALID_EXCEPTION;
    // return Integer Indefinite
    res = 0x80000000;
    BID_RETURN (res);
  } else if ((q + exp) == 10) {	// x = c(0)c(1)...c(q-1)00..0 (10 dec. digits)
    // in this case 2^29.89... ~= 10^9 <= x < 10^10 ~= 2^33.2...
    // so x rounded to an integer may or may not fit in a signed 32-bit int
    // the cases that do not fit are identified here; the ones that fit
    // fall through and will be handled with other cases further,
    // under '1 <= q + exp <= 10'
    if (x_sign) {	// if n < 0 and q + exp = 10
      // if n < -2^31 - 1/2 then n is too large
      // too large if c(0)c(1)...c(q-1)00...0 > 2^31+1/2
      // <=> C * 10^(11-q) > 0x500000005, 1<=q<=7
      // Note: C * 10^(11-q) has 10 or 11 digits; 0x500000005 has 11 digits
      tmp64 = C1 * bid_ten2k64[11 - q];	// C scaled up to 11-digit int
      // c(0)c(1)...c(q-1)00...0 (11 digits)
      if (tmp64 > 0x500000005ull) {
        // set invalid flag
        *pfpsf |= BID_INVALID_EXCEPTION;
        // return Integer Indefinite
        res = 0x80000000;
        BID_RETURN (res);
      }
      // else cases that can be rounded to a 32-bit int fall through
      // to '1 <= q + exp <= 10'
    } else {	// if n > 0 and q + exp = 10
      // if n >= 2^31 - 1/2 then n is too large
      // too large if c(0)c(1)...c(q-1)00...0 >= 2^31-1/2
      // <=> C * 10^(11-q) >= 0x4fffffffb, 1<=q<=7
      // Note: C * 10^(11-q) has 10 or 11 digits; 0x500000005 has 11 digits
      tmp64 = C1 * bid_ten2k64[11 - q];	// C scaled up to 11-digit int
      // c(0)c(1)...c(q-1)00...0 (11 digits)
      if (tmp64 >= 0x4fffffffbull) {
        // set invalid flag
        *pfpsf |= BID_INVALID_EXCEPTION;
        // return Integer Indefinite
        res = 0x80000000;
        BID_RETURN (res);
      }
      // else cases that can be rounded to a 32-bit int fall through
      // to '1 <= q + exp <= 10'
    }
  }
  // n is not too large to be converted to int32: -2^31 - 1/2 <= n < 2^31 - 1/2
  // Note: some of the cases tested for above fall through to this point
  if ((q + exp) < 0) {	// n = +/-0.0...c(0)c(1)...c(q-1)
    // return 0
    res = 0x00000000;
    BID_RETURN (res);
  } else if ((q + exp) == 0) {	// n = +/-0.c(0)c(1)...c(q-1)
    // if 0.c(0)c(1)...c(q-1) <= 0.5 <=> c(0)c(1)...c(q-1) <= 5 * 10^(q-1)
    //   res = 0
    // else
    //   res = +/-1
    ind = q - 1;
    if ((BID_UINT64)C1 <= bid_midpoint64[ind]) {
      res = 0x00000000;	// return 0
    } else if (x_sign) {	// n < 0
      res = 0xffffffff;	// return -1
    } else {	// n > 0
      res = 0x00000001;	// return +1
    }
  } else {	// if (1 <= q + exp <= 10, 1 <= q <= 7, -6 <= exp <= 9)
    // -2^31-1/2 <= x <= -1 or 1 <= x < 2^31-1/2 so x can be rounded
    // to nearest to a 32-bit signed integer
    if (exp < 0) {	// 2 <= q <= 7, -6 <= exp <= -1, 1 <= q + exp <= 10
      ind = -exp;	// 1 <= ind <= 6; ind is a synonym for 'x'
      // chop off ind digits from the lower part of C1
      // C1 = C1 + 1/2 * 10^ind where the result C1 fits in 32 bits
      C1 = C1 + (BID_UINT32)bid_midpoint64[ind - 1];
      // calculate C* and f*
      // C* is actually floor(C*) in this case
      // C* and f* need shifting and masking, as shown by
      // bid_shiftright128[] and bid_maskhigh128[]
      // 1 <= x <= 6 
      // kx = 10^(-x) = bid_ten2mk64[ind - 1]
      // C* = (C1 + 1/2 * 10^x) * 10^(-x)
      // the approximation of 10^(-x) was rounded up to 24 bits
      __mul_64x64_to_128MACH (P128, (BID_UINT64)C1, bid_ten2mk64[ind - 1]);
      Cstar = P128.w[1];
      fstar.w[1] = P128.w[1] & bid_maskhigh128[ind - 1];
      fstar.w[0] = P128.w[0];
      // the top Ex bits of 10^(-x) are T* = bid_ten2mk128trunc[ind].w[0], e.g.
      // if x=1, T*=bid_ten2mk128trunc[0].w[0]=0x1999999999999999
      // if (0 < f* < 10^(-x)) then the result is a midpoint
      //   if floor(C*) is even then C* = floor(C*) - logical right
      //       shift; C* has p decimal digits, correct by Prop. 1)
      //   else if floor(C*) is odd C* = floor(C*)-1 (logical right
      //       shift; C* has p decimal digits, correct by Pr. 1)
      // else
      //   C* = floor(C*) (logical right shift; C has p decimal digits,
      //       correct by Property 1)
      // n = C* * 10^(e+x)

      // shift right C* by Ex-64 = bid_shiftright128[ind]
      shift = bid_shiftright128[ind - 1];	// 0 <= shift <= 39
      Cstar = Cstar >> shift;

      // if the result was a midpoint it was rounded away from zero, so
      // it will need a correction
      // check for midpoints
      if ((fstar.w[1] == 0) && fstar.w[0]
	  && (fstar.w[0] <= bid_ten2mk128trunc[ind - 1].w[1])) {
	// bid_ten2mk128trunc[ind -1].w[1] is identical to 
	// bid_ten2mk128[ind -1].w[1]
	// the result is a midpoint; round to nearest
	if (Cstar & 0x01) {	// Cstar is odd; MP in [EVEN, ODD]
	  // if floor(C*) is odd C = floor(C*) - 1; the result >= 1
	  Cstar--;	// Cstar is now even
	}	// else MP in [ODD, EVEN]
      }
      if (x_sign)
	res = -Cstar;
      else
	res = Cstar;
    } else if (exp == 0) {
      // 1 <= q <= 10
      // res = +/-C (exact)
      if (x_sign)
	res = -C1;
      else
	res = C1;
    } else {	// if (exp > 0) => 1 <= exp <= 9, 1 <= q < 9, 2 <= q + exp <= 10
      // res = +/-C * 10^exp (exact)
      if (x_sign)
	res = -C1 * bid_ten2k64[exp];
      else
	res = C1 * bid_ten2k64[exp];
    }
  }
  BID_RETURN (res);
}

/*****************************************************************************
 *  BID32_to_int32_xrnint
 ****************************************************************************/

#if DECIMAL_CALL_BY_REFERENCE
void
bid32_to_int32_xrnint (int *pres,
		       BID_UINT32*
		       px _EXC_FLAGS_PARAM _EXC_MASKS_PARAM
		       _EXC_INFO_PARAM) {
  BID_UINT32 x = *px;
#else
RES_WRAPFN_DFP(int, bid32_to_int32_xrnint, 32)
int
bid32_to_int32_xrnint (BID_UINT32 x _EXC_FLAGS_PARAM _EXC_MASKS_PARAM
		       _EXC_INFO_PARAM) {
#endif
  int res;
 BID_UINT32 x_sign;
 BID_UINT32 x_exp;
  int exp; // unbiased exponent
  // Note: C1 represents x_significand (BID_UINT32)
  BID_UINT64 tmp64;
  BID_UI32FLOAT tmp1;
  unsigned int x_nr_bits;
  int q, ind, shift;
  BID_UINT32 C1;
  BID_UINT64 Cstar;	// C* represents up to 7 decimal digits ~ 24 bits
  BID_UINT128 fstar;
  BID_UINT128 P128;

  // check for NaN or Infinity
  if ((x & MASK_NAN32) == MASK_NAN32 || (x & MASK_INF32) == MASK_INF32) {
    // set invalid flag
    *pfpsf |= BID_INVALID_EXCEPTION;
    // return Integer Indefinite
    res = 0x80000000;
    BID_RETURN (res);
  }
  // unpack x
  x_sign = x & MASK_SIGN32;	// 0 for positive, MASK_SIGN32 for negative
  // if steering bits are 11 (condition will be 0), then exponent is G[0:w+1] =>
  if ((x & MASK_STEERING_BITS32) == MASK_STEERING_BITS32) {
    x_exp = (x & MASK_BINARY_EXPONENT2_32) >> 21;	// biased
    C1 = (x & MASK_BINARY_SIG2_32) | MASK_BINARY_OR2_32;
    if (C1 > 9999999) {	// non-canonical
      x_exp = 0;
      C1 = 0;
    }
  } else {
    x_exp = (x & MASK_BINARY_EXPONENT1_32) >> 23;	// biased
    C1 = x & MASK_BINARY_SIG1_32;
  }

  // check for zeros (possibly from non-canonical values)
  if (C1 == 0x0) {
    // x is 0
    res = 0x00000000;
    BID_RETURN (res);
  }
  // x is not special and is not zero

  // q = nr. of decimal digits in x (1 <= q <= 7)
  //  determine first the nr. of bits in x
  tmp1.f = (float) C1; // exact conversion
  x_nr_bits = 1 + ((tmp1.ui32 >> 23) & 0xff) - 0x7f;
  q = bid_nr_digits[x_nr_bits - 1].digits;
  if (q == 0) {
    q = bid_nr_digits[x_nr_bits - 1].digits1;
    if (C1 >= bid_nr_digits[x_nr_bits - 1].threshold_lo)
      q++;
  }
  exp = x_exp - 101;	// unbiased exponent

  if ((q + exp) > 10) {	// x >= 10^10 ~= 2^33.2... (cannot fit in 32 bits)
    // set invalid flag
    *pfpsf |= BID_INVALID_EXCEPTION;
    // return Integer Indefinite
    res = 0x80000000;
    BID_RETURN (res);
  } else if ((q + exp) == 10) {	// x = c(0)c(1)...c(q-1)00..0 (10 dec. digits)
    // in this case 2^29.89... ~= 10^9 <= x < 10^10 ~= 2^33.2...
    // so x rounded to an integer may or may not fit in a signed 32-bit int
    // the cases that do not fit are identified here; the ones that fit
    // fall through and will be handled with other cases further,
    // under '1 <= q + exp <= 10'
    if (x_sign) {	// if n < 0 and q + exp = 10
      // if n < -2^31 - 1/2 then n is too large
      // too large if c(0)c(1)...c(q-1)00...0 > 2^31+1/2
      // <=> C * 10^(11-q) > 0x500000005, 1<=q<=7
      // Note: C * 10^(11-q) has 10 or 11 digits; 0x500000005 has 11 digits
      tmp64 = C1 * bid_ten2k64[11 - q];	// C scaled up to 11-digit int
      // c(0)c(1)...c(q-1)0...0 (11 digits)
      if (tmp64 > 0x500000005ull) {
        // set invalid flag
        *pfpsf |= BID_INVALID_EXCEPTION;
        // return Integer Indefinite
        res = 0x80000000;
        BID_RETURN (res);
      }
      // else cases that can be rounded to a 32-bit int fall through
      // to '1 <= q + exp <= 10'
    } else {	// if n > 0 and q + exp = 10
      // if n >= 2^31 - 1/2 then n is too large
      // too large if c(0)c(1)...c(q-1)00...0 >= 2^31-1/2
      // <=> C * 10^(11-q) >= 0x4fffffffb, 1<=q<=7
      // Note: C * 10^(11-q) has 10 or 11 digits; 0x500000005 has 11 digits
      tmp64 = C1 * bid_ten2k64[11 - q];	// C scaled up to 11-digit int
      // c(0)c(1)...c(q-1)0...0 (11 digits)
      if (tmp64 >= 0x4fffffffbull) {
        // set invalid flag
        *pfpsf |= BID_INVALID_EXCEPTION;
        // return Integer Indefinite
        res = 0x80000000;
        BID_RETURN (res);
      }
      // else cases that can be rounded to a 32-bit int fall through
      // to '1 <= q + exp <= 10'
    }
  }
  // n is not too large to be converted to int32: -2^31 - 1/2 < n < 2^31 - 1/2
  // Note: some of the cases tested for above fall through to this point
  if ((q + exp) < 0) {	// n = +/-0.0...c(0)c(1)...c(q-1)
    // set inexact flag
    *pfpsf |= BID_INEXACT_EXCEPTION;
    // return 0
    res = 0x00000000;
    BID_RETURN (res);
  } else if ((q + exp) == 0) {	// n = +/-0.c(0)c(1)...c(q-1)
    // if 0.c(0)c(1)...c(q-1) <= 0.5 <=> c(0)c(1)...c(q-1) <= 5 * 10^(q-1)
    //   res = 0
    // else
    //   res = +/-1
    ind = q - 1;
    if ((BID_UINT64)C1 <= bid_midpoint64[ind]) {
      res = 0x00000000;	// return 0
    } else if (x_sign) {	// n < 0
      res = 0xffffffff;	// return -1
    } else {	// n > 0
      res = 0x00000001;	// return +1
    }
    // set inexact flag
    *pfpsf |= BID_INEXACT_EXCEPTION;
  } else {	// if (1 <= q + exp <= 10, 1 <= q <= 7, -6 <= exp <= 9)
    // -2^31-1/2 <= x <= -1 or 1 <= x < 2^31-1/2 so x can be rounded
    // to nearest to a 32-bit signed integer
    if (exp < 0) {	// 2 <= q <= 7, -6 <= exp <= -1, 1 <= q + exp <= 10
      ind = -exp;	// 1 <= ind <= 6; ind is a synonym for 'x'
      // chop off ind digits from the lower part of C1
      // C1 = C1 + 1/2 * 10^ind where the result C1 fits in 32 bits
      C1 = C1 + (BID_UINT32)bid_midpoint64[ind - 1];
      // calculate C* and f*
      // C* is actually floor(C*) in this case
      // C* and f* need shifting and masking, as shown by
      // bid_shiftright128[] and bid_maskhigh128[]
      // 1 <= x <= 6 
      // kx = 10^(-x) = bid_ten2mk64[ind - 1]
      // C* = (C1 + 1/2 * 10^x) * 10^(-x)
      // the approximation of 10^(-x) was rounded up to 24 bits
      __mul_64x64_to_128MACH (P128, (BID_UINT64)C1, bid_ten2mk64[ind - 1]);
      Cstar = P128.w[1];
      fstar.w[1] = P128.w[1] & bid_maskhigh128[ind - 1];
      fstar.w[0] = P128.w[0];
      // the top Ex bits of 10^(-x) are T* = bid_ten2mk128trunc[ind].w[0], e.g.
      // if x=1, T*=bid_ten2mk128trunc[0].w[0]=0x1999999999999999
      // if (0 < f* < 10^(-x)) then the result is a midpoint
      //   if floor(C*) is even then C* = floor(C*) - logical right
      //       shift; C* has p decimal digits, correct by Prop. 1)
      //   else if floor(C*) is odd C* = floor(C*)-1 (logical right
      //       shift; C* has p decimal digits, correct by Pr. 1)
      // else
      //   C* = floor(C*) (logical right shift; C has p decimal digits,
      //       correct by Property 1)
      // n = C* * 10^(e+x)

      // shift right C* by Ex-64 = bid_shiftright128[ind]
      shift = bid_shiftright128[ind - 1];	// 0 <= shift <= 39
      Cstar = Cstar >> shift;
      // determine inexactness of the rounding of C*
      // if (0 < f* - 1/2 < 10^(-x)) then
      //   the result is exact
      // else // if (f* - 1/2 > T*) then
      //   the result is inexact
      if (ind - 1 <= 2) {
	if (fstar.w[0] > 0x8000000000000000ull) {
	  // f* > 1/2 and the result may be exact
	  tmp64 = fstar.w[0] - 0x8000000000000000ull;	// f* - 1/2
	  if ((tmp64 > bid_ten2mk128trunc[ind - 1].w[1])) {
	    // bid_ten2mk128trunc[ind -1].w[1] is identical to 
	    // bid_ten2mk128[ind -1].w[1]
	    // set the inexact flag
	    *pfpsf |= BID_INEXACT_EXCEPTION;
	  }	// else the result is exact
	} else {	// the result is inexact; f2* <= 1/2
	  // set the inexact flag
	  *pfpsf |= BID_INEXACT_EXCEPTION;
	}
      } else {	// if 3 <= ind - 1 <= 14
	if (fstar.w[1] > bid_onehalf128[ind - 1] ||
	    (fstar.w[1] == bid_onehalf128[ind - 1] && fstar.w[0])) {
	  // f2* > 1/2 and the result may be exact
	  // Calculate f2* - 1/2
	  tmp64 = fstar.w[1] - bid_onehalf128[ind - 1];
	  if (tmp64 || fstar.w[0] > bid_ten2mk128trunc[ind - 1].w[1]) {
	    // bid_ten2mk128trunc[ind -1].w[1] is identical to 
	    // bid_ten2mk128[ind -1].w[1]
	    // set the inexact flag
	    *pfpsf |= BID_INEXACT_EXCEPTION;
	  }	// else the result is exact
	} else {	// the result is inexact; f2* <= 1/2
	  // set the inexact flag
	  *pfpsf |= BID_INEXACT_EXCEPTION;
	}
      }

      // if the result was a midpoint it was rounded away from zero, so
      // it will need a correction
      // check for midpoints
      if ((fstar.w[1] == 0) && fstar.w[0]
	  && (fstar.w[0] <= bid_ten2mk128trunc[ind - 1].w[1])) {
	// bid_ten2mk128trunc[ind -1].w[1] is identical to 
	// bid_ten2mk128[ind -1].w[1]
	// the result is a midpoint; round to nearest
	if (Cstar & 0x01) {	// Cstar is odd; MP in [EVEN, ODD]
	  // if floor(C*) is odd C = floor(C*) - 1; the result >= 1
	  Cstar--;	// Cstar is now even
	}	// else MP in [ODD, EVEN]
      }
      if (x_sign)
	res = -Cstar;
      else
	res = Cstar;
    } else if (exp == 0) {
      // 1 <= q <= 10
      // res = +/-C (exact)
      if (x_sign)
	res = -C1;
      else
	res = C1;
    } else {	// if (exp > 0) => 1 <= exp <= 9, 1 <= q < 9, 2 <= q + exp <= 10
      // res = +/-C * 10^exp (exact)
      if (x_sign)
	res = -C1 * bid_ten2k64[exp];
      else
	res = C1 * bid_ten2k64[exp];
    }
  }
  BID_RETURN (res);
}

/*****************************************************************************
 *  BID32_to_int32_floor
 ****************************************************************************/

#if DECIMAL_CALL_BY_REFERENCE
void
bid32_to_int32_floor (int *pres,
		      BID_UINT32*
		      px _EXC_FLAGS_PARAM _EXC_MASKS_PARAM
		      _EXC_INFO_PARAM) {
  BID_UINT32 x = *px;
#else
RES_WRAPFN_DFP(int, bid32_to_int32_floor, 32)
int
bid32_to_int32_floor (BID_UINT32 x _EXC_FLAGS_PARAM _EXC_MASKS_PARAM
		      _EXC_INFO_PARAM) {
#endif
  int res;
 BID_UINT32 x_sign;
 BID_UINT32 x_exp;
  int exp; // unbiased exponent
  // Note: C1 represents x_significand (BID_UINT32)
  BID_UINT64 tmp64;
  BID_UI32FLOAT tmp1;
  unsigned int x_nr_bits;
  int q, ind, shift;
  BID_UINT32 C1;
  BID_UINT64 Cstar;	// C* represents up to 7 decimal digits ~ 24 bits
  BID_UINT128 fstar;
  BID_UINT128 P128;

  // check for NaN or Infinity
  if ((x & MASK_NAN32) == MASK_NAN32 || (x & MASK_INF32) == MASK_INF32) {
    // set invalid flag
    *pfpsf |= BID_INVALID_EXCEPTION;
    // return Integer Indefinite
    res = 0x80000000;
    BID_RETURN (res);
  }
  // unpack x
  x_sign = x & MASK_SIGN32;	// 0 for positive, MASK_SIGN32 for negative
  // if steering bits are 11 (condition will be 0), then exponent is G[0:w+1] =>
  if ((x & MASK_STEERING_BITS32) == MASK_STEERING_BITS32) {
    x_exp = (x & MASK_BINARY_EXPONENT2_32) >> 21;	// biased
    C1 = (x & MASK_BINARY_SIG2_32) | MASK_BINARY_OR2_32;
    if (C1 > 9999999) {	// non-canonical
      x_exp = 0;
      C1 = 0;
    }
  } else {
    x_exp = (x & MASK_BINARY_EXPONENT1_32) >> 23;	// biased
    C1 = x & MASK_BINARY_SIG1_32;
  }

  // check for zeros (possibly from non-canonical values)
  if (C1 == 0x0) {
    // x is 0
    res = 0x00000000;
    BID_RETURN (res);
  }
  // x is not special and is not zero

  // q = nr. of decimal digits in x (1 <= q <= 7)
  //  determine first the nr. of bits in x
  tmp1.f = (float) C1; // exact conversion
  x_nr_bits = 1 + ((tmp1.ui32 >> 23) & 0xff) - 0x7f;
  q = bid_nr_digits[x_nr_bits - 1].digits;
  if (q == 0) {
    q = bid_nr_digits[x_nr_bits - 1].digits1;
    if (C1 >= bid_nr_digits[x_nr_bits - 1].threshold_lo)
      q++;
  }
  exp = x_exp - 101;	// unbiased exponent

  if ((q + exp) > 10) {	// x >= 10^10 ~= 2^33.2... (cannot fit in 32 bits)
    // set invalid flag
    *pfpsf |= BID_INVALID_EXCEPTION;
    // return Integer Indefinite
    res = 0x80000000;
    BID_RETURN (res);
  } else if ((q + exp) == 10) {	// x = c(0)c(1)...c(q-1)00..0 (10 dec. digits)
    // in this case 2^29.89... ~= 10^9 <= x < 10^10 ~= 2^33.2...
    // so x rounded to an integer may or may not fit in a signed 32-bit int
    // the cases that do not fit are identified here; the ones that fit
    // fall through and will be handled with other cases further,
    // under '1 <= q + exp <= 10'
    if (x_sign) {	// if n < 0 and q + exp = 10
      // if n < -2^31 then n is too large
      // too large if c(0)c(1)...c(q-1)00...0 > 2^31
      // <=> C * 10^(11-q) >= 0x500000000, 1<=q<=7
      // Note: C * 10^(11-q) has 10 or 11 digits; 0x500000000 has 11 digits
      tmp64 = C1 * bid_ten2k64[11 - q];	// C scaled up to 11-digit int
      // c(0)c(1)...c(q-1)0...0 (11 digits)
      if (tmp64 > 0x500000000ull) {
        // set invalid flag
        *pfpsf |= BID_INVALID_EXCEPTION;
        // return Integer Indefinite
        res = 0x80000000;
        BID_RETURN (res);
      }
      // else cases that can be rounded to a 32-bit int fall through
      // to '1 <= q + exp <= 10'
    } else {	// if n > 0 and q + exp = 10
      // if n >= 2^31 then n is too large
      // too large if c(0)c(1)...c(q-1)00...0 >= 2^31
      // <=> C * 10^(11-q) >= 0x500000000, 1<=q<=7
      // Note: C * 10^(11-q) has 10 or 11 digits; 0x500000000 has 11 digits
      tmp64 = C1 * bid_ten2k64[11 - q];	// C scaled up to 11-digit int
      // c(0)c(1)...c(q-1)0...0 (11 digits)
      if (tmp64 >= 0x500000000ull) {
        // set invalid flag
        *pfpsf |= BID_INVALID_EXCEPTION;
        // return Integer Indefinite
        res = 0x80000000;
        BID_RETURN (res);
      }
      // else cases that can be rounded to a 32-bit int fall through
      // to '1 <= q + exp <= 10'
    }
  }
  // n is not too large to be converted to int32: -2^31 <= n < 2^31
  // Note: some of the cases tested for above fall through to this point
  if ((q + exp) <= 0) {	// n = +/-0.[0...0]c(0)c(1)...c(q-1)
    // return -1 or 0
    if (x_sign)
      res = 0xffffffff;
    else
      res = 0x00000000;
    BID_RETURN (res);
  } else {	// if (1 <= q + exp <= 10, 1 <= q <= 7, -6 <= exp <= 9)
    // -2^31-1 < x <= -1 or 1 <= x < 2^31 so x can be rounded
    // to nearest to a 32-bit signed integer
    if (exp < 0) {	// 2 <= q <= 7, -6 <= exp <= -1, 1 <= q + exp <= 10
      ind = -exp;	// 1 <= ind <= 6; ind is a synonym for 'x'
      // chop off ind digits from the lower part of C1
      // C1 fits in 32 bits
      // calculate C* and f*
      // C* is actually floor(C*) in this case
      // C* and f* need shifting and masking, as shown by
      // bid_shiftright128[] and bid_maskhigh128[]
      // 1 <= x <= 6 
      // kx = 10^(-x) = bid_ten2mk64[ind - 1]
      // C* = C1 * 10^(-x)
      // the approximation of 10^(-x) was rounded up to 24 bits
      __mul_64x64_to_128MACH (P128, (BID_UINT64)C1, bid_ten2mk64[ind - 1]);
      Cstar = P128.w[1];
      fstar.w[1] = P128.w[1] & bid_maskhigh128[ind - 1];
      fstar.w[0] = P128.w[0];
      // the top Ex bits of 10^(-x) are T* = bid_ten2mk128trunc[ind].w[0], e.g.
      // if x=1, T*=bid_ten2mk128trunc[0].w[0]=0x1999999999999999
      // C* = floor(C*) (logical right shift; C has p decimal digits,
      //     correct by Property 1)
      // n = C* * 10^(e+x)

      // shift right C* by Ex-64 = bid_shiftright128[ind]
      shift = bid_shiftright128[ind - 1];	// 0 <= shift <= 39
      Cstar = Cstar >> shift;
      // determine inexactness of the rounding of C*
      // if (0 < f* < 10^(-x)) then
      //   the result is exact
      // else // if (f* > T*) then
      //   the result is inexact
      if (ind - 1 <= 2) {
	if (fstar.w[0] > bid_ten2mk128trunc[ind - 1].w[1]) {
	  // bid_ten2mk128trunc[ind -1].w[1] is identical to
	  // bid_ten2mk128[ind -1].w[1]
	  if (x_sign) {	// negative and inexact
	    Cstar++;
	  }
	}	// else the result is exact
      } else {	// if 3 <= ind - 1 <= 14
	if (fstar.w[1] || fstar.w[0] > bid_ten2mk128trunc[ind - 1].w[1]) {
	  // bid_ten2mk128trunc[ind -1].w[1] is identical to
	  // bid_ten2mk128[ind -1].w[1]
	  if (x_sign) {	// negative and inexact
	    Cstar++;
	  }
	}	// else the result is exact
      }

      if (x_sign)
	res = -Cstar;
      else
	res = Cstar;
    } else if (exp == 0) {
      // 1 <= q <= 10
      // res = +/-C (exact)
      if (x_sign)
	res = -C1;
      else
	res = C1;
    } else {	// if (exp > 0) => 1 <= exp <= 9, 1 <= q < 9, 2 <= q + exp <= 10
      // res = +/-C * 10^exp (exact)
      if (x_sign)
	res = -C1 * bid_ten2k64[exp];
      else
	res = C1 * bid_ten2k64[exp];
    }
  }
  BID_RETURN (res);
}

/*****************************************************************************
 *  BID32_to_int32_xfloor
 ****************************************************************************/

#if DECIMAL_CALL_BY_REFERENCE
void
bid32_to_int32_xfloor (int *pres,
		       BID_UINT32*
		       px _EXC_FLAGS_PARAM _EXC_MASKS_PARAM
		       _EXC_INFO_PARAM) {
  BID_UINT32 x = *px;
#else
RES_WRAPFN_DFP(int, bid32_to_int32_xfloor, 32)
int
bid32_to_int32_xfloor (BID_UINT32 x _EXC_FLAGS_PARAM _EXC_MASKS_PARAM
		       _EXC_INFO_PARAM) {
#endif
  int res;
 BID_UINT32 x_sign;
 BID_UINT32 x_exp;
  int exp; // unbiased exponent
  // Note: C1 represents x_significand (BID_UINT32)
  BID_UINT64 tmp64;
  BID_UI32FLOAT tmp1;
  unsigned int x_nr_bits;
  int q, ind, shift;
  BID_UINT32 C1;
  BID_UINT64 Cstar; // C* represents up to 7 decimal digits ~ 24 bits
  BID_UINT128 fstar;
  BID_UINT128 P128;

  // check for NaN or Infinity
  if ((x & MASK_NAN32) == MASK_NAN32 || (x & MASK_INF32) == MASK_INF32) {
    // set invalid flag
    *pfpsf |= BID_INVALID_EXCEPTION;
    // return Integer Indefinite
    res = 0x80000000;
    BID_RETURN (res);
  }
  // unpack x
  x_sign = x & MASK_SIGN32;	// 0 for positive, MASK_SIGN32 for negative
  // if steering bits are 11 (condition will be 0), then exponent is G[0:w+1] =>
  if ((x & MASK_STEERING_BITS32) == MASK_STEERING_BITS32) {
    x_exp = (x & MASK_BINARY_EXPONENT2_32) >> 21;	// biased
    C1 = (x & MASK_BINARY_SIG2_32) | MASK_BINARY_OR2_32;
    if (C1 > 9999999) {	// non-canonical
      x_exp = 0;
      C1 = 0;
    }
  } else {
    x_exp = (x & MASK_BINARY_EXPONENT1_32) >> 23;	// biased
    C1 = x & MASK_BINARY_SIG1_32;
  }

  // check for zeros (possibly from non-canonical values)
  if (C1 == 0x0) {
    // x is 0
    res = 0x00000000;
    BID_RETURN (res);
  }
  // x is not special and is not zero

  // q = nr. of decimal digits in x (1 <= q <= 7)
  //  determine first the nr. of bits in x
  tmp1.f = (float) C1; // exact conversion
  x_nr_bits = 1 + ((tmp1.ui32 >> 23) & 0xff) - 0x7f;
  q = bid_nr_digits[x_nr_bits - 1].digits;
  if (q == 0) {
    q = bid_nr_digits[x_nr_bits - 1].digits1;
    if (C1 >= bid_nr_digits[x_nr_bits - 1].threshold_lo)
      q++;
  }
  exp = x_exp - 101;	// unbiased exponent

  if ((q + exp) > 10) {	// x >= 10^10 ~= 2^33.2... (cannot fit in 32 bits)
    // set invalid flag
    *pfpsf |= BID_INVALID_EXCEPTION;
    // return Integer Indefinite
    res = 0x80000000;
    BID_RETURN (res);
  } else if ((q + exp) == 10) {	// x = c(0)c(1)...c(q-1)00..0 (10 dec. digits)
    // in this case 2^29.89... ~= 10^9 <= x < 10^10 ~= 2^33.2...
    // so x rounded to an integer may or may not fit in a signed 32-bit int
    // the cases that do not fit are identified here; the ones that fit
    // fall through and will be handled with other cases further,
    // under '1 <= q + exp <= 10'
    if (x_sign) {	// if n < 0 and q + exp = 10
      // if n < -2^31 then n is too large
      // too large if c(0)c(1)...c(q-1)00...0 > 2^31
      // <=> C * 10^(11-q) >= 0x500000000, 1<=q<=7
      // Note: C * 10^(11-q) has 10 or 11 digits; 0x500000000 has 11 digits
      tmp64 = C1 * bid_ten2k64[11 - q];	// C scaled up to 11-digit int
      // c(0)c(1)...c(q-1)0...0 (11 digits)
      if (tmp64 > 0x500000000ull) {
        // set invalid flag
        *pfpsf |= BID_INVALID_EXCEPTION;
        // return Integer Indefinite
        res = 0x80000000;
        BID_RETURN (res);
      }
      // else cases that can be rounded to a 32-bit int fall through
      // to '1 <= q + exp <= 10'
    } else {	// if n > 0 and q + exp = 10
      // if n >= 2^31 then n is too large
      // too large if c(0)c(1)...c(q-1)00...0 >= 2^31
      // <=> C * 10^(11-q) >= 0x500000000, 1<=q<=7
      // Note: C * 10^(11-q) has 10 or 11 digits; 0x500000000 has 11 digits
      tmp64 = C1 * bid_ten2k64[11 - q];	// C scaled up to 11-digit int
      // c(0)c(1)...c(q-1)0...0 (11 digits)
      if (tmp64 >= 0x500000000ull) {
        // set invalid flag
        *pfpsf |= BID_INVALID_EXCEPTION;
        // return Integer Indefinite
        res = 0x80000000;
        BID_RETURN (res);
      }
      // else cases that can be rounded to a 32-bit int fall through
      // to '1 <= q + exp <= 10'
    }
  }
  // n is not too large to be converted to int32: -2^31 <= n < 2^31
  // Note: some of the cases tested for above fall through to this point
  if ((q + exp) <= 0) {	// n = +/-0.[0...0]c(0)c(1)...c(q-1)
    // set inexact flag
    *pfpsf |= BID_INEXACT_EXCEPTION;
    // return -1 or 0
    if (x_sign)
      res = 0xffffffff;
    else
      res = 0x00000000;
    BID_RETURN (res);
  } else {	// if (1 <= q + exp <= 10, 1 <= q <= 7, -6 <= exp <= 9)
    // -2^31-1 < x <= -1 or 1 <= x < 2^31 so x can be rounded
    // to nearest to a 32-bit signed integer
    if (exp < 0) {	// 2 <= q <= 7, -6 <= exp <= -1, 1 <= q + exp <= 10
      ind = -exp;	// 1 <= ind <= 6; ind is a synonym for 'x'
      // chop off ind digits from the lower part of C1
      // C1 fits in 32 bits
      // calculate C* and f*
      // C* is actually floor(C*) in this case
      // C* and f* need shifting and masking, as shown by
      // bid_shiftright128[] and bid_maskhigh128[]
      // 1 <= x <= 6 
      // kx = 10^(-x) = bid_ten2mk64[ind - 1]
      // C* = C1 * 10^(-x)
      // the approximation of 10^(-x) was rounded up to 24 bits
      __mul_64x64_to_128MACH (P128, (BID_UINT64)C1, bid_ten2mk64[ind - 1]);
      Cstar = P128.w[1];
      fstar.w[1] = P128.w[1] & bid_maskhigh128[ind - 1];
      fstar.w[0] = P128.w[0];
      // the top Ex bits of 10^(-x) are T* = bid_ten2mk128trunc[ind].w[0], e.g.
      // if x=1, T*=bid_ten2mk128trunc[0].w[0]=0x1999999999999999
      // C* = floor(C*) (logical right shift; C has p decimal digits,
      //     correct by Property 1)
      // n = C* * 10^(e+x)

      // shift right C* by Ex-64 = bid_shiftright128[ind]
      shift = bid_shiftright128[ind - 1];	// 0 <= shift <= 39
      Cstar = Cstar >> shift;
      // determine inexactness of the rounding of C*
      // if (0 < f* < 10^(-x)) then
      //   the result is exact
      // else // if (f* > T*) then
      //   the result is inexact
      if (ind - 1 <= 2) {
	if (fstar.w[0] > bid_ten2mk128trunc[ind - 1].w[1]) {
	  // bid_ten2mk128trunc[ind -1].w[1] is identical to
	  // bid_ten2mk128[ind -1].w[1]
	  if (x_sign) {	// negative and inexact
	    Cstar++;
	  }
	  // set the inexact flag
	  *pfpsf |= BID_INEXACT_EXCEPTION;
	}	// else the result is exact
      } else {	// if 3 <= ind - 1 <= 14
	if (fstar.w[1] || fstar.w[0] > bid_ten2mk128trunc[ind - 1].w[1]) {
	  // bid_ten2mk128trunc[ind -1].w[1] is identical to
	  // bid_ten2mk128[ind -1].w[1]
	  if (x_sign) {	// negative and inexact
	    Cstar++;
	  }
	  // set the inexact flag
	  *pfpsf |= BID_INEXACT_EXCEPTION;
	}	// else the result is exact
      }

      if (x_sign)
	res = -Cstar;
      else
	res = Cstar;
    } else if (exp == 0) {
      // 1 <= q <= 10
      // res = +/-C (exact)
      if (x_sign)
	res = -C1;
      else
	res = C1;
    } else {	// if (exp > 0) => 1 <= exp <= 9, 1 <= q < 9, 2 <= q + exp <= 10
      // res = +/-C * 10^exp (exact)
      if (x_sign)
	res = -C1 * bid_ten2k64[exp];
      else
	res = C1 * bid_ten2k64[exp];
    }
  }
  BID_RETURN (res);
}

/*****************************************************************************
 *  BID32_to_int32_ceil
 ****************************************************************************/

#if DECIMAL_CALL_BY_REFERENCE
void
bid32_to_int32_ceil (int *pres,
		     BID_UINT32*
		     px _EXC_FLAGS_PARAM _EXC_MASKS_PARAM
		     _EXC_INFO_PARAM) {
  BID_UINT32 x = *px;
#else
RES_WRAPFN_DFP(int, bid32_to_int32_ceil, 32)
int
bid32_to_int32_ceil (BID_UINT32 x _EXC_FLAGS_PARAM _EXC_MASKS_PARAM
		     _EXC_INFO_PARAM) {
#endif
  int res;
 BID_UINT32 x_sign;
 BID_UINT32 x_exp;
  int exp; // unbiased exponent
  // Note: C1 represents x_significand (BID_UINT32)
  BID_UINT64 tmp64;
  BID_UI32FLOAT tmp1;
  unsigned int x_nr_bits;
  int q, ind, shift;
  BID_UINT32 C1;
  BID_UINT64 Cstar;	// C* represents up to 7 decimal digits ~ 24 bits
  BID_UINT128 fstar;
  BID_UINT128 P128;

  // check for NaN or Infinity
  if ((x & MASK_NAN32) == MASK_NAN32 || (x & MASK_INF32) == MASK_INF32) {
    // set invalid flag
    *pfpsf |= BID_INVALID_EXCEPTION;
    // return Integer Indefinite
    res = 0x80000000;
    BID_RETURN (res);
  }
  // unpack x
  x_sign = x & MASK_SIGN32;	// 0 for positive, MASK_SIGN32 for negative
  // if steering bits are 11 (condition will be 0), then exponent is G[0:w+1] =>
  if ((x & MASK_STEERING_BITS32) == MASK_STEERING_BITS32) {
    x_exp = (x & MASK_BINARY_EXPONENT2_32) >> 21;	// biased
    C1 = (x & MASK_BINARY_SIG2_32) | MASK_BINARY_OR2_32;
    if (C1 > 9999999) {	// non-canonical
      x_exp = 0;
      C1 = 0;
    }
  } else {
    x_exp = (x & MASK_BINARY_EXPONENT1_32) >> 23;	// biased
    C1 = x & MASK_BINARY_SIG1_32;
  }

  // check for zeros (possibly from non-canonical values)
  if (C1 == 0x0) {
    // x is 0
    res = 0x00000000;
    BID_RETURN (res);
  }
  // x is not special and is not zero

  // q = nr. of decimal digits in x (1 <= q <= 7)
  //  determine first the nr. of bits in x
  tmp1.f = (float) C1; // exact conversion
  x_nr_bits = 1 + ((tmp1.ui32 >> 23) & 0xff) - 0x7f;
  q = bid_nr_digits[x_nr_bits - 1].digits;
  if (q == 0) {
    q = bid_nr_digits[x_nr_bits - 1].digits1;
    if (C1 >= bid_nr_digits[x_nr_bits - 1].threshold_lo)
      q++;
  }
  exp = x_exp - 101;	// unbiased exponent

  if ((q + exp) > 10) {	// x >= 10^10 ~= 2^33.2... (cannot fit in 32 bits)
    // set invalid flag
    *pfpsf |= BID_INVALID_EXCEPTION;
    // return Integer Indefinite
    res = 0x80000000;
    BID_RETURN (res);
  } else if ((q + exp) == 10) {	// x = c(0)c(1)...c(q-1)00..0 (10 dec. digits)
    // in this case 2^29.89... ~= 10^9 <= x < 10^10 ~= 2^33.2...
    // so x rounded to an integer may or may not fit in a signed 32-bit int
    // the cases that do not fit are identified here; the ones that fit
    // fall through and will be handled with other cases further,
    // under '1 <= q + exp <= 10'
    if (x_sign) {	// if n < 0 and q + exp = 10
      // if n <= -2^31 - 1 then n is too large
      // too large if c(0)c(1)...c(q-1)00...0 >= 2^31+1
      // <=> C * 10^(11-q) >= 0x50000000a, 1<=q<=7
      // Note: C * 10^(11-q) has 10 or 11 digits; 0x50000000a has 11 digits
      tmp64 = C1 * bid_ten2k64[11 - q];	// C scaled up to 11-digit int
      // c(0)c(1)...c(q-1)0...0 (11 digits)
      if (tmp64 >= 0x50000000aull) {
        // set invalid flag
        *pfpsf |= BID_INVALID_EXCEPTION;
        // return Integer Indefinite
        res = 0x80000000;
        BID_RETURN (res);
      }
      // else cases that can be rounded to a 32-bit int fall through
      // to '1 <= q + exp <= 10'
    } else {	// if n > 0 and q + exp = 10
      // if n > 2^31 - 1 then n is too large
      // too large if c(0)c(1)...c(q-1)00...0 > 2^31 - 1
      // <=> C * 10^(11-q) > 0x4fffffff6, 1<=q<=7
      // Note: C * 10^(11-q) has 10 or 11 digits; 0x4fffffff6 has 11 digits
      tmp64 = C1 * bid_ten2k64[11 - q];	// C scaled up to 11-digit int
      // c(0)c(1)...c(q-1)0...0 (11 digits)
      if (tmp64 > 0x4fffffff6ull) {
        // set invalid flag
        *pfpsf |= BID_INVALID_EXCEPTION;
        // return Integer Indefinite
        res = 0x80000000;
        BID_RETURN (res);
      }
      // else cases that can be rounded to a 32-bit int fall through
      // to '1 <= q + exp <= 10'
    }
  }
  // n is not too large to be converted to int32: -2^31 - 1 < n <= 2^31 - 1
  // Note: some of the cases tested for above fall through to this point
  if ((q + exp) <= 0) {	// n = +/-0.[0...0]c(0)c(1)...c(q-1)
    // return 0 or 1
    if (x_sign)
      res = 0x00000000;
    else
      res = 0x00000001;
    BID_RETURN (res);
  } else {	// if (1 <= q + exp <= 10, 1 <= q <= 7, -6 <= exp <= 9)
    // -2^31-1 < x <= -1 or 1 <= x <= 2^31-1 so x can be rounded
    // to nearest to a 32-bit signed integer
    if (exp < 0) {	// 2 <= q <= 7, -6 <= exp <= -1, 1 <= q + exp <= 10
      ind = -exp;	// 1 <= ind <= 6; ind is a synonym for 'x'
      // chop off ind digits from the lower part of C1
      // C1 fits in 32 bits
      // calculate C* and f*
      // C* is actually floor(C*) in this case
      // C* and f* need shifting and masking, as shown by
      // bid_shiftright128[] and bid_maskhigh128[]
      // 1 <= x <= 6 
      // kx = 10^(-x) = bid_ten2mk64[ind - 1]
      // C* = C1 * 10^(-x)
      // the approximation of 10^(-x) was rounded up to 24 bits
      __mul_64x64_to_128MACH (P128, (BID_UINT64)C1, bid_ten2mk64[ind - 1]);
      Cstar = P128.w[1];
      fstar.w[1] = P128.w[1] & bid_maskhigh128[ind - 1];
      fstar.w[0] = P128.w[0];
      // the top Ex bits of 10^(-x) are T* = bid_ten2mk128trunc[ind].w[0], e.g.
      // if x=1, T*=bid_ten2mk128trunc[0].w[0]=0x1999999999999999
      // C* = floor(C*) (logical right shift; C has p decimal digits,
      //     correct by Property 1)
      // n = C* * 10^(e+x)

      // shift right C* by Ex-64 = bid_shiftright128[ind]
      shift = bid_shiftright128[ind - 1];	// 0 <= shift <= 39
      Cstar = Cstar >> shift;
      // determine inexactness of the rounding of C*
      // if (0 < f* < 10^(-x)) then
      //   the result is exact
      // else // if (f* > T*) then
      //   the result is inexact
      if (ind - 1 <= 2) {
	if (fstar.w[0] > bid_ten2mk128trunc[ind - 1].w[1]) {
	  // bid_ten2mk128trunc[ind -1].w[1] is identical to
	  // bid_ten2mk128[ind -1].w[1]
	  if (!x_sign) {	// positive and inexact
	    Cstar++;
	  }
	}	// else the result is exact
      } else {	// if 3 <= ind - 1 <= 14
	if (fstar.w[1] || fstar.w[0] > bid_ten2mk128trunc[ind - 1].w[1]) {
	  // bid_ten2mk128trunc[ind -1].w[1] is identical to
	  // bid_ten2mk128[ind -1].w[1]
	  if (!x_sign) {	// positive and inexact 
	    Cstar++;
	  }
	}	// else the result is exact
      }

      if (x_sign)
	res = -Cstar;
      else
	res = Cstar;
    } else if (exp == 0) {
      // 1 <= q <= 10
      // res = +/-C (exact)
      if (x_sign)
	res = -C1;
      else
	res = C1;
    } else {	// if (exp > 0) => 1 <= exp <= 9, 1 <= q < 9, 2 <= q + exp <= 10
      // res = +/-C * 10^exp (exact)
      if (x_sign)
	res = -C1 * bid_ten2k64[exp];
      else
	res = C1 * bid_ten2k64[exp];
    }
  }
  BID_RETURN (res);
}

/*****************************************************************************
 *  BID32_to_int32_xceil
 ****************************************************************************/

#if DECIMAL_CALL_BY_REFERENCE
void
bid32_to_int32_xceil (int *pres,
		      BID_UINT32*
		      px _EXC_FLAGS_PARAM _EXC_MASKS_PARAM
		      _EXC_INFO_PARAM) {
  BID_UINT32 x = *px;
#else
RES_WRAPFN_DFP(int, bid32_to_int32_xceil, 32)
int
bid32_to_int32_xceil (BID_UINT32 x _EXC_FLAGS_PARAM _EXC_MASKS_PARAM
		      _EXC_INFO_PARAM) {
#endif
  int res;
 BID_UINT32 x_sign;
 BID_UINT32 x_exp;
  int exp; // unbiased exponent
  // Note: C1 represents x_significand (BID_UINT32)
  BID_UINT64 tmp64;
  BID_UI32FLOAT tmp1;
  unsigned int x_nr_bits;
  int q, ind, shift;
  BID_UINT32 C1;
  BID_UINT64 Cstar;	// C* represents up to 7 decimal digits ~ 24 bits
  BID_UINT128 fstar;
  BID_UINT128 P128;

  // check for NaN or Infinity
  if ((x & MASK_NAN32) == MASK_NAN32 || (x & MASK_INF32) == MASK_INF32) {
    // set invalid flag
    *pfpsf |= BID_INVALID_EXCEPTION;
    // return Integer Indefinite
    res = 0x80000000;
    BID_RETURN (res);
  }
  // unpack x
  x_sign = x & MASK_SIGN32;	// 0 for positive, MASK_SIGN32 for negative
  // if steering bits are 11 (condition will be 0), then exponent is G[0:w+1] =>
  if ((x & MASK_STEERING_BITS32) == MASK_STEERING_BITS32) {
    x_exp = (x & MASK_BINARY_EXPONENT2_32) >> 21;	// biased
    C1 = (x & MASK_BINARY_SIG2_32) | MASK_BINARY_OR2_32;
    if (C1 > 9999999) {	// non-canonical
      x_exp = 0;
      C1 = 0;
    }
  } else {
    x_exp = (x & MASK_BINARY_EXPONENT1_32) >> 23;	// biased
    C1 = x & MASK_BINARY_SIG1_32;
  }

  // check for zeros (possibly from non-canonical values)
  if (C1 == 0x0) {
    // x is 0
    res = 0x00000000;
    BID_RETURN (res);
  }
  // x is not special and is not zero

  // q = nr. of decimal digits in x (1 <= q <= 7)
  //  determine first the nr. of bits in x
  tmp1.f = (float) C1; // exact conversion
  x_nr_bits = 1 + ((tmp1.ui32 >> 23) & 0xff) - 0x7f;
  q = bid_nr_digits[x_nr_bits - 1].digits;
  if (q == 0) {
    q = bid_nr_digits[x_nr_bits - 1].digits1;
    if (C1 >= bid_nr_digits[x_nr_bits - 1].threshold_lo)
      q++;
  }
  exp = x_exp - 101;	// unbiased exponent

  if ((q + exp) > 10) {	// x >= 10^10 ~= 2^33.2... (cannot fit in 32 bits)
    // set invalid flag
    *pfpsf |= BID_INVALID_EXCEPTION;
    // return Integer Indefinite
    res = 0x80000000;
    BID_RETURN (res);
  } else if ((q + exp) == 10) {	// x = c(0)c(1)...c(q-1)00..0 (10 dec. digits)
    // in this case 2^29.89... ~= 10^9 <= x < 10^10 ~= 2^33.2...
    // so x rounded to an integer may or may not fit in a signed 32-bit int
    // the cases that do not fit are identified here; the ones that fit
    // fall through and will be handled with other cases further,
    // under '1 <= q + exp <= 10'
    if (x_sign) {	// if n < 0 and q + exp = 10
      // if n <= -2^31 - 1 then n is too large
      // too large if c(0)c(1)...c(q-1)00...0 >= 2^31+1
      // <=> C * 10^(11-q) >= 0x50000000a, 1<=q<=7
      // Note: C * 10^(11-q) has 10 or 11 digits; 0x50000000a has 11 digits
      tmp64 = C1 * bid_ten2k64[11 - q];	// C scaled up to 11-digit int
      // c(0)c(1)...c(q-1)0...0 (11 digits)
      if (tmp64 >= 0x50000000aull) {
        // set invalid flag
        *pfpsf |= BID_INVALID_EXCEPTION;
        // return Integer Indefinite
        res = 0x80000000;
        BID_RETURN (res);
      }
      // else cases that can be rounded to a 32-bit int fall through
      // to '1 <= q + exp <= 10'
    } else {	// if n > 0 and q + exp = 10
      // if n > 2^31 - 1 then n is too large
      // too large if c(0)c(1)...c(q-1)00...0 > 2^31 - 1
      // <=> C * 10^(11-q) > 0x4fffffff6, 1<=q<=7
      // Note: C * 10^(11-q) has 10 or 11 digits; 0x4fffffff6 has 11 digits
      tmp64 = C1 * bid_ten2k64[11 - q];	// C scaled up to 11-digit int
      // c(0)c(1)...c(q-1)0...0 (11 digits)
      if (tmp64 > 0x4fffffff6ull) {
        // set invalid flag
        *pfpsf |= BID_INVALID_EXCEPTION;
        // return Integer Indefinite
        res = 0x80000000;
        BID_RETURN (res);
      }
      // else cases that can be rounded to a 32-bit int fall through
      // to '1 <= q + exp <= 10'
    }
  }
  // n is not too large to be converted to int32: -2^31 - 1 < n <= 2^31 - 1
  // Note: some of the cases tested for above fall through to this point
  if ((q + exp) <= 0) {	// n = +/-0.[0...0]c(0)c(1)...c(q-1)
    // set inexact flag
    *pfpsf |= BID_INEXACT_EXCEPTION;
    // return 0 or 1
    if (x_sign)
      res = 0x00000000;
    else
      res = 0x00000001;
    BID_RETURN (res);
  } else {	// if (1 <= q + exp <= 10, 1 <= q <= 7, -6 <= exp <= 9)
    // -2^31-1 < x <= -1 or 1 <= x <= 2^31-1 so x can be rounded
    // to nearest to a 32-bit signed integer
    if (exp < 0) {	// 2 <= q <= 7, -6 <= exp <= -1, 1 <= q + exp <= 10
      ind = -exp;	// 1 <= ind <= 6; ind is a synonym for 'x'
      // chop off ind digits from the lower part of C1
      // C1 fits in 32 bits
      // calculate C* and f*
      // C* is actually floor(C*) in this case
      // C* and f* need shifting and masking, as shown by
      // bid_shiftright128[] and bid_maskhigh128[]
      // 1 <= x <= 6 
      // kx = 10^(-x) = bid_ten2mk64[ind - 1]
      // C* = C1 * 10^(-x)
      // the approximation of 10^(-x) was rounded up to 24 bits
      __mul_64x64_to_128MACH (P128, (BID_UINT64)C1, bid_ten2mk64[ind - 1]);
      Cstar = P128.w[1];
      fstar.w[1] = P128.w[1] & bid_maskhigh128[ind - 1];
      fstar.w[0] = P128.w[0];
      // the top Ex bits of 10^(-x) are T* = bid_ten2mk128trunc[ind].w[0], e.g.
      // if x=1, T*=bid_ten2mk128trunc[0].w[0]=0x1999999999999999
      // C* = floor(C*) (logical right shift; C has p decimal digits,
      //     correct by Property 1)
      // n = C* * 10^(e+x)

      // shift right C* by Ex-64 = bid_shiftright128[ind]
      shift = bid_shiftright128[ind - 1];	// 0 <= shift <= 39
      Cstar = Cstar >> shift;
      // determine inexactness of the rounding of C*
      // if (0 < f* < 10^(-x)) then
      //   the result is exact
      // else // if (f* > T*) then
      //   the result is inexact
      if (ind - 1 <= 2) {
	if (fstar.w[0] > bid_ten2mk128trunc[ind - 1].w[1]) {
	  // bid_ten2mk128trunc[ind -1].w[1] is identical to
	  // bid_ten2mk128[ind -1].w[1]
	  if (!x_sign) {	// positive and inexact
	    Cstar++;
	  }
	  // set the inexact flag
	  *pfpsf |= BID_INEXACT_EXCEPTION;
	}	// else the result is exact
      } else {	// if 3 <= ind - 1 <= 14
	if (fstar.w[1] || fstar.w[0] > bid_ten2mk128trunc[ind - 1].w[1]) {
	  // bid_ten2mk128trunc[ind -1].w[1] is identical to
	  // bid_ten2mk128[ind -1].w[1]
	  if (!x_sign) {	// positive and inexact 
	    Cstar++;
	  }
	  // set the inexact flag
	  *pfpsf |= BID_INEXACT_EXCEPTION;
	}	// else the result is exact
      }

      if (x_sign)
	res = -Cstar;
      else
	res = Cstar;
    } else if (exp == 0) {
      // 1 <= q <= 10
      // res = +/-C (exact)
      if (x_sign)
	res = -C1;
      else
	res = C1;
    } else {	// if (exp > 0) => 1 <= exp <= 9, 1 <= q < 9, 2 <= q + exp <= 10
      // res = +/-C * 10^exp (exact)
      if (x_sign)
	res = -C1 * bid_ten2k64[exp];
      else
	res = C1 * bid_ten2k64[exp];
    }
  }
  BID_RETURN (res);
}

/*****************************************************************************
 *  BID32_to_int32_int
 ****************************************************************************/

#if DECIMAL_CALL_BY_REFERENCE
void
bid32_to_int32_int (int *pres,
		    BID_UINT32*
		    px _EXC_FLAGS_PARAM _EXC_MASKS_PARAM
		    _EXC_INFO_PARAM) {
  BID_UINT32 x = *px;
#else
RES_WRAPFN_DFP(int, bid32_to_int32_int, 32)
int
bid32_to_int32_int (BID_UINT32 x _EXC_FLAGS_PARAM _EXC_MASKS_PARAM
		    _EXC_INFO_PARAM) {
#endif
  int res;
 BID_UINT32 x_sign;
 BID_UINT32 x_exp;
  int exp; // unbiased exponent
  // Note: C1 represents x_significand (BID_UINT32)
  BID_UINT64 tmp64;
  BID_UI32FLOAT tmp1;
  unsigned int x_nr_bits;
  int q, ind, shift;
  BID_UINT32 C1;
  BID_UINT64 Cstar;	// C* represents up to 7 decimal digits ~ 24 bits
  BID_UINT128 P128;

  // check for NaN or Infinity
  if ((x & MASK_NAN32) == MASK_NAN32 || (x & MASK_INF32) == MASK_INF32) {
    // set invalid flag
    *pfpsf |= BID_INVALID_EXCEPTION;
    // return Integer Indefinite
    res = 0x80000000;
    BID_RETURN (res);
  }
  // unpack x
  x_sign = x & MASK_SIGN32;	// 0 for positive, MASK_SIGN32 for negative
  // if steering bits are 11 (condition will be 0), then exponent is G[0:w+1] =>
  if ((x & MASK_STEERING_BITS32) == MASK_STEERING_BITS32) {
    x_exp = (x & MASK_BINARY_EXPONENT2_32) >> 21;	// biased
    C1 = (x & MASK_BINARY_SIG2_32) | MASK_BINARY_OR2_32;
    if (C1 > 9999999) {	// non-canonical
      x_exp = 0;
      C1 = 0;
    }
  } else {
    x_exp = (x & MASK_BINARY_EXPONENT1_32) >> 23;	// biased
    C1 = x & MASK_BINARY_SIG1_32;
  }

  // check for zeros (possibly from non-canonical values)
  if (C1 == 0x0) {
    // x is 0
    res = 0x00000000;
    BID_RETURN (res);
  }
  // x is not special and is not zero

  // q = nr. of decimal digits in x (1 <= q <= 7)
  //  determine first the nr. of bits in x
  tmp1.f = (float) C1; // exact conversion
  x_nr_bits = 1 + ((tmp1.ui32 >> 23) & 0xff) - 0x7f;
  q = bid_nr_digits[x_nr_bits - 1].digits;
  if (q == 0) {
    q = bid_nr_digits[x_nr_bits - 1].digits1;
    if (C1 >= bid_nr_digits[x_nr_bits - 1].threshold_lo)
      q++;
  }
  exp = x_exp - 101;	// unbiased exponent

  if ((q + exp) > 10) {	// x >= 10^10 ~= 2^33.2... (cannot fit in 32 bits)
    // set invalid flag
    *pfpsf |= BID_INVALID_EXCEPTION;
    // return Integer Indefinite
    res = 0x80000000;
    BID_RETURN (res);
  } else if ((q + exp) == 10) {	// x = c(0)c(1)...c(q-1)00..0 (10 dec. digits)
    // in this case 2^29.89... ~= 10^9 <= x < 10^10 ~= 2^33.2...
    // so x rounded to an integer may or may not fit in a signed 32-bit int
    // the cases that do not fit are identified here; the ones that fit
    // fall through and will be handled with other cases further,
    // under '1 <= q + exp <= 10'
    if (x_sign) {	// if n < 0 and q + exp = 10
      // if n <= -2^31 - 1 then n is too large
      // too large if c(0)c(1)...c(q-1)00...0 >= 2^31+1
      // <=> C * 10^(11-q) >= 0x50000000a, 1<=q<=7
      // Note: C * 10^(11-q) has 10 or 11 digits; 0x50000000a has 11 digits
      tmp64 = C1 * bid_ten2k64[11 - q];	// C scaled up to 11-digit int
      // c(0)c(1)...c(q-1)0...0 (11 digits)
      if (tmp64 >= 0x50000000aull) {
        // set invalid flag
        *pfpsf |= BID_INVALID_EXCEPTION;
        // return Integer Indefinite
        res = 0x80000000;
        BID_RETURN (res);
      }
      // else cases that can be rounded to a 32-bit int fall through
      // to '1 <= q + exp <= 10'
    } else {	// if n > 0 and q + exp = 10
      // if n >= 2^31 then n is too large
      // too large if c(0)c(1)...c(q-1)00...0 >= 2^31
      // <=> C * 10^(11-q) >= 0x500000000, 1<=q<=7
      // Note: C * 10^(11-q) has 10 or 11 digits; 0x500000000 has 11 digits
      tmp64 = C1 * bid_ten2k64[11 - q];	// C scaled up to 11-digit int
      // c(0)c(1)...c(q-1)0...0 (11 digits)
      if (tmp64 >= 0x500000000ull) {
        // set invalid flag
        *pfpsf |= BID_INVALID_EXCEPTION;
        // return Integer Indefinite
        res = 0x80000000;
        BID_RETURN (res);
      }
      // else cases that can be rounded to a 32-bit int fall through
      // to '1 <= q + exp <= 10'
    }
  }
  // n is not too large to be converted to int32: -2^31 - 1 < n < 2^31
  // Note: some of the cases tested for above fall through to this point
  if ((q + exp) <= 0) {	// n = +/-0.[0...0]c(0)c(1)...c(q-1)
    // return 0
    res = 0x00000000;
    BID_RETURN (res);
  } else {	// if (1 <= q + exp <= 10, 1 <= q <= 7, -6 <= exp <= 9)
    // -2^31-1 < x <= -1 or 1 <= x < 2^31 so x can be rounded
    // to nearest to a 32-bit signed integer
    if (exp < 0) {	// 2 <= q <= 7, -6 <= exp <= -1, 1 <= q + exp <= 10
      ind = -exp;	// 1 <= ind <= 6; ind is a synonym for 'x'
      // chop off ind digits from the lower part of C1
      // C1 fits in 32 bits
      // calculate C* and f*
      // C* is actually floor(C*) in this case
      // C* and f* need shifting and masking, as shown by
      // bid_shiftright128[] and bid_maskhigh128[]
      // 1 <= x <= 6 
      // kx = 10^(-x) = bid_ten2mk64[ind - 1]
      // C* = C1 * 10^(-x)
      // the approximation of 10^(-x) was rounded up to 24 bits
      __mul_64x64_to_128MACH (P128, (BID_UINT64)C1, bid_ten2mk64[ind - 1]);
      Cstar = P128.w[1];
      // the top Ex bits of 10^(-x) are T* = bid_ten2mk128trunc[ind].w[0], e.g.
      // if x=1, T*=bid_ten2mk128trunc[0].w[0]=0x1999999999999999
      // C* = floor(C*) (logical right shift; C has p decimal digits,
      //     correct by Property 1)
      // n = C* * 10^(e+x)

      // shift right C* by Ex-64 = bid_shiftright128[ind]
      shift = bid_shiftright128[ind - 1];	// 0 <= shift <= 39
      Cstar = Cstar >> shift;
      if (x_sign)
	res = -Cstar;
      else
	res = Cstar;
    } else if (exp == 0) {
      // 1 <= q <= 10
      // res = +/-C (exact)
      if (x_sign)
	res = -C1;
      else
	res = C1;
    } else {	// if (exp > 0) => 1 <= exp <= 9, 1 <= q < 9, 2 <= q + exp <= 10
      // res = +/-C * 10^exp (exact)
      if (x_sign)
	res = -C1 * bid_ten2k64[exp];
      else
	res = C1 * bid_ten2k64[exp];
    }
  }
  BID_RETURN (res);
}

/*****************************************************************************
 *  BID32_to_int32_xint
 ****************************************************************************/

#if DECIMAL_CALL_BY_REFERENCE
void
bid32_to_int32_xint (int *pres,
		     BID_UINT32*
		     px _EXC_FLAGS_PARAM _EXC_MASKS_PARAM
		     _EXC_INFO_PARAM) {
  BID_UINT32 x = *px;
#else
RES_WRAPFN_DFP(int, bid32_to_int32_xint, 32)
int
bid32_to_int32_xint (BID_UINT32 x _EXC_FLAGS_PARAM _EXC_MASKS_PARAM
		     _EXC_INFO_PARAM) {
#endif
  int res;
 BID_UINT32 x_sign;
 BID_UINT32 x_exp;
  int exp; // unbiased exponent
  // Note: C1 represents x_significand (BID_UINT32)
  BID_UINT64 tmp64;
  BID_UI32FLOAT tmp1;
  unsigned int x_nr_bits;
  int q, ind, shift;
  BID_UINT32 C1;
  BID_UINT64 Cstar; // C* represents up to 7 decimal digits ~ 24 bits
  BID_UINT128 fstar;
  BID_UINT128 P128;

  // check for NaN or Infinity
  if ((x & MASK_NAN32) == MASK_NAN32 || (x & MASK_INF32) == MASK_INF32) {
    // set invalid flag
    *pfpsf |= BID_INVALID_EXCEPTION;
    // return Integer Indefinite
    res = 0x80000000;
    BID_RETURN (res);
  }
  // unpack x
  x_sign = x & MASK_SIGN32;	// 0 for positive, MASK_SIGN32 for negative
  // if steering bits are 11 (condition will be 0), then exponent is G[0:w+1] =>
  if ((x & MASK_STEERING_BITS32) == MASK_STEERING_BITS32) {
    x_exp = (x & MASK_BINARY_EXPONENT2_32) >> 21;	// biased
    C1 = (x & MASK_BINARY_SIG2_32) | MASK_BINARY_OR2_32;
    if (C1 > 9999999) {	// non-canonical
      x_exp = 0;
      C1 = 0;
    }
  } else {
    x_exp = (x & MASK_BINARY_EXPONENT1_32) >> 23;	// biased
    C1 = x & MASK_BINARY_SIG1_32;
  }

  // check for zeros (possibly from non-canonical values)
  if (C1 == 0x0) {
    // x is 0
    res = 0x00000000;
    BID_RETURN (res);
  }
  // x is not special and is not zero

  // q = nr. of decimal digits in x (1 <= q <= 7)
  //  determine first the nr. of bits in x
  tmp1.f = (float) C1; // exact conversion
  x_nr_bits = 1 + ((tmp1.ui32 >> 23) & 0xff) - 0x7f;
  q = bid_nr_digits[x_nr_bits - 1].digits;
  if (q == 0) {
    q = bid_nr_digits[x_nr_bits - 1].digits1;
    if (C1 >= bid_nr_digits[x_nr_bits - 1].threshold_lo)
      q++;
  }
  exp = x_exp - 101;	// unbiased exponent

  if ((q + exp) > 10) {	// x >= 10^10 ~= 2^33.2... (cannot fit in 32 bits)
    // set invalid flag
    *pfpsf |= BID_INVALID_EXCEPTION;
    // return Integer Indefinite
    res = 0x80000000;
    BID_RETURN (res);
  } else if ((q + exp) == 10) {	// x = c(0)c(1)...c(q-1)00..0 (10 dec. digits)
    // in this case 2^29.89... ~= 10^9 <= x < 10^10 ~= 2^33.2...
    // so x rounded to an integer may or may not fit in a signed 32-bit int
    // the cases that do not fit are identified here; the ones that fit
    // fall through and will be handled with other cases further,
    // under '1 <= q + exp <= 10'
    if (x_sign) {	// if n < 0 and q + exp = 10
      // if n <= -2^31 - 1 then n is too large
      // too large if c(0)c(1)...c(q-1)00...0 >= 2^31+1
      // <=> C * 10^(11-q) >= 0x50000000a, 1<=q<=7
      // Note: C * 10^(11-q) has 10 or 11 digits; 0x50000000a has 11 digits
      tmp64 = C1 * bid_ten2k64[11 - q];	// C scaled up to 11-digit int
      // c(0)c(1)...c(q-1)0...0 (11 digits)
      if (tmp64 >= 0x50000000aull) {
        // set invalid flag
        *pfpsf |= BID_INVALID_EXCEPTION;
        // return Integer Indefinite
        res = 0x80000000;
        BID_RETURN (res);
      }
      // else cases that can be rounded to a 32-bit int fall through
      // to '1 <= q + exp <= 10'
    } else {	// if n > 0 and q + exp = 10
      // if n >= 2^31 then n is too large
      // too large if c(0)c(1)...c(q-1)00...0 >= 2^31
      // <=> C * 10^(11-q) >= 0x500000000, 1<=q<=7
      // Note: C * 10^(11-q) has 10 or 11 digits; 0x500000000 has 11 digits
      tmp64 = C1 * bid_ten2k64[11 - q];	// C scaled up to 11-digit int
      // c(0)c(1)...c(q-1)0...0 (11 digits)
      if (tmp64 >= 0x500000000ull) {
        // set invalid flag
        *pfpsf |= BID_INVALID_EXCEPTION;
        // return Integer Indefinite
        res = 0x80000000;
        BID_RETURN (res);
      }
      // else cases that can be rounded to a 32-bit int fall through
      // to '1 <= q + exp <= 10'
    }
  }
  // n is not too large to be converted to int32: -2^31 - 1 < n < 2^31
  // Note: some of the cases tested for above fall through to this point
  if ((q + exp) <= 0) {	// n = +/-0.[0...0]c(0)c(1)...c(q-1)
    // set inexact flag
    *pfpsf |= BID_INEXACT_EXCEPTION;
    // return 0
    res = 0x00000000;
    BID_RETURN (res);
  } else {	// if (1 <= q + exp <= 10, 1 <= q <= 7, -6 <= exp <= 9)
    // -2^31-1 < x <= -1 or 1 <= x < 2^31 so x can be rounded
    // to nearest to a 32-bit signed integer
    if (exp < 0) {	// 2 <= q <= 7, -6 <= exp <= -1, 1 <= q + exp <= 10
      ind = -exp;	// 1 <= ind <= 6; ind is a synonym for 'x'
      // chop off ind digits from the lower part of C1
      // C1 fits in 32 bits
      // calculate C* and f*
      // C* is actually floor(C*) in this case
      // C* and f* need shifting and masking, as shown by
      // bid_shiftright128[] and bid_maskhigh128[]
      // 1 <= x <= 6 
      // kx = 10^(-x) = bid_ten2mk64[ind - 1]
      // C* = C1 * 10^(-x)
      // the approximation of 10^(-x) was rounded up to 24 bits
      __mul_64x64_to_128MACH (P128, (BID_UINT64)C1, bid_ten2mk64[ind - 1]);
      Cstar = P128.w[1];
      fstar.w[1] = P128.w[1] & bid_maskhigh128[ind - 1];
      fstar.w[0] = P128.w[0];
      // the top Ex bits of 10^(-x) are T* = bid_ten2mk128trunc[ind].w[0], e.g.
      // if x=1, T*=bid_ten2mk128trunc[0].w[0]=0x1999999999999999
      // C* = floor(C*) (logical right shift; C has p decimal digits,
      //     correct by Property 1)
      // n = C* * 10^(e+x)

      // shift right C* by Ex-64 = bid_shiftright128[ind]
      shift = bid_shiftright128[ind - 1];	// 0 <= shift <= 39
      Cstar = Cstar >> shift;
      // determine inexactness of the rounding of C*
      // if (0 < f* < 10^(-x)) then
      //   the result is exact
      // else // if (f* > T*) then
      //   the result is inexact
      if (ind - 1 <= 2) {
	if (fstar.w[0] > bid_ten2mk128trunc[ind - 1].w[1]) {
	  // bid_ten2mk128trunc[ind -1].w[1] is identical to
	  // bid_ten2mk128[ind -1].w[1]
	  // set the inexact flag
	  *pfpsf |= BID_INEXACT_EXCEPTION;
	}	// else the result is exact
      } else {	// if 3 <= ind - 1 <= 14
	if (fstar.w[1] || fstar.w[0] > bid_ten2mk128trunc[ind - 1].w[1]) {
	  // bid_ten2mk128trunc[ind -1].w[1] is identical to
	  // bid_ten2mk128[ind -1].w[1]
	  // set the inexact flag
	  *pfpsf |= BID_INEXACT_EXCEPTION;
	}	// else the result is exact
      }

      if (x_sign)
	res = -Cstar;
      else
	res = Cstar;
    } else if (exp == 0) {
      // 1 <= q <= 10
      // res = +/-C (exact)
      if (x_sign)
	res = -C1;
      else
	res = C1;
    } else {	// if (exp > 0) => 1 <= exp <= 9, 1 <= q < 9, 2 <= q + exp <= 10
      // res = +/-C * 10^exp (exact)
      if (x_sign)
	res = -C1 * bid_ten2k64[exp];
      else
	res = C1 * bid_ten2k64[exp];
    }
  }
  BID_RETURN (res);
}

/*****************************************************************************
 *  BID32_to_int32_rninta
 ****************************************************************************/

#if DECIMAL_CALL_BY_REFERENCE
void
bid32_to_int32_rninta (int *pres,
		       BID_UINT32*
		       px _EXC_FLAGS_PARAM _EXC_MASKS_PARAM
		       _EXC_INFO_PARAM) {
  BID_UINT32 x = *px;
#else
RES_WRAPFN_DFP(int, bid32_to_int32_rninta, 32)
int
bid32_to_int32_rninta (BID_UINT32 x _EXC_FLAGS_PARAM _EXC_MASKS_PARAM
		       _EXC_INFO_PARAM) {
#endif
  int res;
 BID_UINT32 x_sign;
 BID_UINT32 x_exp;
  int exp; // unbiased exponent
  // Note: C1 represents x_significand (BID_UINT32)
  BID_UINT64 tmp64;
  BID_UI32FLOAT tmp1;
  unsigned int x_nr_bits;
  int q, ind, shift;
  BID_UINT32 C1;
  BID_UINT64 Cstar;	// C* represents up to 7 decimal digits ~ 24 bits
  BID_UINT128 P128;

  // check for NaN or Infinity
  if ((x & MASK_NAN32) == MASK_NAN32 || (x & MASK_INF32) == MASK_INF32) {
    // set invalid flag
    *pfpsf |= BID_INVALID_EXCEPTION;
    // return Integer Indefinite
    res = 0x80000000;
    BID_RETURN (res);
  }
  // unpack x
  x_sign = x & MASK_SIGN32;	// 0 for positive, MASK_SIGN32 for negative
  // if steering bits are 11 (condition will be 0), then exponent is G[0:w+1] =>
  if ((x & MASK_STEERING_BITS32) == MASK_STEERING_BITS32) {
    x_exp = (x & MASK_BINARY_EXPONENT2_32) >> 21;	// biased
    C1 = (x & MASK_BINARY_SIG2_32) | MASK_BINARY_OR2_32;
    if (C1 > 9999999) {	// non-canonical
      x_exp = 0;
      C1 = 0;
    }
  } else {
    x_exp = (x & MASK_BINARY_EXPONENT1_32) >> 23;	// biased
    C1 = x & MASK_BINARY_SIG1_32;
  }

  // check for zeros (possibly from non-canonical values)
  if (C1 == 0x0) {
    // x is 0
    res = 0x00000000;
    BID_RETURN (res);
  }
  // x is not special and is not zero

  // q = nr. of decimal digits in x (1 <= q <= 7)
  //  determine first the nr. of bits in x
  tmp1.f = (float) C1; // exact conversion
  x_nr_bits = 1 + ((tmp1.ui32 >> 23) & 0xff) - 0x7f;
  q = bid_nr_digits[x_nr_bits - 1].digits;
  if (q == 0) {
    q = bid_nr_digits[x_nr_bits - 1].digits1;
    if (C1 >= bid_nr_digits[x_nr_bits - 1].threshold_lo)
      q++;
  }
  exp = x_exp - 101;	// unbiased exponent

  if ((q + exp) > 10) {	// x >= 10^10 ~= 2^33.2... (cannot fit in 32 bits)
    // set invalid flag
    *pfpsf |= BID_INVALID_EXCEPTION;
    // return Integer Indefinite
    res = 0x80000000;
    BID_RETURN (res);
  } else if ((q + exp) == 10) {	// x = c(0)c(1)...c(q-1)00..0 (10 dec. digits)
    // in this case 2^29.89... ~= 10^9 <= x < 10^10 ~= 2^33.2...
    // so x rounded to an integer may or may not fit in a signed 32-bit int
    // the cases that do not fit are identified here; the ones that fit
    // fall through and will be handled with other cases further,
    // under '1 <= q + exp <= 10'
    if (x_sign) {	// if n < 0 and q + exp = 10
      // if n <= -2^31 - 1/2 then n is too large
      // too large if c(0)c(1)...c(q-1)00...0 >= 2^31+1/2
      // <=> C * 10^(11-q) >= 0x500000005, 1<=q<=7
      // Note: C * 10^(11-q) has 10 or 11 digits; 0x500000005 has 11 digits
      tmp64 = C1 * bid_ten2k64[11 - q];	// C scaled up to 11-digit int
      // c(0)c(1)...c(q-1)0...0 (11 digits)
      if (tmp64 >= 0x500000005ull) {
        // set invalid flag
        *pfpsf |= BID_INVALID_EXCEPTION;
        // return Integer Indefinite
        res = 0x80000000;
        BID_RETURN (res);
      }
      // else cases that can be rounded to a 32-bit int fall through
      // to '1 <= q + exp <= 10'
    } else {	// if n > 0 and q + exp = 10
      // if n >= 2^31 - 1/2 then n is too large
      // too large if c(0)c(1)...c(q-1)00...0 >= 2^31-1/2
      // <=> C * 10^(11-q) >= 0x4fffffffb, 1<=q<=7
      // Note: C * 10^(11-q) has 10 or 11 digits; 0x500000005 has 11 digits
      tmp64 = C1 * bid_ten2k64[11 - q];	// C scaled up to 11-digit int
      // c(0)c(1)...c(q-1)0...0 (11 digits)
      if (tmp64 >= 0x4fffffffbull) {
        // set invalid flag
        *pfpsf |= BID_INVALID_EXCEPTION;
        // return Integer Indefinite
        res = 0x80000000;
        BID_RETURN (res);
      }
      // else cases that can be rounded to a 32-bit int fall through
      // to '1 <= q + exp <= 10'
    }
  }
  // n is not too large to be converted to int32: -2^31 - 1/2 < n < 2^31 - 1/2
  // Note: some of the cases tested for above fall through to this point
  if ((q + exp) < 0) {	// n = +/-0.0...c(0)c(1)...c(q-1)
    // return 0
    res = 0x00000000;
    BID_RETURN (res);
  } else if ((q + exp) == 0) {	// n = +/-0.c(0)c(1)...c(q-1)
    // if 0.c(0)c(1)...c(q-1) <= 0.5 <=> c(0)c(1)...c(q-1) <= 5 * 10^(q-1)
    //   res = 0
    // else
    //   res = +/-1
    ind = q - 1;
    if (C1 < bid_midpoint64[ind]) {
      res = 0x00000000;	// return 0
    } else if (x_sign) {	// n < 0
      res = 0xffffffff;	// return -1
    } else {	// n > 0
      res = 0x00000001;	// return +1
    }
  } else {	// if (1 <= q + exp <= 10, 1 <= q <= 7, -6 <= exp <= 9)
    // -2^31-1/2 <= x <= -1 or 1 <= x < 2^31-1/2 so x can be rounded
    // to nearest away to a 32-bit signed integer
    if (exp < 0) {	// 2 <= q <= 7, -6 <= exp <= -1, 1 <= q + exp <= 10
      ind = -exp;	// 1 <= ind <= 6; ind is a synonym for 'x'
      // chop off ind digits from the lower part of C1
      // C1 = C1 + 1/2 * 10^ind where the result C1 fits in 32 bits
      C1 = C1 + (BID_UINT32)bid_midpoint64[ind - 1];
      // calculate C* and f*
      // C* is actually floor(C*) in this case
      // C* and f* need shifting and masking, as shown by
      // bid_shiftright128[] and bid_maskhigh128[]
      // 1 <= x <= 6 
      // kx = 10^(-x) = bid_ten2mk64[ind - 1]
      // C* = (C1 + 1/2 * 10^x) * 10^(-x)
      // the approximation of 10^(-x) was rounded up to 24 bits
      __mul_64x64_to_128MACH (P128, (BID_UINT64)C1, bid_ten2mk64[ind - 1]);
      Cstar = P128.w[1];
      // the top Ex bits of 10^(-x) are T* = bid_ten2mk128trunc[ind].w[0], e.g.
      // if x=1, T*=bid_ten2mk128trunc[0].w[0]=0x1999999999999999
      // C* = floor(C*)-1 (logical right shift; C* has p decimal digits, 
      // correct by Pr. 1)
      // n = C* * 10^(e+x)

      // shift right C* by Ex-64 = bid_shiftright128[ind]
      shift = bid_shiftright128[ind - 1];	// 0 <= shift <= 39
      Cstar = Cstar >> shift;

      // if the result was a midpoint it was rounded away from zero
      if (x_sign)
	res = -Cstar;
      else
	res = Cstar;
    } else if (exp == 0) {
      // 1 <= q <= 10
      // res = +/-C (exact)
      if (x_sign)
	res = -C1;
      else
	res = C1;
    } else {	// if (exp > 0) => 1 <= exp <= 9, 1 <= q < 9, 2 <= q + exp <= 10
      // res = +/-C * 10^exp (exact)
      if (x_sign)
	res = -C1 * bid_ten2k64[exp];
      else
	res = C1 * bid_ten2k64[exp];
    }
  }
  BID_RETURN (res);
}

/*****************************************************************************
 *  BID32_to_int32_xrninta
 ****************************************************************************/

#if DECIMAL_CALL_BY_REFERENCE
void
bid32_to_int32_xrninta (int *pres,
			BID_UINT32*
			px _EXC_FLAGS_PARAM _EXC_MASKS_PARAM
			_EXC_INFO_PARAM) {
  BID_UINT32 x = *px;
#else
RES_WRAPFN_DFP(int, bid32_to_int32_xrninta, 32)
int
bid32_to_int32_xrninta (BID_UINT32 x _EXC_FLAGS_PARAM _EXC_MASKS_PARAM
			_EXC_INFO_PARAM) {
#endif
  int res;
 BID_UINT32 x_sign;
 BID_UINT32 x_exp;
  int exp; // unbiased exponent
  // Note: C1 represents x_significand (BID_UINT32)
  BID_UINT64 tmp64;
  BID_UI32FLOAT tmp1;
  unsigned int x_nr_bits;
  int q, ind, shift;
  BID_UINT32 C1;
  BID_UINT64 Cstar; // C* represents up to 7 decimal digits ~ 24 bits
  BID_UINT128 fstar;
  BID_UINT128 P128;

  // check for NaN or Infinity
  if ((x & MASK_NAN32) == MASK_NAN32 || (x & MASK_INF32) == MASK_INF32) {
    // set invalid flag
    *pfpsf |= BID_INVALID_EXCEPTION;
    // return Integer Indefinite
    res = 0x80000000;
    BID_RETURN (res);
  }
  // unpack x
  x_sign = x & MASK_SIGN32;	// 0 for positive, MASK_SIGN32 for negative
  // if steering bits are 11 (condition will be 0), then exponent is G[0:w+1] =>
  if ((x & MASK_STEERING_BITS32) == MASK_STEERING_BITS32) {
    x_exp = (x & MASK_BINARY_EXPONENT2_32) >> 21;	// biased
    C1 = (x & MASK_BINARY_SIG2_32) | MASK_BINARY_OR2_32;
    if (C1 > 9999999) {	// non-canonical
      x_exp = 0;
      C1 = 0;
    }
  } else {
    x_exp = (x & MASK_BINARY_EXPONENT1_32) >> 23;	// biased
    C1 = x & MASK_BINARY_SIG1_32;
  }

  // check for zeros (possibly from non-canonical values)
  if (C1 == 0x0) {
    // x is 0
    res = 0x00000000;
    BID_RETURN (res);
  }
  // x is not special and is not zero

  // q = nr. of decimal digits in x (1 <= q <= 7)
  //  determine first the nr. of bits in x
  tmp1.f = (float) C1; // exact conversion
  x_nr_bits = 1 + ((tmp1.ui32 >> 23) & 0xff) - 0x7f;
  q = bid_nr_digits[x_nr_bits - 1].digits;
  if (q == 0) {
    q = bid_nr_digits[x_nr_bits - 1].digits1;
    if (C1 >= bid_nr_digits[x_nr_bits - 1].threshold_lo)
      q++;
  }
  exp = x_exp - 101;	// unbiased exponent

  if ((q + exp) > 10) {	// x >= 10^10 ~= 2^33.2... (cannot fit in 32 bits)
    // set invalid flag
    *pfpsf |= BID_INVALID_EXCEPTION;
    // return Integer Indefinite
    res = 0x80000000;
    BID_RETURN (res);
  } else if ((q + exp) == 10) {	// x = c(0)c(1)...c(q-1)00..0 (10 dec. digits)
    // in this case 2^29.89... ~= 10^9 <= x < 10^10 ~= 2^33.2...
    // so x rounded to an integer may or may not fit in a signed 32-bit int
    // the cases that do not fit are identified here; the ones that fit
    // fall through and will be handled with other cases further,
    // under '1 <= q + exp <= 10'
    if (x_sign) {	// if n < 0 and q + exp = 10
      // if n <= -2^31 - 1/2 then n is too large
      // too large if c(0)c(1)...c(q-1)00...0 >= 2^31+1/2
      // <=> C * 10^(11-q) >= 0x500000005, 1<=q<=7
      // Note: C * 10^(11-q) has 10 or 11 digits; 0x500000005 has 11 digits
      tmp64 = C1 * bid_ten2k64[11 - q];	// C scaled up to 11-digit int
      // c(0)c(1)...c(q-1)0...0 (11 digits)
      if (tmp64 >= 0x500000005ull) {
        // set invalid flag
        *pfpsf |= BID_INVALID_EXCEPTION;
        // return Integer Indefinite
        res = 0x80000000;
        BID_RETURN (res);
      }
      // else cases that can be rounded to a 32-bit int fall through
      // to '1 <= q + exp <= 10'
    } else {	// if n > 0 and q + exp = 10
      // if n >= 2^31 - 1/2 then n is too large
      // too large if c(0)c(1)...c(q-1)00...0 >= 2^31-1/2
      // <=> C * 10^(11-q) >= 0x4fffffffb, 1<=q<=7
      // Note: C * 10^(11-q) has 10 or 11 digits; 0x500000005 has 11 digits
      tmp64 = C1 * bid_ten2k64[11 - q];	// C scaled up to 11-digit int
      // c(0)c(1)...c(q-1)0...0 (11 digits)
      if (tmp64 >= 0x4fffffffbull) {
        // set invalid flag
        *pfpsf |= BID_INVALID_EXCEPTION;
        // return Integer Indefinite
        res = 0x80000000;
        BID_RETURN (res);
      }
      // else cases that can be rounded to a 32-bit int fall through
      // to '1 <= q + exp <= 10'
    }
  }
  // n is not too large to be converted to int32: -2^31 - 1/2 < n < 2^31 - 1/2
  // Note: some of the cases tested for above fall through to this point
  if ((q + exp) < 0) {	// n = +/-0.0...c(0)c(1)...c(q-1)
    // set inexact flag
    *pfpsf |= BID_INEXACT_EXCEPTION;
    // return 0
    res = 0x00000000;
    BID_RETURN (res);
  } else if ((q + exp) == 0) {	// n = +/-0.c(0)c(1)...c(q-1)
    // if 0.c(0)c(1)...c(q-1) <= 0.5 <=> c(0)c(1)...c(q-1) <= 5 * 10^(q-1)
    //   res = 0
    // else
    //   res = +/-1
    ind = q - 1;
    if (C1 < bid_midpoint64[ind]) {
      res = 0x00000000;	// return 0
    } else if (x_sign) {	// n < 0
      res = 0xffffffff;	// return -1
    } else {	// n > 0
      res = 0x00000001;	// return +1
    }
    // set inexact flag
    *pfpsf |= BID_INEXACT_EXCEPTION;
  } else {	// if (1 <= q + exp <= 10, 1 <= q <= 7, -6 <= exp <= 9)
    // -2^31-1/2 <= x <= -1 or 1 <= x < 2^31-1/2 so x can be rounded
    // to nearest away to a 32-bit signed integer
    if (exp < 0) {	// 2 <= q <= 7, -6 <= exp <= -1, 1 <= q + exp <= 10
      ind = -exp;	// 1 <= ind <= 6; ind is a synonym for 'x'
      // chop off ind digits from the lower part of C1
      // C1 = C1 + 1/2 * 10^ind where the result C1 fits in 32 bits
      C1 = C1 + (BID_UINT32)bid_midpoint64[ind - 1];
      // calculate C* and f*
      // C* is actually floor(C*) in this case
      // C* and f* need shifting and masking, as shown by
      // bid_shiftright128[] and bid_maskhigh128[]
      // 1 <= x <= 6 
      // kx = 10^(-x) = bid_ten2mk64[ind - 1]
      // C* = (C1 + 1/2 * 10^x) * 10^(-x)
      // the approximation of 10^(-x) was rounded up to 24 bits
      __mul_64x64_to_128MACH (P128, (BID_UINT64)C1, bid_ten2mk64[ind - 1]);
      Cstar = P128.w[1];
      fstar.w[1] = P128.w[1] & bid_maskhigh128[ind - 1];
      fstar.w[0] = P128.w[0];
      // the top Ex bits of 10^(-x) are T* = bid_ten2mk128trunc[ind].w[0], e.g.
      // if x=1, T*=bid_ten2mk128trunc[0].w[0]=0x1999999999999999
      // C* = floor(C*)-1 (logical right shift; C* has p decimal digits, 
      // correct by Pr. 1)
      // n = C* * 10^(e+x)

      // shift right C* by Ex-64 = bid_shiftright128[ind]
      shift = bid_shiftright128[ind - 1];	// 0 <= shift <= 39
      Cstar = Cstar >> shift;
      // determine inexactness of the rounding of C*
      // if (0 < f* - 1/2 < 10^(-x)) then
      //   the result is exact
      // else // if (f* - 1/2 > T*) then
      //   the result is inexact
      if (ind - 1 <= 2) {
	if (fstar.w[0] > 0x8000000000000000ull) {
	  // f* > 1/2 and the result may be exact
	  tmp64 = fstar.w[0] - 0x8000000000000000ull;	// f* - 1/2
	  if ((tmp64 > bid_ten2mk128trunc[ind - 1].w[1])) {
	    // bid_ten2mk128trunc[ind -1].w[1] is identical to
	    // bid_ten2mk128[ind -1].w[1]
	    // set the inexact flag
	    *pfpsf |= BID_INEXACT_EXCEPTION;
	  }	// else the result is exact
	} else {	// the result is inexact; f2* <= 1/2
	  // set the inexact flag
	  *pfpsf |= BID_INEXACT_EXCEPTION;
	}
      } else {	// if 3 <= ind - 1 <= 14
	if (fstar.w[1] > bid_onehalf128[ind - 1] ||
	    (fstar.w[1] == bid_onehalf128[ind - 1] && fstar.w[0])) {
	  // f2* > 1/2 and the result may be exact
	  // Calculate f2* - 1/2
	  tmp64 = fstar.w[1] - bid_onehalf128[ind - 1];
	  if (tmp64 || fstar.w[0] > bid_ten2mk128trunc[ind - 1].w[1]) {
	    // bid_ten2mk128trunc[ind -1].w[1] is identical to
	    // bid_ten2mk128[ind -1].w[1]
	    // set the inexact flag
	    *pfpsf |= BID_INEXACT_EXCEPTION;
	  }	// else the result is exact
	} else {	// the result is inexact; f2* <= 1/2
	  // set the inexact flag
	  *pfpsf |= BID_INEXACT_EXCEPTION;
	}
      }

      // if the result was a midpoint it was rounded away from zero
      if (x_sign)
	res = -Cstar;
      else
	res = Cstar;
    } else if (exp == 0) {
      // 1 <= q <= 10
      // res = +/-C (exact)
      if (x_sign)
	res = -C1;
      else
	res = C1;
    } else {	// if (exp > 0) => 1 <= exp <= 9, 1 <= q < 9, 2 <= q + exp <= 10
      // res = +/-C * 10^exp (exact)
      if (x_sign)
	res = -C1 * bid_ten2k64[exp];
      else
	res = C1 * bid_ten2k64[exp];
    }
  }
  BID_RETURN (res);
}
