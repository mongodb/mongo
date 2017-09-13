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
 *  BID128_to_int32_rnint 
 ****************************************************************************/

BID128_FUNCTION_ARG1_NORND_CUSTOMRESTYPE (int, bid128_to_int32_rnint, x)

     int res;
     BID_UINT64 x_sign;
     BID_UINT64 x_exp;
     int exp;			// unbiased exponent
  // Note: C1.w[1], C1.w[0] represent x_signif_hi, x_signif_lo (all are BID_UINT64)
     BID_UINT64 tmp64;
     BID_UI64DOUBLE tmp1;
     unsigned int x_nr_bits;
     int q, ind, shift;
     BID_UINT128 C1, C;
     BID_UINT128 Cstar;		// C* represents up to 34 decimal digits ~ 113 bits
     BID_UINT256 fstar;
     BID_UINT256 P256;

  // unpack x
x_sign = x.w[1] & MASK_SIGN;	// 0 for positive, MASK_SIGN for negative
x_exp = x.w[1] & MASK_EXP;	// biased and shifted left 49 bit positions
C1.w[1] = x.w[1] & MASK_COEFF;
C1.w[0] = x.w[0];

  // check for NaN or Infinity
if ((x.w[1] & MASK_SPECIAL) == MASK_SPECIAL) {
    // x is special
if ((x.w[1] & MASK_NAN) == MASK_NAN) {	// x is NAN
  if ((x.w[1] & MASK_SNAN) == MASK_SNAN) {	// x is SNAN
    // set invalid flag
    *pfpsf |= BID_INVALID_EXCEPTION;
    // return Integer Indefinite
    res = 0x80000000;
  } else {	// x is QNaN
    // set invalid flag
    *pfpsf |= BID_INVALID_EXCEPTION;
    // return Integer Indefinite
    res = 0x80000000;
  }
  BID_RETURN_VAL (res);
} else {	// x is not a NaN, so it must be infinity
  if (!x_sign) {	// x is +inf
    // set invalid flag
    *pfpsf |= BID_INVALID_EXCEPTION;
    // return Integer Indefinite
    res = 0x80000000;
  } else {	// x is -inf
    // set invalid flag
    *pfpsf |= BID_INVALID_EXCEPTION;
    // return Integer Indefinite
    res = 0x80000000;
  }
  BID_RETURN_VAL (res);
}
}
  // check for non-canonical values (after the check for special values)
if ((C1.w[1] > 0x0001ed09bead87c0ull)
    || (C1.w[1] == 0x0001ed09bead87c0ull
	&& (C1.w[0] > 0x378d8e63ffffffffull))
    || ((x.w[1] & 0x6000000000000000ull) == 0x6000000000000000ull)) {
  res = 0x00000000;
  BID_RETURN_VAL (res);
} else if ((C1.w[1] == 0x0ull) && (C1.w[0] == 0x0ull)) {
  // x is 0
  res = 0x00000000;
  BID_RETURN_VAL (res);
} else {	// x is not special and is not zero

  // q = nr. of decimal digits in x
  //  determine first the nr. of bits in x
  if (C1.w[1] == 0) {
    if (C1.w[0] >= 0x0020000000000000ull) {	// x >= 2^53
      // split the 64-bit value in two 32-bit halves to avoid rounding errors
	tmp1.d = (double) (C1.w[0] >> 32);	// exact conversion
	x_nr_bits =
	  33 + ((((unsigned int) (tmp1.ui64 >> 52)) & 0x7ff) - 0x3ff);
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
	|| (C1.w[1] == bid_nr_digits[x_nr_bits - 1].threshold_hi
	    && C1.w[0] >= bid_nr_digits[x_nr_bits - 1].threshold_lo))
      q++;
  }
  exp = (x_exp >> 49) - 6176;
  if ((q + exp) > 10) {	// x >= 10^10 ~= 2^33.2... (cannot fit in 32 bits)
    // set invalid flag
    *pfpsf |= BID_INVALID_EXCEPTION;
    // return Integer Indefinite
    res = 0x80000000;
    BID_RETURN_VAL (res);
  } else if ((q + exp) == 10) {	// x = c(0)c(1)...c(9).c(10)...c(q-1)
    // in this case 2^29.89... ~= 10^9 <= x < 10^10 ~= 2^33.2...
    // so x rounded to an integer may or may not fit in a signed 32-bit int
    // the cases that do not fit are identified here; the ones that fit
    // fall through and will be handled with other cases further,
    // under '1 <= q + exp <= 10'
    if (x_sign) {	// if n < 0 and q + exp = 10
      // if n < -2^31 - 1/2 then n is too large
      // too large if c(0)c(1)...c(9).c(10)...c(q-1) > 2^31+1/2
      // <=> 0.c(0)c(1)...c(q-1) * 10^11 > 0x500000005, 1<=q<=34
      if (q <= 11) {
	tmp64 = C1.w[0] * bid_ten2k64[11 - q];	// C scaled up to 11-digit int
	// c(0)c(1)...c(9)c(10) or c(0)c(1)...c(q-1)0...0 (11 digits)
	if (tmp64 > 0x500000005ull) {
	  // set invalid flag
	  *pfpsf |= BID_INVALID_EXCEPTION;
	  // return Integer Indefinite
	  res = 0x80000000;
	  BID_RETURN_VAL (res);
	}
	// else cases that can be rounded to a 32-bit int fall through
	// to '1 <= q + exp <= 10'
      } else {	// if (q > 11), i.e. 12 <= q <= 34 and so -24 <= exp <= -2
	// 0.c(0)c(1)...c(q-1) * 10^11 > 0x500000005 <=>
	// C > 0x500000005 * 10^(q-11) where 1 <= q - 11 <= 23
	// (scale 2^31+1/2 up)
	tmp64 = 0x500000005ull;
	if (q - 11 <= 19) {	// 1 <= q - 11 <= 19; 10^(q-11) requires 64 bits
	  __mul_64x64_to_128MACH (C, tmp64, bid_ten2k64[q - 11]);
	} else {	// 20 <= q - 11 <= 23, and 10^(q-11) requires 128 bits
	  __mul_128x64_to_128 (C, tmp64, bid_ten2k128[q - 31]);
	}
	if (C1.w[1] > C.w[1] || (C1.w[1] == C.w[1] && C1.w[0] > C.w[0])) {
	  // set invalid flag
	  *pfpsf |= BID_INVALID_EXCEPTION;
	  // return Integer Indefinite
	  res = 0x80000000;
	  BID_RETURN_VAL (res);
	}
	// else cases that can be rounded to a 32-bit int fall through
	// to '1 <= q + exp <= 10'
      }
    } else {	// if n > 0 and q + exp = 10
      // if n >= 2^31 - 1/2 then n is too large
      // too large if c(0)c(1)...c(9).c(10)...c(q-1) >= 2^31-1/2
      // too large if 0.c(0)c(1)...c(q-1) * 10^11 >= 0x4fffffffb, 1<=q<=34
      if (q <= 11) {
	tmp64 = C1.w[0] * bid_ten2k64[11 - q];	// C scaled up to 11-digit int
	// c(0)c(1)...c(9)c(10) or c(0)c(1)...c(q-1)0...0 (11 digits)
	if (tmp64 >= 0x4fffffffbull) {
	  // set invalid flag
	  *pfpsf |= BID_INVALID_EXCEPTION;
	  // return Integer Indefinite
	  res = 0x80000000;
	  BID_RETURN_VAL (res);
	}
	// else cases that can be rounded to a 32-bit int fall through
	// to '1 <= q + exp <= 10'
      } else {	// if (q > 11), i.e. 12 <= q <= 34 and so -24 <= exp <= -2
	// 0.c(0)c(1)...c(q-1) * 10^11 >= 0x4fffffffb <=>
	// C >= 0x4fffffffb * 10^(q-11) where 1 <= q - 11 <= 23
	// (scale 2^31-1/2 up)
	tmp64 = 0x4fffffffbull;
	if (q - 11 <= 19) {	// 1 <= q - 11 <= 19; 10^(q-11) requires 64 bits
	  __mul_64x64_to_128MACH (C, tmp64, bid_ten2k64[q - 11]);
	} else {	// 20 <= q - 11 <= 23, and 10^(q-11) requires 128 bits
	  __mul_128x64_to_128 (C, tmp64, bid_ten2k128[q - 31]);
	}
	if (C1.w[1] > C.w[1]
	    || (C1.w[1] == C.w[1] && C1.w[0] >= C.w[0])) {
	  // set invalid flag
	  *pfpsf |= BID_INVALID_EXCEPTION;
	  // return Integer Indefinite
	  res = 0x80000000;
	  BID_RETURN_VAL (res);
	}
	// else cases that can be rounded to a 32-bit int fall through
	// to '1 <= q + exp <= 10'
      }
    }
  }
  // n is not too large to be converted to int32: -2^31 - 1/2 < n < 2^31 - 1/2
  // Note: some of the cases tested for above fall through to this point
  if ((q + exp) < 0) {	// n = +/-0.0...c(0)c(1)...c(q-1)
    // return 0
    res = 0x00000000;
    BID_RETURN_VAL (res);
  } else if ((q + exp) == 0) {	// n = +/-0.c(0)c(1)...c(q-1)
    // if 0.c(0)c(1)...c(q-1) <= 0.5 <=> c(0)c(1)...c(q-1) <= 5 * 10^(q-1)
    //   res = 0
    // else
    //   res = +/-1
    ind = q - 1;
    if (ind <= 18) {	// 0 <= ind <= 18
      if ((C1.w[1] == 0) && (C1.w[0] <= bid_midpoint64[ind])) {
	res = 0x00000000;	// return 0
      } else if (x_sign) {	// n < 0
	res = 0xffffffff;	// return -1
      } else {	// n > 0
	res = 0x00000001;	// return +1
      }
    } else {	// 19 <= ind <= 33
      if ((C1.w[1] < bid_midpoint128[ind - 19].w[1])
	  || ((C1.w[1] == bid_midpoint128[ind - 19].w[1])
	      && (C1.w[0] <= bid_midpoint128[ind - 19].w[0]))) {
	res = 0x00000000;	// return 0
      } else if (x_sign) {	// n < 0
	res = 0xffffffff;	// return -1
      } else {	// n > 0
	res = 0x00000001;	// return +1
      }
    }
  } else {	// if (1 <= q + exp <= 10, 1 <= q <= 34, -33 <= exp <= 9)
    // -2^31-1/2 <= x <= -1 or 1 <= x < 2^31-1/2 so x can be rounded
    // to nearest to a 32-bit signed integer
    if (exp < 0) {	// 2 <= q <= 34, -33 <= exp <= -1, 1 <= q + exp <= 10
      ind = -exp;	// 1 <= ind <= 33; ind is a synonym for 'x'
      // chop off ind digits from the lower part of C1
      // C1 = C1 + 1/2 * 10^ind where the result C1 fits in 127 bits
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
      // 1 <= x <= 33
      // kx = 10^(-x) = bid_ten2mk128[ind - 1]
      // C* = (C1 + 1/2 * 10^x) * 10^(-x)
      // the approximation of 10^(-x) was rounded up to 118 bits
      __mul_128x128_to_256 (P256, C1, bid_ten2mk128[ind - 1]);
      if (ind - 1 <= 21) {	// 0 <= ind - 1 <= 21
	Cstar.w[1] = P256.w[3];
	Cstar.w[0] = P256.w[2];
	fstar.w[3] = 0;
	fstar.w[2] = P256.w[2] & bid_maskhigh128[ind - 1];
	fstar.w[1] = P256.w[1];
	fstar.w[0] = P256.w[0];
      } else {	// 22 <= ind - 1 <= 33
	Cstar.w[1] = 0;
	Cstar.w[0] = P256.w[3];
	fstar.w[3] = P256.w[3] & bid_maskhigh128[ind - 1];
	fstar.w[2] = P256.w[2];
	fstar.w[1] = P256.w[1];
	fstar.w[0] = P256.w[0];
      }
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
      shift = bid_shiftright128[ind - 1];	// 0 <= shift <= 102
      if (ind - 1 <= 21) {	// 0 <= ind - 1 <= 21
	Cstar.w[0] =
	  (Cstar.w[0] >> shift) | (Cstar.w[1] << (64 - shift));
	// redundant, it will be 0! Cstar.w[1] = (Cstar.w[1] >> shift);
      } else {	// 22 <= ind - 1 <= 33
	Cstar.w[0] = (Cstar.w[0] >> (shift - 64));	// 2 <= shift - 64 <= 38
      }
      // if the result was a midpoint it was rounded away from zero, so
      // it will need a correction
      // check for midpoints
      if ((fstar.w[3] == 0) && (fstar.w[2] == 0)
	  && (fstar.w[1] || fstar.w[0])
	  && (fstar.w[1] < bid_ten2mk128trunc[ind - 1].w[1]
	      || (fstar.w[1] == bid_ten2mk128trunc[ind - 1].w[1]
		  && fstar.w[0] <= bid_ten2mk128trunc[ind - 1].w[0]))) {
	// the result is a midpoint; round to nearest
	if (Cstar.w[0] & 0x01) {	// Cstar.w[0] is odd; MP in [EVEN, ODD]
	  // if floor(C*) is odd C = floor(C*) - 1; the result >= 1
	  Cstar.w[0]--;	// Cstar.w[0] is now even
	}	// else MP in [ODD, EVEN]
      }
      if (x_sign)
	res = -Cstar.w[0];
      else
	res = Cstar.w[0];
    } else if (exp == 0) {
      // 1 <= q <= 10
      // res = +/-C (exact)
      if (x_sign)
	res = -C1.w[0];
      else
	res = C1.w[0];
    } else {	// if (exp > 0) => 1 <= exp <= 9, 1 <= q < 9, 2 <= q + exp <= 10
      // res = +/-C * 10^exp (exact)
      if (x_sign)
	res = -C1.w[0] * bid_ten2k64[exp];
      else
	res = C1.w[0] * bid_ten2k64[exp];
    }
  }
}

BID_RETURN_VAL (res);
}

/*****************************************************************************
 *  BID128_to_int32_xrnint
 ****************************************************************************/

BID128_FUNCTION_ARG1_NORND_CUSTOMRESTYPE (int, bid128_to_int32_xrnint,
					  x)

     int res;
     BID_UINT64 x_sign;
     BID_UINT64 x_exp;
     int exp;			// unbiased exponent
  // Note: C1.w[1], C1.w[0] represent x_signif_hi, x_signif_lo (all are BID_UINT64)
     BID_UINT64 tmp64, tmp64A;
     BID_UI64DOUBLE tmp1;
     unsigned int x_nr_bits;
     int q, ind, shift;
     BID_UINT128 C1, C;
     BID_UINT128 Cstar;		// C* represents up to 34 decimal digits ~ 113 bits
     BID_UINT256 fstar;
     BID_UINT256 P256;

  // unpack x
x_sign = x.w[1] & MASK_SIGN;	// 0 for positive, MASK_SIGN for negative
x_exp = x.w[1] & MASK_EXP;	// biased and shifted left 49 bit positions
C1.w[1] = x.w[1] & MASK_COEFF;
C1.w[0] = x.w[0];

  // check for NaN or Infinity
if ((x.w[1] & MASK_SPECIAL) == MASK_SPECIAL) {
    // x is special
if ((x.w[1] & MASK_NAN) == MASK_NAN) {	// x is NAN
  if ((x.w[1] & MASK_SNAN) == MASK_SNAN) {	// x is SNAN
    // set invalid flag
    *pfpsf |= BID_INVALID_EXCEPTION;
    // return Integer Indefinite
    res = 0x80000000;
  } else {	// x is QNaN
    // set invalid flag
    *pfpsf |= BID_INVALID_EXCEPTION;
    // return Integer Indefinite
    res = 0x80000000;
  }
  BID_RETURN_VAL (res);
} else {	// x is not a NaN, so it must be infinity
  if (!x_sign) {	// x is +inf
    // set invalid flag
    *pfpsf |= BID_INVALID_EXCEPTION;
    // return Integer Indefinite
    res = 0x80000000;
  } else {	// x is -inf
    // set invalid flag
    *pfpsf |= BID_INVALID_EXCEPTION;
    // return Integer Indefinite
    res = 0x80000000;
  }
  BID_RETURN_VAL (res);
}
}
  // check for non-canonical values (after the check for special values)
if ((C1.w[1] > 0x0001ed09bead87c0ull)
    || (C1.w[1] == 0x0001ed09bead87c0ull
	&& (C1.w[0] > 0x378d8e63ffffffffull))
    || ((x.w[1] & 0x6000000000000000ull) == 0x6000000000000000ull)) {
  res = 0x00000000;
  BID_RETURN_VAL (res);
} else if ((C1.w[1] == 0x0ull) && (C1.w[0] == 0x0ull)) {
  // x is 0
  res = 0x00000000;
  BID_RETURN_VAL (res);
} else {	// x is not special and is not zero

  // q = nr. of decimal digits in x
  //  determine first the nr. of bits in x
  if (C1.w[1] == 0) {
    if (C1.w[0] >= 0x0020000000000000ull) {	// x >= 2^53
      // split the 64-bit value in two 32-bit halves to avoid rounding errors
	tmp1.d = (double) (C1.w[0] >> 32);	// exact conversion
	x_nr_bits =
	  33 + ((((unsigned int) (tmp1.ui64 >> 52)) & 0x7ff) - 0x3ff);
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
	|| (C1.w[1] == bid_nr_digits[x_nr_bits - 1].threshold_hi
	    && C1.w[0] >= bid_nr_digits[x_nr_bits - 1].threshold_lo))
      q++;
  }
  exp = (x_exp >> 49) - 6176;
  if ((q + exp) > 10) {	// x >= 10^10 ~= 2^33.2... (cannot fit in 32 bits)
    // set invalid flag
    *pfpsf |= BID_INVALID_EXCEPTION;
    // return Integer Indefinite
    res = 0x80000000;
    BID_RETURN_VAL (res);
  } else if ((q + exp) == 10) {	// x = c(0)c(1)...c(9).c(10)...c(q-1)
    // in this case 2^29.89... ~= 10^9 <= x < 10^10 ~= 2^33.2...
    // so x rounded to an integer may or may not fit in a signed 32-bit int
    // the cases that do not fit are identified here; the ones that fit
    // fall through and will be handled with other cases further,
    // under '1 <= q + exp <= 10'
    if (x_sign) {	// if n < 0 and q + exp = 10
      // if n < -2^31 - 1/2 then n is too large
      // too large if c(0)c(1)...c(9).c(10)...c(q-1) > 2^31+1/2
      // <=> 0.c(0)c(1)...c(q-1) * 10^11 > 0x500000005, 1<=q<=34
      if (q <= 11) {
	tmp64 = C1.w[0] * bid_ten2k64[11 - q];	// C scaled up to 11-digit int
	// c(0)c(1)...c(9)c(10) or c(0)c(1)...c(q-1)0...0 (11 digits)
	if (tmp64 > 0x500000005ull) {
	  // set invalid flag
	  *pfpsf |= BID_INVALID_EXCEPTION;
	  // return Integer Indefinite
	  res = 0x80000000;
	  BID_RETURN_VAL (res);
	}
	// else cases that can be rounded to a 32-bit int fall through
	// to '1 <= q + exp <= 10'
      } else {	// if (q > 11), i.e. 12 <= q <= 34 and so -24 <= exp <= -2
	// 0.c(0)c(1)...c(q-1) * 10^11 > 0x500000005 <=>
	// C > 0x500000005 * 10^(q-11) where 1 <= q - 11 <= 23
	// (scale 2^31+1/2 up)
	tmp64 = 0x500000005ull;
	if (q - 11 <= 19) {	// 1 <= q - 11 <= 19; 10^(q-11) requires 64 bits
	  __mul_64x64_to_128MACH (C, tmp64, bid_ten2k64[q - 11]);
	} else {	// 20 <= q - 11 <= 23, and 10^(q-11) requires 128 bits
	  __mul_128x64_to_128 (C, tmp64, bid_ten2k128[q - 31]);
	}
	if (C1.w[1] > C.w[1] || (C1.w[1] == C.w[1] && C1.w[0] > C.w[0])) {
	  // set invalid flag
	  *pfpsf |= BID_INVALID_EXCEPTION;
	  // return Integer Indefinite
	  res = 0x80000000;
	  BID_RETURN_VAL (res);
	}
	// else cases that can be rounded to a 32-bit int fall through
	// to '1 <= q + exp <= 10'
      }
    } else {	// if n > 0 and q + exp = 10
      // if n >= 2^31 - 1/2 then n is too large
      // too large if c(0)c(1)...c(9).c(10)...c(q-1) >= 2^31-1/2
      // too large if 0.c(0)c(1)...c(q-1) * 10^11 >= 0x4fffffffb, 1<=q<=34
      if (q <= 11) {
	tmp64 = C1.w[0] * bid_ten2k64[11 - q];	// C scaled up to 11-digit int
	// c(0)c(1)...c(9)c(10) or c(0)c(1)...c(q-1)0...0 (11 digits)
	if (tmp64 >= 0x4fffffffbull) {
	  // set invalid flag
	  *pfpsf |= BID_INVALID_EXCEPTION;
	  // return Integer Indefinite
	  res = 0x80000000;
	  BID_RETURN_VAL (res);
	}
	// else cases that can be rounded to a 32-bit int fall through
	// to '1 <= q + exp <= 10'
      } else {	// if (q > 11), i.e. 12 <= q <= 34 and so -24 <= exp <= -2
	// 0.c(0)c(1)...c(q-1) * 10^11 >= 0x4fffffffb <=>
	// C >= 0x4fffffffb * 10^(q-11) where 1 <= q - 11 <= 23
	// (scale 2^31-1/2 up)
	tmp64 = 0x4fffffffbull;
	if (q - 11 <= 19) {	// 1 <= q - 11 <= 19; 10^(q-11) requires 64 bits
	  __mul_64x64_to_128MACH (C, tmp64, bid_ten2k64[q - 11]);
	} else {	// 20 <= q - 11 <= 23, and 10^(q-11) requires 128 bits
	  __mul_128x64_to_128 (C, tmp64, bid_ten2k128[q - 31]);
	}
	if (C1.w[1] > C.w[1]
	    || (C1.w[1] == C.w[1] && C1.w[0] >= C.w[0])) {
	  // set invalid flag
	  *pfpsf |= BID_INVALID_EXCEPTION;
	  // return Integer Indefinite
	  res = 0x80000000;
	  BID_RETURN_VAL (res);
	}
	// else cases that can be rounded to a 32-bit int fall through
	// to '1 <= q + exp <= 10'
      }
    }
  }
  // n is not too large to be converted to int32: -2^31 - 1/2 < n < 2^31 - 1/2
  // Note: some of the cases tested for above fall through to this point
  if ((q + exp) < 0) {	// n = +/-0.0...c(0)c(1)...c(q-1)
    // set inexact flag
    *pfpsf |= BID_INEXACT_EXCEPTION;
    // return 0
    res = 0x00000000;
    BID_RETURN_VAL (res);
  } else if ((q + exp) == 0) {	// n = +/-0.c(0)c(1)...c(q-1)
    // if 0.c(0)c(1)...c(q-1) <= 0.5 <=> c(0)c(1)...c(q-1) <= 5 * 10^(q-1)
    //   res = 0
    // else
    //   res = +/-1
    ind = q - 1;
    if (ind <= 18) {	// 0 <= ind <= 18
      if ((C1.w[1] == 0) && (C1.w[0] <= bid_midpoint64[ind])) {
	res = 0x00000000;	// return 0
      } else if (x_sign) {	// n < 0
	res = 0xffffffff;	// return -1
      } else {	// n > 0
	res = 0x00000001;	// return +1
      }
    } else {	// 19 <= ind <= 33
      if ((C1.w[1] < bid_midpoint128[ind - 19].w[1])
	  || ((C1.w[1] == bid_midpoint128[ind - 19].w[1])
	      && (C1.w[0] <= bid_midpoint128[ind - 19].w[0]))) {
	res = 0x00000000;	// return 0
      } else if (x_sign) {	// n < 0
	res = 0xffffffff;	// return -1
      } else {	// n > 0
	res = 0x00000001;	// return +1
      }
    }
    // set inexact flag
    *pfpsf |= BID_INEXACT_EXCEPTION;
  } else {	// if (1 <= q + exp <= 10, 1 <= q <= 34, -33 <= exp <= 9)
    // -2^31-1/2 <= x <= -1 or 1 <= x < 2^31-1/2 so x can be rounded
    // to nearest to a 32-bit signed integer
    if (exp < 0) {	// 2 <= q <= 34, -33 <= exp <= -1, 1 <= q + exp <= 10
      ind = -exp;	// 1 <= ind <= 33; ind is a synonym for 'x'
      // chop off ind digits from the lower part of C1
      // C1 = C1 + 1/2 * 10^ind where the result C1 fits in 127 bits
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
      // 1 <= x <= 33
      // kx = 10^(-x) = bid_ten2mk128[ind - 1]
      // C* = (C1 + 1/2 * 10^x) * 10^(-x)
      // the approximation of 10^(-x) was rounded up to 118 bits
      __mul_128x128_to_256 (P256, C1, bid_ten2mk128[ind - 1]);
      if (ind - 1 <= 21) {	// 0 <= ind - 1 <= 21
	Cstar.w[1] = P256.w[3];
	Cstar.w[0] = P256.w[2];
	fstar.w[3] = 0;
	fstar.w[2] = P256.w[2] & bid_maskhigh128[ind - 1];
	fstar.w[1] = P256.w[1];
	fstar.w[0] = P256.w[0];
      } else {	// 22 <= ind - 1 <= 33
	Cstar.w[1] = 0;
	Cstar.w[0] = P256.w[3];
	fstar.w[3] = P256.w[3] & bid_maskhigh128[ind - 1];
	fstar.w[2] = P256.w[2];
	fstar.w[1] = P256.w[1];
	fstar.w[0] = P256.w[0];
      }
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
      shift = bid_shiftright128[ind - 1];	// 0 <= shift <= 102
      if (ind - 1 <= 21) {	// 0 <= ind - 1 <= 21
	Cstar.w[0] =
	  (Cstar.w[0] >> shift) | (Cstar.w[1] << (64 - shift));
	// redundant, it will be 0! Cstar.w[1] = (Cstar.w[1] >> shift);
      } else {	// 22 <= ind - 1 <= 33
	Cstar.w[0] = (Cstar.w[0] >> (shift - 64));	// 2 <= shift - 64 <= 38
      }
      // determine inexactness of the rounding of C*
      // if (0 < f* - 1/2 < 10^(-x)) then
      //   the result is exact
      // else // if (f* - 1/2 > T*) then
      //   the result is inexact
      if (ind - 1 <= 2) {
	if (fstar.w[1] > 0x8000000000000000ull || (fstar.w[1] == 0x8000000000000000ull && fstar.w[0] > 0x0ull)) {	// f* > 1/2 and the result may be exact
	  tmp64 = fstar.w[1] - 0x8000000000000000ull;	// f* - 1/2
	  if (tmp64 > bid_ten2mk128trunc[ind - 1].w[1]
	      || (tmp64 == bid_ten2mk128trunc[ind - 1].w[1]
		  && fstar.w[0] >= bid_ten2mk128trunc[ind - 1].w[0])) {
	    // set the inexact flag
	    *pfpsf |= BID_INEXACT_EXCEPTION;
	  }	// else the result is exact
	} else {	// the result is inexact; f2* <= 1/2
	  // set the inexact flag
	  *pfpsf |= BID_INEXACT_EXCEPTION;
	}
      } else if (ind - 1 <= 21) {	// if 3 <= ind <= 21
	if (fstar.w[3] > 0x0 ||
	    (fstar.w[3] == 0x0 && fstar.w[2] > bid_onehalf128[ind - 1]) ||
	    (fstar.w[3] == 0x0 && fstar.w[2] == bid_onehalf128[ind - 1] &&
	     (fstar.w[1] || fstar.w[0]))) {
	  // f2* > 1/2 and the result may be exact
	  // Calculate f2* - 1/2
	  tmp64 = fstar.w[2] - bid_onehalf128[ind - 1];
	  tmp64A = fstar.w[3];
	  if (tmp64 > fstar.w[2])
	    tmp64A--;
	  if (tmp64A || tmp64
	      || fstar.w[1] > bid_ten2mk128trunc[ind - 1].w[1]
	      || (fstar.w[1] == bid_ten2mk128trunc[ind - 1].w[1]
		  && fstar.w[0] > bid_ten2mk128trunc[ind - 1].w[0])) {
	    // set the inexact flag
	    *pfpsf |= BID_INEXACT_EXCEPTION;
	  }	// else the result is exact
	} else {	// the result is inexact; f2* <= 1/2
	  // set the inexact flag
	  *pfpsf |= BID_INEXACT_EXCEPTION;
	}
      } else {	// if 22 <= ind <= 33
	if (fstar.w[3] > bid_onehalf128[ind - 1] ||
	    (fstar.w[3] == bid_onehalf128[ind - 1] &&
	     (fstar.w[2] || fstar.w[1] || fstar.w[0]))) {
	  // f2* > 1/2 and the result may be exact
	  // Calculate f2* - 1/2
	  tmp64 = fstar.w[3] - bid_onehalf128[ind - 1];
	  if (tmp64 || fstar.w[2]
	      || fstar.w[1] > bid_ten2mk128trunc[ind - 1].w[1]
	      || (fstar.w[1] == bid_ten2mk128trunc[ind - 1].w[1]
		  && fstar.w[0] > bid_ten2mk128trunc[ind - 1].w[0])) {
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
      if ((fstar.w[3] == 0) && (fstar.w[2] == 0)
	  && (fstar.w[1] || fstar.w[0])
	  && (fstar.w[1] < bid_ten2mk128trunc[ind - 1].w[1]
	      || (fstar.w[1] == bid_ten2mk128trunc[ind - 1].w[1]
		  && fstar.w[0] <= bid_ten2mk128trunc[ind - 1].w[0]))) {
	// the result is a midpoint; round to nearest
	if (Cstar.w[0] & 0x01) {	// Cstar.w[0] is odd; MP in [EVEN, ODD]
	  // if floor(C*) is odd C = floor(C*) - 1; the result >= 1
	  Cstar.w[0]--;	// Cstar.w[0] is now even
	}	// else MP in [ODD, EVEN]
      }
      if (x_sign)
	res = -Cstar.w[0];
      else
	res = Cstar.w[0];
    } else if (exp == 0) {
      // 1 <= q <= 10
      // res = +/-C (exact)
      if (x_sign)
	res = -C1.w[0];
      else
	res = C1.w[0];
    } else {	// if (exp > 0) => 1 <= exp <= 9, 1 <= q < 9, 2 <= q + exp <= 10
      // res = +/-C * 10^exp (exact)
      if (x_sign)
	res = -C1.w[0] * bid_ten2k64[exp];
      else
	res = C1.w[0] * bid_ten2k64[exp];
    }
  }
}

BID_RETURN_VAL (res);
}

/*****************************************************************************
 *  BID128_to_int32_floor
 ****************************************************************************/

BID128_FUNCTION_ARG1_NORND_CUSTOMRESTYPE (int, bid128_to_int32_floor, x)

     int res;
     BID_UINT64 x_sign;
     BID_UINT64 x_exp;
     int exp;			// unbiased exponent
  // Note: C1.w[1], C1.w[0] represent x_signif_hi, x_signif_lo (all are BID_UINT64)
     BID_UINT64 tmp64, tmp64A;
     BID_UI64DOUBLE tmp1;
     unsigned int x_nr_bits;
     int q, ind, shift;
     BID_UINT128 C1, C;
     BID_UINT128 Cstar;		// C* represents up to 34 decimal digits ~ 113 bits
     BID_UINT256 fstar;
     BID_UINT256 P256;
     int is_inexact_lt_midpoint = 0;
     int is_inexact_gt_midpoint = 0;
     int is_midpoint_lt_even = 0;
     int is_midpoint_gt_even = 0;

  // unpack x
x_sign = x.w[1] & MASK_SIGN;	// 0 for positive, MASK_SIGN for negative
x_exp = x.w[1] & MASK_EXP;	// biased and shifted left 49 bit positions
C1.w[1] = x.w[1] & MASK_COEFF;
C1.w[0] = x.w[0];

  // check for NaN or Infinity
if ((x.w[1] & MASK_SPECIAL) == MASK_SPECIAL) {
    // x is special
if ((x.w[1] & MASK_NAN) == MASK_NAN) {	// x is NAN
  if ((x.w[1] & MASK_SNAN) == MASK_SNAN) {	// x is SNAN
    // set invalid flag
    *pfpsf |= BID_INVALID_EXCEPTION;
    // return Integer Indefinite
    res = 0x80000000;
  } else {	// x is QNaN
    // set invalid flag
    *pfpsf |= BID_INVALID_EXCEPTION;
    // return Integer Indefinite
    res = 0x80000000;
  }
  BID_RETURN_VAL (res);
} else {	// x is not a NaN, so it must be infinity
  if (!x_sign) {	// x is +inf
    // set invalid flag
    *pfpsf |= BID_INVALID_EXCEPTION;
    // return Integer Indefinite
    res = 0x80000000;
  } else {	// x is -inf
    // set invalid flag
    *pfpsf |= BID_INVALID_EXCEPTION;
    // return Integer Indefinite
    res = 0x80000000;
  }
  BID_RETURN_VAL (res);
}
}
  // check for non-canonical values (after the check for special values)
if ((C1.w[1] > 0x0001ed09bead87c0ull)
    || (C1.w[1] == 0x0001ed09bead87c0ull
	&& (C1.w[0] > 0x378d8e63ffffffffull))
    || ((x.w[1] & 0x6000000000000000ull) == 0x6000000000000000ull)) {
  res = 0x00000000;
  BID_RETURN_VAL (res);
} else if ((C1.w[1] == 0x0ull) && (C1.w[0] == 0x0ull)) {
  // x is 0
  res = 0x00000000;
  BID_RETURN_VAL (res);
} else {	// x is not special and is not zero

  // q = nr. of decimal digits in x
  //  determine first the nr. of bits in x
  if (C1.w[1] == 0) {
    if (C1.w[0] >= 0x0020000000000000ull) {	// x >= 2^53
      // split the 64-bit value in two 32-bit halves to avoid rounding errors
	tmp1.d = (double) (C1.w[0] >> 32);	// exact conversion
	x_nr_bits =
	  33 + ((((unsigned int) (tmp1.ui64 >> 52)) & 0x7ff) - 0x3ff);
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
	|| (C1.w[1] == bid_nr_digits[x_nr_bits - 1].threshold_hi
	    && C1.w[0] >= bid_nr_digits[x_nr_bits - 1].threshold_lo))
      q++;
  }
  exp = (x_exp >> 49) - 6176;
  if ((q + exp) > 10) {	// x >= 10^10 ~= 2^33.2... (cannot fit in 32 bits)
    // set invalid flag
    *pfpsf |= BID_INVALID_EXCEPTION;
    // return Integer Indefinite
    res = 0x80000000;
    BID_RETURN_VAL (res);
  } else if ((q + exp) == 10) {	// x = c(0)c(1)...c(9).c(10)...c(q-1)
    // in this case 2^29.89... ~= 10^9 <= x < 10^10 ~= 2^33.2...
    // so x rounded to an integer may or may not fit in a signed 32-bit int
    // the cases that do not fit are identified here; the ones that fit
    // fall through and will be handled with other cases further,
    // under '1 <= q + exp <= 10'
    if (x_sign) {	// if n < 0 and q + exp = 10
      // if n < -2^31 then n is too large
      // too large if c(0)c(1)...c(9).c(10)...c(q-1) > 2^31
      // <=> 0.c(0)c(1)...c(q-1) * 10^11 > 0x500000000, 1<=q<=34
      if (q <= 11) {
	tmp64 = C1.w[0] * bid_ten2k64[11 - q];	// C scaled up to 11-digit int
	// c(0)c(1)...c(9)c(10) or c(0)c(1)...c(q-1)0...0 (11 digits)
	if (tmp64 > 0x500000000ull) {
	  // set invalid flag
	  *pfpsf |= BID_INVALID_EXCEPTION;
	  // return Integer Indefinite
	  res = 0x80000000;
	  BID_RETURN_VAL (res);
	}
	// else cases that can be rounded to a 32-bit int fall through
	// to '1 <= q + exp <= 10'
      } else {	// if (q > 11), i.e. 12 <= q <= 34 and so -24 <= exp <= -2
	// 0.c(0)c(1)...c(q-1) * 10^11 > 0x500000000 <=>
	// C > 0x500000000 * 10^(q-11) where 1 <= q - 11 <= 23
	// (scale 2^31 up)
	tmp64 = 0x500000000ull;
	if (q - 11 <= 19) {	// 1 <= q - 11 <= 19; 10^(q-11) requires 64 bits
	  __mul_64x64_to_128MACH (C, tmp64, bid_ten2k64[q - 11]);
	} else {	// 20 <= q - 11 <= 23, and 10^(q-11) requires 128 bits
	  __mul_128x64_to_128 (C, tmp64, bid_ten2k128[q - 31]);
	}
	if (C1.w[1] > C.w[1] || (C1.w[1] == C.w[1] && C1.w[0] > C.w[0])) {
	  // set invalid flag
	  *pfpsf |= BID_INVALID_EXCEPTION;
	  // return Integer Indefinite
	  res = 0x80000000;
	  BID_RETURN_VAL (res);
	}
	// else cases that can be rounded to a 32-bit int fall through
	// to '1 <= q + exp <= 10'
      }
    } else {	// if n > 0 and q + exp = 10
      // if n >= 2^31 then n is too large
      // too large if c(0)c(1)...c(9).c(10)...c(q-1) >= 2^31
      // too large if 0.c(0)c(1)...c(q-1) * 10^11 >= 0x500000000, 1<=q<=34
      if (q <= 11) {
	tmp64 = C1.w[0] * bid_ten2k64[11 - q];	// C scaled up to 11-digit int
	// c(0)c(1)...c(9)c(10) or c(0)c(1)...c(q-1)0...0 (11 digits)
	if (tmp64 >= 0x500000000ull) {
	  // set invalid flag
	  *pfpsf |= BID_INVALID_EXCEPTION;
	  // return Integer Indefinite
	  res = 0x80000000;
	  BID_RETURN_VAL (res);
	}
	// else cases that can be rounded to a 32-bit int fall through
	// to '1 <= q + exp <= 10'
      } else {	// if (q > 11), i.e. 12 <= q <= 34 and so -24 <= exp <= -2
	// 0.c(0)c(1)...c(q-1) * 10^11 >= 0x500000000 <=>
	// C >= 0x500000000 * 10^(q-11) where 1 <= q - 11 <= 23
	// (scale 2^31 up)
	tmp64 = 0x500000000ull;
	if (q - 11 <= 19) {	// 1 <= q - 11 <= 19; 10^(q-11) requires 64 bits
	  __mul_64x64_to_128MACH (C, tmp64, bid_ten2k64[q - 11]);
	} else {	// 20 <= q - 11 <= 23, and 10^(q-11) requires 128 bits
	  __mul_128x64_to_128 (C, tmp64, bid_ten2k128[q - 31]);
	}
	if (C1.w[1] > C.w[1]
	    || (C1.w[1] == C.w[1] && C1.w[0] >= C.w[0])) {
	  // set invalid flag
	  *pfpsf |= BID_INVALID_EXCEPTION;
	  // return Integer Indefinite
	  res = 0x80000000;
	  BID_RETURN_VAL (res);
	}
	// else cases that can be rounded to a 32-bit int fall through
	// to '1 <= q + exp <= 10'
      }
    }
  }
  // n is not too large to be converted to int32: -2^31 <= n < 2^31
  // Note: some of the cases tested for above fall through to this point
  if ((q + exp) <= 0) {
    // n = +/-0.0...c(0)c(1)...c(q-1) or n = +/-0.c(0)c(1)...c(q-1)
    // return 0
    if (x_sign)
      res = 0xffffffff;
    else
      res = 0x00000000;
    BID_RETURN_VAL (res);
  } else {	// if (1 <= q + exp <= 10, 1 <= q <= 34, -33 <= exp <= 9)
    // -2^31 <= x <= -1 or 1 <= x < 2^31 so x can be rounded
    // toward negative infinity to a 32-bit signed integer
    if (exp < 0) {	// 2 <= q <= 34, -33 <= exp <= -1, 1 <= q + exp <= 10
      ind = -exp;	// 1 <= ind <= 33; ind is a synonym for 'x'
      // chop off ind digits from the lower part of C1
      // C1 = C1 + 1/2 * 10^ind where the result C1 fits in 127 bits
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
      // 1 <= x <= 33
      // kx = 10^(-x) = bid_ten2mk128[ind - 1]
      // C* = (C1 + 1/2 * 10^x) * 10^(-x)
      // the approximation of 10^(-x) was rounded up to 118 bits
      __mul_128x128_to_256 (P256, C1, bid_ten2mk128[ind - 1]);
      if (ind - 1 <= 21) {	// 0 <= ind - 1 <= 21
	Cstar.w[1] = P256.w[3];
	Cstar.w[0] = P256.w[2];
	fstar.w[3] = 0;
	fstar.w[2] = P256.w[2] & bid_maskhigh128[ind - 1];
	fstar.w[1] = P256.w[1];
	fstar.w[0] = P256.w[0];
      } else {	// 22 <= ind - 1 <= 33
	Cstar.w[1] = 0;
	Cstar.w[0] = P256.w[3];
	fstar.w[3] = P256.w[3] & bid_maskhigh128[ind - 1];
	fstar.w[2] = P256.w[2];
	fstar.w[1] = P256.w[1];
	fstar.w[0] = P256.w[0];
      }
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
      shift = bid_shiftright128[ind - 1];	// 0 <= shift <= 102
      if (ind - 1 <= 21) {	// 0 <= ind - 1 <= 21
	Cstar.w[0] =
	  (Cstar.w[0] >> shift) | (Cstar.w[1] << (64 - shift));
	// redundant, it will be 0! Cstar.w[1] = (Cstar.w[1] >> shift);
      } else {	// 22 <= ind - 1 <= 33
	Cstar.w[0] = (Cstar.w[0] >> (shift - 64));	// 2 <= shift - 64 <= 38
      }
      // determine inexactness of the rounding of C*
      // if (0 < f* - 1/2 < 10^(-x)) then
      //   the result is exact
      // else // if (f* - 1/2 > T*) then
      //   the result is inexact
      if (ind - 1 <= 2) {
	if (fstar.w[1] > 0x8000000000000000ull || (fstar.w[1] == 0x8000000000000000ull && fstar.w[0] > 0x0ull)) {	// f* > 1/2 and the result may be exact
	  tmp64 = fstar.w[1] - 0x8000000000000000ull;	// f* - 1/2
	  if (tmp64 > bid_ten2mk128trunc[ind - 1].w[1]
	      || (tmp64 == bid_ten2mk128trunc[ind - 1].w[1]
		  && fstar.w[0] >= bid_ten2mk128trunc[ind - 1].w[0])) {
	    is_inexact_lt_midpoint = 1;
	  }	// else the result is exact
	} else {	// the result is inexact; f2* <= 1/2
	  is_inexact_gt_midpoint = 1;
	}
      } else if (ind - 1 <= 21) {	// if 3 <= ind <= 21
	if (fstar.w[3] > 0x0 ||
	    (fstar.w[3] == 0x0 && fstar.w[2] > bid_onehalf128[ind - 1]) ||
	    (fstar.w[3] == 0x0 && fstar.w[2] == bid_onehalf128[ind - 1] &&
	     (fstar.w[1] || fstar.w[0]))) {
	  // f2* > 1/2 and the result may be exact
	  // Calculate f2* - 1/2
	  tmp64 = fstar.w[2] - bid_onehalf128[ind - 1];
	  tmp64A = fstar.w[3];
	  if (tmp64 > fstar.w[2])
	    tmp64A--;
	  if (tmp64A || tmp64
	      || fstar.w[1] > bid_ten2mk128trunc[ind - 1].w[1]
	      || (fstar.w[1] == bid_ten2mk128trunc[ind - 1].w[1]
		  && fstar.w[0] > bid_ten2mk128trunc[ind - 1].w[0])) {
	    is_inexact_lt_midpoint = 1;
	  }	// else the result is exact
	} else {	// the result is inexact; f2* <= 1/2
	  is_inexact_gt_midpoint = 1;
	}
      } else {	// if 22 <= ind <= 33
	if (fstar.w[3] > bid_onehalf128[ind - 1] ||
	    (fstar.w[3] == bid_onehalf128[ind - 1] &&
	     (fstar.w[2] || fstar.w[1] || fstar.w[0]))) {
	  // f2* > 1/2 and the result may be exact
	  // Calculate f2* - 1/2
	  tmp64 = fstar.w[3] - bid_onehalf128[ind - 1];
	  if (tmp64 || fstar.w[2]
	      || fstar.w[1] > bid_ten2mk128trunc[ind - 1].w[1]
	      || (fstar.w[1] == bid_ten2mk128trunc[ind - 1].w[1]
		  && fstar.w[0] > bid_ten2mk128trunc[ind - 1].w[0])) {
	    is_inexact_lt_midpoint = 1;
	  }	// else the result is exact
	} else {	// the result is inexact; f2* <= 1/2
	  is_inexact_gt_midpoint = 1;
	}
      }

      // if the result was a midpoint it was rounded away from zero, so
      // it will need a correction
      // check for midpoints
      if ((fstar.w[3] == 0) && (fstar.w[2] == 0)
	  && (fstar.w[1] || fstar.w[0])
	  && (fstar.w[1] < bid_ten2mk128trunc[ind - 1].w[1]
	      || (fstar.w[1] == bid_ten2mk128trunc[ind - 1].w[1]
		  && fstar.w[0] <= bid_ten2mk128trunc[ind - 1].w[0]))) {
	// the result is a midpoint; round to nearest
	if (Cstar.w[0] & 0x01) {	// Cstar.w[0] is odd; MP in [EVEN, ODD]
	  // if floor(C*) is odd C = floor(C*) - 1; the result >= 1
	  Cstar.w[0]--;	// Cstar.w[0] is now even
	  is_midpoint_gt_even = 1;
	  is_inexact_lt_midpoint = 0;
	  is_inexact_gt_midpoint = 0;
	} else {	// else MP in [ODD, EVEN]
	  is_midpoint_lt_even = 1;
	  is_inexact_lt_midpoint = 0;
	  is_inexact_gt_midpoint = 0;
	}
      }
      // general correction for RM
      if (x_sign && (is_midpoint_gt_even || is_inexact_lt_midpoint)) {
	Cstar.w[0] = Cstar.w[0] + 1;
      } else if (!x_sign
		 && (is_midpoint_lt_even || is_inexact_gt_midpoint)) {
	Cstar.w[0] = Cstar.w[0] - 1;
      } else {
	;	// the result is already correct
      }
      if (x_sign)
	res = -Cstar.w[0];
      else
	res = Cstar.w[0];
    } else if (exp == 0) {
      // 1 <= q <= 10
      // res = +/-C (exact)
      if (x_sign)
	res = -C1.w[0];
      else
	res = C1.w[0];
    } else {	// if (exp > 0) => 1 <= exp <= 9, 1 <= q < 9, 2 <= q + exp <= 10
      // res = +/-C * 10^exp (exact)
      if (x_sign)
	res = -C1.w[0] * bid_ten2k64[exp];
      else
	res = C1.w[0] * bid_ten2k64[exp];
    }
  }
}

BID_RETURN_VAL (res);
}


/*****************************************************************************
 *  BID128_to_int32_xfloor
 ****************************************************************************/

BID128_FUNCTION_ARG1_NORND_CUSTOMRESTYPE (int, bid128_to_int32_xfloor,
					  x)

     int res;
     BID_UINT64 x_sign;
     BID_UINT64 x_exp;
     int exp;			// unbiased exponent
  // Note: C1.w[1], C1.w[0] represent x_signif_hi, x_signif_lo (all are BID_UINT64)
     BID_UINT64 tmp64, tmp64A;
     BID_UI64DOUBLE tmp1;
     unsigned int x_nr_bits;
     int q, ind, shift;
     BID_UINT128 C1, C;
     BID_UINT128 Cstar;		// C* represents up to 34 decimal digits ~ 113 bits
     BID_UINT256 fstar;
     BID_UINT256 P256;
     int is_inexact_lt_midpoint = 0;
     int is_inexact_gt_midpoint = 0;
     int is_midpoint_lt_even = 0;
     int is_midpoint_gt_even = 0;

  // unpack x
x_sign = x.w[1] & MASK_SIGN;	// 0 for positive, MASK_SIGN for negative
x_exp = x.w[1] & MASK_EXP;	// biased and shifted left 49 bit positions
C1.w[1] = x.w[1] & MASK_COEFF;
C1.w[0] = x.w[0];

  // check for NaN or Infinity
if ((x.w[1] & MASK_SPECIAL) == MASK_SPECIAL) {
    // x is special
if ((x.w[1] & MASK_NAN) == MASK_NAN) {	// x is NAN
  if ((x.w[1] & MASK_SNAN) == MASK_SNAN) {	// x is SNAN
    // set invalid flag
    *pfpsf |= BID_INVALID_EXCEPTION;
    // return Integer Indefinite
    res = 0x80000000;
  } else {	// x is QNaN
    // set invalid flag
    *pfpsf |= BID_INVALID_EXCEPTION;
    // return Integer Indefinite
    res = 0x80000000;
  }
  BID_RETURN_VAL (res);
} else {	// x is not a NaN, so it must be infinity
  if (!x_sign) {	// x is +inf
    // set invalid flag
    *pfpsf |= BID_INVALID_EXCEPTION;
    // return Integer Indefinite
    res = 0x80000000;
  } else {	// x is -inf
    // set invalid flag
    *pfpsf |= BID_INVALID_EXCEPTION;
    // return Integer Indefinite
    res = 0x80000000;
  }
  BID_RETURN_VAL (res);
}
}
  // check for non-canonical values (after the check for special values)
if ((C1.w[1] > 0x0001ed09bead87c0ull)
    || (C1.w[1] == 0x0001ed09bead87c0ull
	&& (C1.w[0] > 0x378d8e63ffffffffull))
    || ((x.w[1] & 0x6000000000000000ull) == 0x6000000000000000ull)) {
  res = 0x00000000;
  BID_RETURN_VAL (res);
} else if ((C1.w[1] == 0x0ull) && (C1.w[0] == 0x0ull)) {
  // x is 0
  res = 0x00000000;
  BID_RETURN_VAL (res);
} else {	// x is not special and is not zero

  // q = nr. of decimal digits in x
  //  determine first the nr. of bits in x
  if (C1.w[1] == 0) {
    if (C1.w[0] >= 0x0020000000000000ull) {	// x >= 2^53
      // split the 64-bit value in two 32-bit halves to avoid rounding errors
	tmp1.d = (double) (C1.w[0] >> 32);	// exact conversion
	x_nr_bits =
	  33 + ((((unsigned int) (tmp1.ui64 >> 52)) & 0x7ff) - 0x3ff);
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
	|| (C1.w[1] == bid_nr_digits[x_nr_bits - 1].threshold_hi
	    && C1.w[0] >= bid_nr_digits[x_nr_bits - 1].threshold_lo))
      q++;
  }
  exp = (x_exp >> 49) - 6176;
  if ((q + exp) > 10) {	// x >= 10^10 ~= 2^33.2... (cannot fit in 32 bits)
    // set invalid flag
    *pfpsf |= BID_INVALID_EXCEPTION;
    // return Integer Indefinite
    res = 0x80000000;
    BID_RETURN_VAL (res);
  } else if ((q + exp) == 10) {	// x = c(0)c(1)...c(9).c(10)...c(q-1)
    // in this case 2^29.89... ~= 10^9 <= x < 10^10 ~= 2^33.2...
    // so x rounded to an integer may or may not fit in a signed 32-bit int
    // the cases that do not fit are identified here; the ones that fit
    // fall through and will be handled with other cases further,
    // under '1 <= q + exp <= 10'
    if (x_sign) {	// if n < 0 and q + exp = 10
      // if n < -2^31 then n is too large
      // too large if c(0)c(1)...c(9).c(10)...c(q-1) > 2^31
      // <=> 0.c(0)c(1)...c(q-1) * 10^11 > 0x500000000, 1<=q<=34
      if (q <= 11) {
	tmp64 = C1.w[0] * bid_ten2k64[11 - q];	// C scaled up to 11-digit int
	// c(0)c(1)...c(9)c(10) or c(0)c(1)...c(q-1)0...0 (11 digits)
	if (tmp64 > 0x500000000ull) {
	  // set invalid flag
	  *pfpsf |= BID_INVALID_EXCEPTION;
	  // return Integer Indefinite
	  res = 0x80000000;
	  BID_RETURN_VAL (res);
	}
	// else cases that can be rounded to a 32-bit int fall through
	// to '1 <= q + exp <= 10'
      } else {	// if (q > 11), i.e. 12 <= q <= 34 and so -24 <= exp <= -2
	// 0.c(0)c(1)...c(q-1) * 10^11 > 0x500000000 <=>
	// C > 0x500000000 * 10^(q-11) where 1 <= q - 11 <= 23
	// (scale 2^31 up)
	tmp64 = 0x500000000ull;
	if (q - 11 <= 19) {	// 1 <= q - 11 <= 19; 10^(q-11) requires 64 bits
	  __mul_64x64_to_128MACH (C, tmp64, bid_ten2k64[q - 11]);
	} else {	// 20 <= q - 11 <= 23, and 10^(q-11) requires 128 bits
	  __mul_128x64_to_128 (C, tmp64, bid_ten2k128[q - 31]);
	}
	if (C1.w[1] > C.w[1] || (C1.w[1] == C.w[1] && C1.w[0] > C.w[0])) {
	  // set invalid flag
	  *pfpsf |= BID_INVALID_EXCEPTION;
	  // return Integer Indefinite
	  res = 0x80000000;
	  BID_RETURN_VAL (res);
	}
	// else cases that can be rounded to a 32-bit int fall through
	// to '1 <= q + exp <= 10'
      }
    } else {	// if n > 0 and q + exp = 10
      // if n >= 2^31 then n is too large
      // too large if c(0)c(1)...c(9).c(10)...c(q-1) >= 2^31
      // too large if 0.c(0)c(1)...c(q-1) * 10^11 >= 0x500000000, 1<=q<=34
      if (q <= 11) {
	tmp64 = C1.w[0] * bid_ten2k64[11 - q];	// C scaled up to 11-digit int
	// c(0)c(1)...c(9)c(10) or c(0)c(1)...c(q-1)0...0 (11 digits)
	if (tmp64 >= 0x500000000ull) {
	  // set invalid flag
	  *pfpsf |= BID_INVALID_EXCEPTION;
	  // return Integer Indefinite
	  res = 0x80000000;
	  BID_RETURN_VAL (res);
	}
	// else cases that can be rounded to a 32-bit int fall through
	// to '1 <= q + exp <= 10'
      } else {	// if (q > 11), i.e. 12 <= q <= 34 and so -24 <= exp <= -2
	// 0.c(0)c(1)...c(q-1) * 10^11 >= 0x500000000 <=>
	// C >= 0x500000000 * 10^(q-11) where 1 <= q - 11 <= 23
	// (scale 2^31 up)
	tmp64 = 0x500000000ull;
	if (q - 11 <= 19) {	// 1 <= q - 11 <= 19; 10^(q-11) requires 64 bits
	  __mul_64x64_to_128MACH (C, tmp64, bid_ten2k64[q - 11]);
	} else {	// 20 <= q - 11 <= 23, and 10^(q-11) requires 128 bits
	  __mul_128x64_to_128 (C, tmp64, bid_ten2k128[q - 31]);
	}
	if (C1.w[1] > C.w[1]
	    || (C1.w[1] == C.w[1] && C1.w[0] >= C.w[0])) {
	  // set invalid flag
	  *pfpsf |= BID_INVALID_EXCEPTION;
	  // return Integer Indefinite
	  res = 0x80000000;
	  BID_RETURN_VAL (res);
	}
	// else cases that can be rounded to a 32-bit int fall through
	// to '1 <= q + exp <= 10'
      }
    }
  }
  // n is not too large to be converted to int32: -2^31 <= n < 2^31
  // Note: some of the cases tested for above fall through to this point
  if ((q + exp) <= 0) {
    // n = +/-0.0...c(0)c(1)...c(q-1) or n = +/-0.c(0)c(1)...c(q-1)
    // set inexact flag
    *pfpsf |= BID_INEXACT_EXCEPTION;
    // return 0
    if (x_sign)
      res = 0xffffffff;
    else
      res = 0x00000000;
    BID_RETURN_VAL (res);
  } else {	// if (1 <= q + exp <= 10, 1 <= q <= 34, -33 <= exp <= 9)
    // -2^31 <= x <= -1 or 1 <= x < 2^31 so x can be rounded
    // toward negative infinity to a 32-bit signed integer
    if (exp < 0) {	// 2 <= q <= 34, -33 <= exp <= -1, 1 <= q + exp <= 10
      ind = -exp;	// 1 <= ind <= 33; ind is a synonym for 'x'
      // chop off ind digits from the lower part of C1
      // C1 = C1 + 1/2 * 10^ind where the result C1 fits in 127 bits
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
      // 1 <= x <= 33
      // kx = 10^(-x) = bid_ten2mk128[ind - 1]
      // C* = (C1 + 1/2 * 10^x) * 10^(-x)
      // the approximation of 10^(-x) was rounded up to 118 bits
      __mul_128x128_to_256 (P256, C1, bid_ten2mk128[ind - 1]);
      if (ind - 1 <= 21) {	// 0 <= ind - 1 <= 21
	Cstar.w[1] = P256.w[3];
	Cstar.w[0] = P256.w[2];
	fstar.w[3] = 0;
	fstar.w[2] = P256.w[2] & bid_maskhigh128[ind - 1];
	fstar.w[1] = P256.w[1];
	fstar.w[0] = P256.w[0];
      } else {	// 22 <= ind - 1 <= 33
	Cstar.w[1] = 0;
	Cstar.w[0] = P256.w[3];
	fstar.w[3] = P256.w[3] & bid_maskhigh128[ind - 1];
	fstar.w[2] = P256.w[2];
	fstar.w[1] = P256.w[1];
	fstar.w[0] = P256.w[0];
      }
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
      shift = bid_shiftright128[ind - 1];	// 0 <= shift <= 102
      if (ind - 1 <= 21) {	// 0 <= ind - 1 <= 21
	Cstar.w[0] =
	  (Cstar.w[0] >> shift) | (Cstar.w[1] << (64 - shift));
	// redundant, it will be 0! Cstar.w[1] = (Cstar.w[1] >> shift);
      } else {	// 22 <= ind - 1 <= 33
	Cstar.w[0] = (Cstar.w[0] >> (shift - 64));	// 2 <= shift - 64 <= 38
      }
      // determine inexactness of the rounding of C*
      // if (0 < f* - 1/2 < 10^(-x)) then
      //   the result is exact
      // else // if (f* - 1/2 > T*) then
      //   the result is inexact
      if (ind - 1 <= 2) {
	if (fstar.w[1] > 0x8000000000000000ull || (fstar.w[1] == 0x8000000000000000ull && fstar.w[0] > 0x0ull)) {	// f* > 1/2 and the result may be exact
	  tmp64 = fstar.w[1] - 0x8000000000000000ull;	// f* - 1/2
	  if (tmp64 > bid_ten2mk128trunc[ind - 1].w[1]
	      || (tmp64 == bid_ten2mk128trunc[ind - 1].w[1]
		  && fstar.w[0] >= bid_ten2mk128trunc[ind - 1].w[0])) {
	    // set the inexact flag
	    *pfpsf |= BID_INEXACT_EXCEPTION;
	    is_inexact_lt_midpoint = 1;
	  }	// else the result is exact
	} else {	// the result is inexact; f2* <= 1/2
	  // set the inexact flag
	  *pfpsf |= BID_INEXACT_EXCEPTION;
	  is_inexact_gt_midpoint = 1;
	}
      } else if (ind - 1 <= 21) {	// if 3 <= ind <= 21
	if (fstar.w[3] > 0x0 ||
	    (fstar.w[3] == 0x0 && fstar.w[2] > bid_onehalf128[ind - 1]) ||
	    (fstar.w[3] == 0x0 && fstar.w[2] == bid_onehalf128[ind - 1] &&
	     (fstar.w[1] || fstar.w[0]))) {
	  // f2* > 1/2 and the result may be exact
	  // Calculate f2* - 1/2
	  tmp64 = fstar.w[2] - bid_onehalf128[ind - 1];
	  tmp64A = fstar.w[3];
	  if (tmp64 > fstar.w[2])
	    tmp64A--;
	  if (tmp64A || tmp64
	      || fstar.w[1] > bid_ten2mk128trunc[ind - 1].w[1]
	      || (fstar.w[1] == bid_ten2mk128trunc[ind - 1].w[1]
		  && fstar.w[0] > bid_ten2mk128trunc[ind - 1].w[0])) {
	    // set the inexact flag
	    *pfpsf |= BID_INEXACT_EXCEPTION;
	    is_inexact_lt_midpoint = 1;
	  }	// else the result is exact
	} else {	// the result is inexact; f2* <= 1/2
	  // set the inexact flag
	  *pfpsf |= BID_INEXACT_EXCEPTION;
	  is_inexact_gt_midpoint = 1;
	}
      } else {	// if 22 <= ind <= 33
	if (fstar.w[3] > bid_onehalf128[ind - 1] ||
	    (fstar.w[3] == bid_onehalf128[ind - 1] &&
	     (fstar.w[2] || fstar.w[1] || fstar.w[0]))) {
	  // f2* > 1/2 and the result may be exact
	  // Calculate f2* - 1/2
	  tmp64 = fstar.w[3] - bid_onehalf128[ind - 1];
	  if (tmp64 || fstar.w[2]
	      || fstar.w[1] > bid_ten2mk128trunc[ind - 1].w[1]
	      || (fstar.w[1] == bid_ten2mk128trunc[ind - 1].w[1]
		  && fstar.w[0] > bid_ten2mk128trunc[ind - 1].w[0])) {
	    // set the inexact flag
	    *pfpsf |= BID_INEXACT_EXCEPTION;
	    is_inexact_lt_midpoint = 1;
	  }	// else the result is exact
	} else {	// the result is inexact; f2* <= 1/2
	  // set the inexact flag
	  *pfpsf |= BID_INEXACT_EXCEPTION;
	  is_inexact_gt_midpoint = 1;
	}
      }

      // if the result was a midpoint it was rounded away from zero, so
      // it will need a correction
      // check for midpoints
      if ((fstar.w[3] == 0) && (fstar.w[2] == 0)
	  && (fstar.w[1] || fstar.w[0])
	  && (fstar.w[1] < bid_ten2mk128trunc[ind - 1].w[1]
	      || (fstar.w[1] == bid_ten2mk128trunc[ind - 1].w[1]
		  && fstar.w[0] <= bid_ten2mk128trunc[ind - 1].w[0]))) {
	// the result is a midpoint; round to nearest
	if (Cstar.w[0] & 0x01) {	// Cstar.w[0] is odd; MP in [EVEN, ODD]
	  // if floor(C*) is odd C = floor(C*) - 1; the result >= 1
	  Cstar.w[0]--;	// Cstar.w[0] is now even
	  is_midpoint_gt_even = 1;
	  is_inexact_lt_midpoint = 0;
	  is_inexact_gt_midpoint = 0;
	} else {	// else MP in [ODD, EVEN]
	  is_midpoint_lt_even = 1;
	  is_inexact_lt_midpoint = 0;
	  is_inexact_gt_midpoint = 0;
	}
      }
      // general correction for RM
      if (x_sign && (is_midpoint_gt_even || is_inexact_lt_midpoint)) {
	Cstar.w[0] = Cstar.w[0] + 1;
      } else if (!x_sign
		 && (is_midpoint_lt_even || is_inexact_gt_midpoint)) {
	Cstar.w[0] = Cstar.w[0] - 1;
      } else {
	;	// the result is already correct
      }
      if (x_sign)
	res = -Cstar.w[0];
      else
	res = Cstar.w[0];
    } else if (exp == 0) {
      // 1 <= q <= 10
      // res = +/-C (exact)
      if (x_sign)
	res = -C1.w[0];
      else
	res = C1.w[0];
    } else {	// if (exp > 0) => 1 <= exp <= 9, 1 <= q < 9, 2 <= q + exp <= 10
      // res = +/-C * 10^exp (exact)
      if (x_sign)
	res = -C1.w[0] * bid_ten2k64[exp];
      else
	res = C1.w[0] * bid_ten2k64[exp];
    }
  }
}

BID_RETURN_VAL (res);
}

/*****************************************************************************
 *  BID128_to_int32_ceil
 ****************************************************************************/

BID128_FUNCTION_ARG1_NORND_CUSTOMRESTYPE (int, bid128_to_int32_ceil, x)

     int res;
     BID_UINT64 x_sign;
     BID_UINT64 x_exp;
     int exp;			// unbiased exponent
  // Note: C1.w[1], C1.w[0] represent x_signif_hi, x_signif_lo (all are BID_UINT64)
     BID_UINT64 tmp64, tmp64A;
     BID_UI64DOUBLE tmp1;
     unsigned int x_nr_bits;
     int q, ind, shift;
     BID_UINT128 C1, C;
     BID_UINT128 Cstar;		// C* represents up to 34 decimal digits ~ 113 bits
     BID_UINT256 fstar;
     BID_UINT256 P256;
     int is_inexact_lt_midpoint = 0;
     int is_inexact_gt_midpoint = 0;
     int is_midpoint_lt_even = 0;
     int is_midpoint_gt_even = 0;

  // unpack x
x_sign = x.w[1] & MASK_SIGN;	// 0 for positive, MASK_SIGN for negative
x_exp = x.w[1] & MASK_EXP;	// biased and shifted left 49 bit positions
C1.w[1] = x.w[1] & MASK_COEFF;
C1.w[0] = x.w[0];

  // check for NaN or Infinity
if ((x.w[1] & MASK_SPECIAL) == MASK_SPECIAL) {
    // x is special
if ((x.w[1] & MASK_NAN) == MASK_NAN) {	// x is NAN
  if ((x.w[1] & MASK_SNAN) == MASK_SNAN) {	// x is SNAN
    // set invalid flag
    *pfpsf |= BID_INVALID_EXCEPTION;
    // return Integer Indefinite
    res = 0x80000000;
  } else {	// x is QNaN
    // set invalid flag
    *pfpsf |= BID_INVALID_EXCEPTION;
    // return Integer Indefinite
    res = 0x80000000;
  }
  BID_RETURN_VAL (res);
} else {	// x is not a NaN, so it must be infinity
  if (!x_sign) {	// x is +inf
    // set invalid flag
    *pfpsf |= BID_INVALID_EXCEPTION;
    // return Integer Indefinite
    res = 0x80000000;
  } else {	// x is -inf
    // set invalid flag
    *pfpsf |= BID_INVALID_EXCEPTION;
    // return Integer Indefinite
    res = 0x80000000;
  }
  BID_RETURN_VAL (res);
}
}
  // check for non-canonical values (after the check for special values)
if ((C1.w[1] > 0x0001ed09bead87c0ull)
    || (C1.w[1] == 0x0001ed09bead87c0ull
	&& (C1.w[0] > 0x378d8e63ffffffffull))
    || ((x.w[1] & 0x6000000000000000ull) == 0x6000000000000000ull)) {
  res = 0x00000000;
  BID_RETURN_VAL (res);
} else if ((C1.w[1] == 0x0ull) && (C1.w[0] == 0x0ull)) {
  // x is 0
  res = 0x00000000;
  BID_RETURN_VAL (res);
} else {	// x is not special and is not zero

  // q = nr. of decimal digits in x
  //  determine first the nr. of bits in x
  if (C1.w[1] == 0) {
    if (C1.w[0] >= 0x0020000000000000ull) {	// x >= 2^53
      // split the 64-bit value in two 32-bit halves to avoid rounding errors
	tmp1.d = (double) (C1.w[0] >> 32);	// exact conversion
	x_nr_bits =
	  33 + ((((unsigned int) (tmp1.ui64 >> 52)) & 0x7ff) - 0x3ff);
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
	|| (C1.w[1] == bid_nr_digits[x_nr_bits - 1].threshold_hi
	    && C1.w[0] >= bid_nr_digits[x_nr_bits - 1].threshold_lo))
      q++;
  }
  exp = (x_exp >> 49) - 6176;
  if ((q + exp) > 10) {	// x >= 10^10 ~= 2^33.2... (cannot fit in 32 bits)
    // set invalid flag
    *pfpsf |= BID_INVALID_EXCEPTION;
    // return Integer Indefinite
    res = 0x80000000;
    BID_RETURN_VAL (res);
  } else if ((q + exp) == 10) {	// x = c(0)c(1)...c(9).c(10)...c(q-1)
    // in this case 2^29.89... ~= 10^9 <= x < 10^10 ~= 2^33.2...
    // so x rounded to an integer may or may not fit in a signed 32-bit int
    // the cases that do not fit are identified here; the ones that fit
    // fall through and will be handled with other cases further,
    // under '1 <= q + exp <= 10'
    if (x_sign) {	// if n < 0 and q + exp = 10
      // if n <= -2^31-1 then n is too large
      // too large if c(0)c(1)...c(9).c(10)...c(q-1) >= 2^31+1
      // <=> 0.c(0)c(1)...c(q-1) * 10^11 >= 0x50000000a, 1<=q<=34
      if (q <= 11) {
	tmp64 = C1.w[0] * bid_ten2k64[11 - q];	// C scaled up to 11-digit int
	// c(0)c(1)...c(9)c(10) or c(0)c(1)...c(q-1)0...0 (11 digits)
	if (tmp64 >= 0x50000000aull) {
	  // set invalid flag
	  *pfpsf |= BID_INVALID_EXCEPTION;
	  // return Integer Indefinite
	  res = 0x80000000;
	  BID_RETURN_VAL (res);
	}
	// else cases that can be rounded to a 32-bit int fall through
	// to '1 <= q + exp <= 10'
      } else {	// if (q > 11), i.e. 12 <= q <= 34 and so -24 <= exp <= -2
	// 0.c(0)c(1)...c(q-1) * 10^11 >= 0x50000000a <=>
	// C >= 0x50000000a * 10^(q-11) where 1 <= q - 11 <= 23
	// (scale 2^31+1 up)
	tmp64 = 0x50000000aull;
	if (q - 11 <= 19) {	// 1 <= q - 11 <= 19; 10^(q-11) requires 64 bits
	  __mul_64x64_to_128MACH (C, tmp64, bid_ten2k64[q - 11]);
	} else {	// 20 <= q - 11 <= 23, and 10^(q-11) requires 128 bits
	  __mul_128x64_to_128 (C, tmp64, bid_ten2k128[q - 31]);
	}
	if (C1.w[1] > C.w[1]
	    || (C1.w[1] == C.w[1] && C1.w[0] >= C.w[0])) {
	  // set invalid flag
	  *pfpsf |= BID_INVALID_EXCEPTION;
	  // return Integer Indefinite
	  res = 0x80000000;
	  BID_RETURN_VAL (res);
	}
	// else cases that can be rounded to a 32-bit int fall through
	// to '1 <= q + exp <= 10'
      }
    } else {	// if n > 0 and q + exp = 10
      // if n > 2^31 - 1 then n is too large
      // too large if c(0)c(1)...c(9).c(10)...c(q-1) > 2^31 - 1
      // too large if 0.c(0)c(1)...c(q-1) * 10^11 > 0x4fffffff6, 1<=q<=34
      if (q <= 11) {
	tmp64 = C1.w[0] * bid_ten2k64[11 - q];	// C scaled up to 11-digit int
	// c(0)c(1)...c(9)c(10) or c(0)c(1)...c(q-1)0...0 (11 digits)
	if (tmp64 > 0x4fffffff6ull) {
	  // set invalid flag
	  *pfpsf |= BID_INVALID_EXCEPTION;
	  // return Integer Indefinite
	  res = 0x80000000;
	  BID_RETURN_VAL (res);
	}
	// else cases that can be rounded to a 32-bit int fall through
	// to '1 <= q + exp <= 10'
      } else {	// if (q > 11), i.e. 12 <= q <= 34 and so -24 <= exp <= -2
	// 0.c(0)c(1)...c(q-1) * 10^11 > 0x4fffffff6 <=>
	// C > 0x4fffffff6 * 10^(q-11) where 1 <= q - 11 <= 23
	// (scale 2^31 up)
	tmp64 = 0x4fffffff6ull;
	if (q - 11 <= 19) {	// 1 <= q - 11 <= 19; 10^(q-11) requires 64 bits
	  __mul_64x64_to_128MACH (C, tmp64, bid_ten2k64[q - 11]);
	} else {	// 20 <= q - 11 <= 23, and 10^(q-11) requires 128 bits
	  __mul_128x64_to_128 (C, tmp64, bid_ten2k128[q - 31]);
	}
	if (C1.w[1] > C.w[1] || (C1.w[1] == C.w[1] && C1.w[0] > C.w[0])) {
	  // set invalid flag
	  *pfpsf |= BID_INVALID_EXCEPTION;
	  // return Integer Indefinite
	  res = 0x80000000;
	  BID_RETURN_VAL (res);
	}
	// else cases that can be rounded to a 32-bit int fall through
	// to '1 <= q + exp <= 10'
      }
    }
  }
  // n is not too large to be converted to int32: -2^31-1 < n <= 2^31-1
  // Note: some of the cases tested for above fall through to this point
  if ((q + exp) <= 0) {
    // n = +/-0.0...c(0)c(1)...c(q-1) or n = +/-0.c(0)c(1)...c(q-1)
    // return 0
    if (x_sign)
      res = 0x00000000;
    else
      res = 0x00000001;
    BID_RETURN_VAL (res);
  } else {	// if (1 <= q + exp <= 10, 1 <= q <= 34, -33 <= exp <= 9)
    // -2^31-1 < x <= -1 or 1 <= x <= 2^31-1 so x can be rounded
    // toward positive infinity to a 32-bit signed integer
    if (exp < 0) {	// 2 <= q <= 34, -33 <= exp <= -1, 1 <= q + exp <= 10
      ind = -exp;	// 1 <= ind <= 33; ind is a synonym for 'x'
      // chop off ind digits from the lower part of C1
      // C1 = C1 + 1/2 * 10^ind where the result C1 fits in 127 bits
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
      // 1 <= x <= 33
      // kx = 10^(-x) = bid_ten2mk128[ind - 1]
      // C* = (C1 + 1/2 * 10^x) * 10^(-x)
      // the approximation of 10^(-x) was rounded up to 118 bits
      __mul_128x128_to_256 (P256, C1, bid_ten2mk128[ind - 1]);
      if (ind - 1 <= 21) {	// 0 <= ind - 1 <= 21
	Cstar.w[1] = P256.w[3];
	Cstar.w[0] = P256.w[2];
	fstar.w[3] = 0;
	fstar.w[2] = P256.w[2] & bid_maskhigh128[ind - 1];
	fstar.w[1] = P256.w[1];
	fstar.w[0] = P256.w[0];
      } else {	// 22 <= ind - 1 <= 33
	Cstar.w[1] = 0;
	Cstar.w[0] = P256.w[3];
	fstar.w[3] = P256.w[3] & bid_maskhigh128[ind - 1];
	fstar.w[2] = P256.w[2];
	fstar.w[1] = P256.w[1];
	fstar.w[0] = P256.w[0];
      }
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
      shift = bid_shiftright128[ind - 1];	// 0 <= shift <= 102
      if (ind - 1 <= 21) {	// 0 <= ind - 1 <= 21
	Cstar.w[0] =
	  (Cstar.w[0] >> shift) | (Cstar.w[1] << (64 - shift));
	// redundant, it will be 0! Cstar.w[1] = (Cstar.w[1] >> shift);
      } else {	// 22 <= ind - 1 <= 33
	Cstar.w[0] = (Cstar.w[0] >> (shift - 64));	// 2 <= shift - 64 <= 38
      }
      // determine inexactness of the rounding of C*
      // if (0 < f* - 1/2 < 10^(-x)) then
      //   the result is exact
      // else // if (f* - 1/2 > T*) then
      //   the result is inexact
      if (ind - 1 <= 2) {
	if (fstar.w[1] > 0x8000000000000000ull || (fstar.w[1] == 0x8000000000000000ull && fstar.w[0] > 0x0ull)) {	// f* > 1/2 and the result may be exact
	  tmp64 = fstar.w[1] - 0x8000000000000000ull;	// f* - 1/2
	  if (tmp64 > bid_ten2mk128trunc[ind - 1].w[1]
	      || (tmp64 == bid_ten2mk128trunc[ind - 1].w[1]
		  && fstar.w[0] >= bid_ten2mk128trunc[ind - 1].w[0])) {
	    is_inexact_lt_midpoint = 1;
	  }	// else the result is exact
	} else {	// the result is inexact; f2* <= 1/2
	  is_inexact_gt_midpoint = 1;
	}
      } else if (ind - 1 <= 21) {	// if 3 <= ind <= 21
	if (fstar.w[3] > 0x0 ||
	    (fstar.w[3] == 0x0 && fstar.w[2] > bid_onehalf128[ind - 1]) ||
	    (fstar.w[3] == 0x0 && fstar.w[2] == bid_onehalf128[ind - 1] &&
	     (fstar.w[1] || fstar.w[0]))) {
	  // f2* > 1/2 and the result may be exact
	  // Calculate f2* - 1/2
	  tmp64 = fstar.w[2] - bid_onehalf128[ind - 1];
	  tmp64A = fstar.w[3];
	  if (tmp64 > fstar.w[2])
	    tmp64A--;
	  if (tmp64A || tmp64
	      || fstar.w[1] > bid_ten2mk128trunc[ind - 1].w[1]
	      || (fstar.w[1] == bid_ten2mk128trunc[ind - 1].w[1]
		  && fstar.w[0] > bid_ten2mk128trunc[ind - 1].w[0])) {
	    is_inexact_lt_midpoint = 1;
	  }	// else the result is exact
	} else {	// the result is inexact; f2* <= 1/2
	  is_inexact_gt_midpoint = 1;
	}
      } else {	// if 22 <= ind <= 33
	if (fstar.w[3] > bid_onehalf128[ind - 1] ||
	    (fstar.w[3] == bid_onehalf128[ind - 1] &&
	     (fstar.w[2] || fstar.w[1] || fstar.w[0]))) {
	  // f2* > 1/2 and the result may be exact
	  // Calculate f2* - 1/2
	  tmp64 = fstar.w[3] - bid_onehalf128[ind - 1];
	  if (tmp64 || fstar.w[2]
	      || fstar.w[1] > bid_ten2mk128trunc[ind - 1].w[1]
	      || (fstar.w[1] == bid_ten2mk128trunc[ind - 1].w[1]
		  && fstar.w[0] > bid_ten2mk128trunc[ind - 1].w[0])) {
	    is_inexact_lt_midpoint = 1;
	  }	// else the result is exact
	} else {	// the result is inexact; f2* <= 1/2
	  is_inexact_gt_midpoint = 1;
	}
      }

      // if the result was a midpoint it was rounded away from zero, so
      // it will need a correction
      // check for midpoints
      if ((fstar.w[3] == 0) && (fstar.w[2] == 0)
	  && (fstar.w[1] || fstar.w[0])
	  && (fstar.w[1] < bid_ten2mk128trunc[ind - 1].w[1]
	      || (fstar.w[1] == bid_ten2mk128trunc[ind - 1].w[1]
		  && fstar.w[0] <= bid_ten2mk128trunc[ind - 1].w[0]))) {
	// the result is a midpoint; round to nearest
	if (Cstar.w[0] & 0x01) {	// Cstar.w[0] is odd; MP in [EVEN, ODD]
	  // if floor(C*) is odd C = floor(C*) - 1; the result >= 1
	  Cstar.w[0]--;	// Cstar.w[0] is now even
	  is_midpoint_gt_even = 1;
	  is_inexact_lt_midpoint = 0;
	  is_inexact_gt_midpoint = 0;
	} else {	// else MP in [ODD, EVEN]
	  is_midpoint_lt_even = 1;
	  is_inexact_lt_midpoint = 0;
	  is_inexact_gt_midpoint = 0;
	}
      }
      // general correction for RM
      if (x_sign && (is_midpoint_lt_even || is_inexact_gt_midpoint)) {
	Cstar.w[0] = Cstar.w[0] - 1;
      } else if (!x_sign
		 && (is_midpoint_gt_even || is_inexact_lt_midpoint)) {
	Cstar.w[0] = Cstar.w[0] + 1;
      } else {
	;	// the result is already correct
      }
      if (x_sign)
	res = -Cstar.w[0];
      else
	res = Cstar.w[0];
    } else if (exp == 0) {
      // 1 <= q <= 10
      // res = +/-C (exact)
      if (x_sign)
	res = -C1.w[0];
      else
	res = C1.w[0];
    } else {	// if (exp > 0) => 1 <= exp <= 9, 1 <= q < 9, 2 <= q + exp <= 10
      // res = +/-C * 10^exp (exact)
      if (x_sign)
	res = -C1.w[0] * bid_ten2k64[exp];
      else
	res = C1.w[0] * bid_ten2k64[exp];
    }
  }
}

BID_RETURN_VAL (res);
}

/*****************************************************************************
 *  BID128_to_int32_xceil
 ****************************************************************************/

BID128_FUNCTION_ARG1_NORND_CUSTOMRESTYPE (int, bid128_to_int32_xceil, x)

     int res;
     BID_UINT64 x_sign;
     BID_UINT64 x_exp;
     int exp;			// unbiased exponent
  // Note: C1.w[1], C1.w[0] represent x_signif_hi, x_signif_lo (all are BID_UINT64)
     BID_UINT64 tmp64, tmp64A;
     BID_UI64DOUBLE tmp1;
     unsigned int x_nr_bits;
     int q, ind, shift;
     BID_UINT128 C1, C;
     BID_UINT128 Cstar;		// C* represents up to 34 decimal digits ~ 113 bits
     BID_UINT256 fstar;
     BID_UINT256 P256;
     int is_inexact_lt_midpoint = 0;
     int is_inexact_gt_midpoint = 0;
     int is_midpoint_lt_even = 0;
     int is_midpoint_gt_even = 0;

  // unpack x
x_sign = x.w[1] & MASK_SIGN;	// 0 for positive, MASK_SIGN for negative
x_exp = x.w[1] & MASK_EXP;	// biased and shifted left 49 bit positions
C1.w[1] = x.w[1] & MASK_COEFF;
C1.w[0] = x.w[0];

  // check for NaN or Infinity
if ((x.w[1] & MASK_SPECIAL) == MASK_SPECIAL) {
    // x is special
if ((x.w[1] & MASK_NAN) == MASK_NAN) {	// x is NAN
  if ((x.w[1] & MASK_SNAN) == MASK_SNAN) {	// x is SNAN
    // set invalid flag
    *pfpsf |= BID_INVALID_EXCEPTION;
    // return Integer Indefinite
    res = 0x80000000;
  } else {	// x is QNaN
    // set invalid flag
    *pfpsf |= BID_INVALID_EXCEPTION;
    // return Integer Indefinite
    res = 0x80000000;
  }
  BID_RETURN_VAL (res);
} else {	// x is not a NaN, so it must be infinity
  if (!x_sign) {	// x is +inf
    // set invalid flag
    *pfpsf |= BID_INVALID_EXCEPTION;
    // return Integer Indefinite
    res = 0x80000000;
  } else {	// x is -inf
    // set invalid flag
    *pfpsf |= BID_INVALID_EXCEPTION;
    // return Integer Indefinite
    res = 0x80000000;
  }
  BID_RETURN_VAL (res);
}
}
  // check for non-canonical values (after the check for special values)
if ((C1.w[1] > 0x0001ed09bead87c0ull)
    || (C1.w[1] == 0x0001ed09bead87c0ull
	&& (C1.w[0] > 0x378d8e63ffffffffull))
    || ((x.w[1] & 0x6000000000000000ull) == 0x6000000000000000ull)) {
  res = 0x00000000;
  BID_RETURN_VAL (res);
} else if ((C1.w[1] == 0x0ull) && (C1.w[0] == 0x0ull)) {
  // x is 0
  res = 0x00000000;
  BID_RETURN_VAL (res);
} else {	// x is not special and is not zero

  // q = nr. of decimal digits in x
  //  determine first the nr. of bits in x
  if (C1.w[1] == 0) {
    if (C1.w[0] >= 0x0020000000000000ull) {	// x >= 2^53
      // split the 64-bit value in two 32-bit halves to avoid rounding errors
	tmp1.d = (double) (C1.w[0] >> 32);	// exact conversion
	x_nr_bits =
	  33 + ((((unsigned int) (tmp1.ui64 >> 52)) & 0x7ff) - 0x3ff);
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
	|| (C1.w[1] == bid_nr_digits[x_nr_bits - 1].threshold_hi
	    && C1.w[0] >= bid_nr_digits[x_nr_bits - 1].threshold_lo))
      q++;
  }
  exp = (x_exp >> 49) - 6176;
  if ((q + exp) > 10) {	// x >= 10^10 ~= 2^33.2... (cannot fit in 32 bits)
    // set invalid flag
    *pfpsf |= BID_INVALID_EXCEPTION;
    // return Integer Indefinite
    res = 0x80000000;
    BID_RETURN_VAL (res);
  } else if ((q + exp) == 10) {	// x = c(0)c(1)...c(9).c(10)...c(q-1)
    // in this case 2^29.89... ~= 10^9 <= x < 10^10 ~= 2^33.2...
    // so x rounded to an integer may or may not fit in a signed 32-bit int
    // the cases that do not fit are identified here; the ones that fit
    // fall through and will be handled with other cases further,
    // under '1 <= q + exp <= 10'
    if (x_sign) {	// if n < 0 and q + exp = 10
      // if n <= -2^31-1 then n is too large
      // too large if c(0)c(1)...c(9).c(10)...c(q-1) >= 2^31+1
      // <=> 0.c(0)c(1)...c(q-1) * 10^11 >= 0x50000000a, 1<=q<=34
      if (q <= 11) {
	tmp64 = C1.w[0] * bid_ten2k64[11 - q];	// C scaled up to 11-digit int
	// c(0)c(1)...c(9)c(10) or c(0)c(1)...c(q-1)0...0 (11 digits)
	if (tmp64 >= 0x50000000aull) {
	  // set invalid flag
	  *pfpsf |= BID_INVALID_EXCEPTION;
	  // return Integer Indefinite
	  res = 0x80000000;
	  BID_RETURN_VAL (res);
	}
	// else cases that can be rounded to a 32-bit int fall through
	// to '1 <= q + exp <= 10'
      } else {	// if (q > 11), i.e. 12 <= q <= 34 and so -24 <= exp <= -2
	// 0.c(0)c(1)...c(q-1) * 10^11 >= 0x50000000a <=>
	// C >= 0x50000000a * 10^(q-11) where 1 <= q - 11 <= 23
	// (scale 2^31+1 up)
	tmp64 = 0x50000000aull;
	if (q - 11 <= 19) {	// 1 <= q - 11 <= 19; 10^(q-11) requires 64 bits
	  __mul_64x64_to_128MACH (C, tmp64, bid_ten2k64[q - 11]);
	} else {	// 20 <= q - 11 <= 23, and 10^(q-11) requires 128 bits
	  __mul_128x64_to_128 (C, tmp64, bid_ten2k128[q - 31]);
	}
	if (C1.w[1] > C.w[1]
	    || (C1.w[1] == C.w[1] && C1.w[0] >= C.w[0])) {
	  // set invalid flag
	  *pfpsf |= BID_INVALID_EXCEPTION;
	  // return Integer Indefinite
	  res = 0x80000000;
	  BID_RETURN_VAL (res);
	}
	// else cases that can be rounded to a 32-bit int fall through
	// to '1 <= q + exp <= 10'
      }
    } else {	// if n > 0 and q + exp = 10
      // if n > 2^31 - 1 then n is too large
      // too large if c(0)c(1)...c(9).c(10)...c(q-1) > 2^31 - 1
      // too large if 0.c(0)c(1)...c(q-1) * 10^11 > 0x4fffffff6, 1<=q<=34
      if (q <= 11) {
	tmp64 = C1.w[0] * bid_ten2k64[11 - q];	// C scaled up to 11-digit int
	// c(0)c(1)...c(9)c(10) or c(0)c(1)...c(q-1)0...0 (11 digits)
	if (tmp64 > 0x4fffffff6ull) {
	  // set invalid flag
	  *pfpsf |= BID_INVALID_EXCEPTION;
	  // return Integer Indefinite
	  res = 0x80000000;
	  BID_RETURN_VAL (res);
	}
	// else cases that can be rounded to a 32-bit int fall through
	// to '1 <= q + exp <= 10'
      } else {	// if (q > 11), i.e. 12 <= q <= 34 and so -24 <= exp <= -2
	// 0.c(0)c(1)...c(q-1) * 10^11 > 0x4fffffff6 <=>
	// C > 0x4fffffff6 * 10^(q-11) where 1 <= q - 11 <= 23
	// (scale 2^31 up)
	tmp64 = 0x4fffffff6ull;
	if (q - 11 <= 19) {	// 1 <= q - 11 <= 19; 10^(q-11) requires 64 bits
	  __mul_64x64_to_128MACH (C, tmp64, bid_ten2k64[q - 11]);
	} else {	// 20 <= q - 11 <= 23, and 10^(q-11) requires 128 bits
	  __mul_128x64_to_128 (C, tmp64, bid_ten2k128[q - 31]);
	}
	if (C1.w[1] > C.w[1] || (C1.w[1] == C.w[1] && C1.w[0] > C.w[0])) {
	  // set invalid flag
	  *pfpsf |= BID_INVALID_EXCEPTION;
	  // return Integer Indefinite
	  res = 0x80000000;
	  BID_RETURN_VAL (res);
	}
	// else cases that can be rounded to a 32-bit int fall through
	// to '1 <= q + exp <= 10'
      }
    }
  }
  // n is not too large to be converted to int32: -2^31-1 < n <= 2^31-1
  // Note: some of the cases tested for above fall through to this point
  if ((q + exp) <= 0) {
    // n = +/-0.0...c(0)c(1)...c(q-1) or n = +/-0.c(0)c(1)...c(q-1)
    // set inexact flag
    *pfpsf |= BID_INEXACT_EXCEPTION;
    // return 0
    if (x_sign)
      res = 0x00000000;
    else
      res = 0x00000001;
    BID_RETURN_VAL (res);
  } else {	// if (1 <= q + exp <= 10, 1 <= q <= 34, -33 <= exp <= 9)
    // -2^31-1 < x <= -1 or 1 <= x <= 2^31-1 so x can be rounded
    // toward positive infinity to a 32-bit signed integer
    if (exp < 0) {	// 2 <= q <= 34, -33 <= exp <= -1, 1 <= q + exp <= 10
      ind = -exp;	// 1 <= ind <= 33; ind is a synonym for 'x'
      // chop off ind digits from the lower part of C1
      // C1 = C1 + 1/2 * 10^ind where the result C1 fits in 127 bits
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
      // 1 <= x <= 33
      // kx = 10^(-x) = bid_ten2mk128[ind - 1]
      // C* = (C1 + 1/2 * 10^x) * 10^(-x)
      // the approximation of 10^(-x) was rounded up to 118 bits
      __mul_128x128_to_256 (P256, C1, bid_ten2mk128[ind - 1]);
      if (ind - 1 <= 21) {	// 0 <= ind - 1 <= 21
	Cstar.w[1] = P256.w[3];
	Cstar.w[0] = P256.w[2];
	fstar.w[3] = 0;
	fstar.w[2] = P256.w[2] & bid_maskhigh128[ind - 1];
	fstar.w[1] = P256.w[1];
	fstar.w[0] = P256.w[0];
      } else {	// 22 <= ind - 1 <= 33
	Cstar.w[1] = 0;
	Cstar.w[0] = P256.w[3];
	fstar.w[3] = P256.w[3] & bid_maskhigh128[ind - 1];
	fstar.w[2] = P256.w[2];
	fstar.w[1] = P256.w[1];
	fstar.w[0] = P256.w[0];
      }
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
      shift = bid_shiftright128[ind - 1];	// 0 <= shift <= 102
      if (ind - 1 <= 21) {	// 0 <= ind - 1 <= 21
	Cstar.w[0] =
	  (Cstar.w[0] >> shift) | (Cstar.w[1] << (64 - shift));
	// redundant, it will be 0! Cstar.w[1] = (Cstar.w[1] >> shift);
      } else {	// 22 <= ind - 1 <= 33
	Cstar.w[0] = (Cstar.w[0] >> (shift - 64));	// 2 <= shift - 64 <= 38
      }
      // determine inexactness of the rounding of C*
      // if (0 < f* - 1/2 < 10^(-x)) then
      //   the result is exact
      // else // if (f* - 1/2 > T*) then
      //   the result is inexact
      if (ind - 1 <= 2) {
	if (fstar.w[1] > 0x8000000000000000ull || (fstar.w[1] == 0x8000000000000000ull && fstar.w[0] > 0x0ull)) {	// f* > 1/2 and the result may be exact
	  tmp64 = fstar.w[1] - 0x8000000000000000ull;	// f* - 1/2
	  if (tmp64 > bid_ten2mk128trunc[ind - 1].w[1]
	      || (tmp64 == bid_ten2mk128trunc[ind - 1].w[1]
		  && fstar.w[0] >= bid_ten2mk128trunc[ind - 1].w[0])) {
	    // set the inexact flag
	    *pfpsf |= BID_INEXACT_EXCEPTION;
	    is_inexact_lt_midpoint = 1;
	  }	// else the result is exact
	} else {	// the result is inexact; f2* <= 1/2
	  // set the inexact flag
	  *pfpsf |= BID_INEXACT_EXCEPTION;
	  is_inexact_gt_midpoint = 1;
	}
      } else if (ind - 1 <= 21) {	// if 3 <= ind <= 21
	if (fstar.w[3] > 0x0 ||
	    (fstar.w[3] == 0x0 && fstar.w[2] > bid_onehalf128[ind - 1]) ||
	    (fstar.w[3] == 0x0 && fstar.w[2] == bid_onehalf128[ind - 1] &&
	     (fstar.w[1] || fstar.w[0]))) {
	  // f2* > 1/2 and the result may be exact
	  // Calculate f2* - 1/2
	  tmp64 = fstar.w[2] - bid_onehalf128[ind - 1];
	  tmp64A = fstar.w[3];
	  if (tmp64 > fstar.w[2])
	    tmp64A--;
	  if (tmp64A || tmp64
	      || fstar.w[1] > bid_ten2mk128trunc[ind - 1].w[1]
	      || (fstar.w[1] == bid_ten2mk128trunc[ind - 1].w[1]
		  && fstar.w[0] > bid_ten2mk128trunc[ind - 1].w[0])) {
	    // set the inexact flag
	    *pfpsf |= BID_INEXACT_EXCEPTION;
	    is_inexact_lt_midpoint = 1;
	  }	// else the result is exact
	} else {	// the result is inexact; f2* <= 1/2
	  // set the inexact flag
	  *pfpsf |= BID_INEXACT_EXCEPTION;
	  is_inexact_gt_midpoint = 1;
	}
      } else {	// if 22 <= ind <= 33
	if (fstar.w[3] > bid_onehalf128[ind - 1] ||
	    (fstar.w[3] == bid_onehalf128[ind - 1] &&
	     (fstar.w[2] || fstar.w[1] || fstar.w[0]))) {
	  // f2* > 1/2 and the result may be exact
	  // Calculate f2* - 1/2
	  tmp64 = fstar.w[3] - bid_onehalf128[ind - 1];
	  if (tmp64 || fstar.w[2]
	      || fstar.w[1] > bid_ten2mk128trunc[ind - 1].w[1]
	      || (fstar.w[1] == bid_ten2mk128trunc[ind - 1].w[1]
		  && fstar.w[0] > bid_ten2mk128trunc[ind - 1].w[0])) {
	    // set the inexact flag
	    *pfpsf |= BID_INEXACT_EXCEPTION;
	    is_inexact_lt_midpoint = 1;
	  }	// else the result is exact
	} else {	// the result is inexact; f2* <= 1/2
	  // set the inexact flag
	  *pfpsf |= BID_INEXACT_EXCEPTION;
	  is_inexact_gt_midpoint = 1;
	}
      }

      // if the result was a midpoint it was rounded away from zero, so
      // it will need a correction
      // check for midpoints
      if ((fstar.w[3] == 0) && (fstar.w[2] == 0)
	  && (fstar.w[1] || fstar.w[0])
	  && (fstar.w[1] < bid_ten2mk128trunc[ind - 1].w[1]
	      || (fstar.w[1] == bid_ten2mk128trunc[ind - 1].w[1]
		  && fstar.w[0] <= bid_ten2mk128trunc[ind - 1].w[0]))) {
	// the result is a midpoint; round to nearest
	if (Cstar.w[0] & 0x01) {	// Cstar.w[0] is odd; MP in [EVEN, ODD]
	  // if floor(C*) is odd C = floor(C*) - 1; the result >= 1
	  Cstar.w[0]--;	// Cstar.w[0] is now even
	  is_midpoint_gt_even = 1;
	  is_inexact_lt_midpoint = 0;
	  is_inexact_gt_midpoint = 0;
	} else {	// else MP in [ODD, EVEN]
	  is_midpoint_lt_even = 1;
	  is_inexact_lt_midpoint = 0;
	  is_inexact_gt_midpoint = 0;
	}
      }
      // general correction for RM
      if (x_sign && (is_midpoint_lt_even || is_inexact_gt_midpoint)) {
	Cstar.w[0] = Cstar.w[0] - 1;
      } else if (!x_sign
		 && (is_midpoint_gt_even || is_inexact_lt_midpoint)) {
	Cstar.w[0] = Cstar.w[0] + 1;
      } else {
	;	// the result is already correct
      }
      if (x_sign)
	res = -Cstar.w[0];
      else
	res = Cstar.w[0];
    } else if (exp == 0) {
      // 1 <= q <= 10
      // res = +/-C (exact)
      if (x_sign)
	res = -C1.w[0];
      else
	res = C1.w[0];
    } else {	// if (exp > 0) => 1 <= exp <= 9, 1 <= q < 9, 2 <= q + exp <= 10
      // res = +/-C * 10^exp (exact)
      if (x_sign)
	res = -C1.w[0] * bid_ten2k64[exp];
      else
	res = C1.w[0] * bid_ten2k64[exp];
    }
  }
}

BID_RETURN_VAL (res);
}

/*****************************************************************************
 *  BID128_to_int32_int
 ****************************************************************************/

BID128_FUNCTION_ARG1_NORND_CUSTOMRESTYPE (int, bid128_to_int32_int, x)

     int res;
     BID_UINT64 x_sign;
     BID_UINT64 x_exp;
     int exp;			// unbiased exponent
  // Note: C1.w[1], C1.w[0] represent x_signif_hi, x_signif_lo (all are BID_UINT64)
     BID_UINT64 tmp64, tmp64A;
     BID_UI64DOUBLE tmp1;
     unsigned int x_nr_bits;
     int q, ind, shift;
     BID_UINT128 C1, C;
     BID_UINT128 Cstar;		// C* represents up to 34 decimal digits ~ 113 bits
     BID_UINT256 fstar;
     BID_UINT256 P256;
     int is_inexact_gt_midpoint = 0;
     int is_midpoint_lt_even = 0;

  // unpack x
x_sign = x.w[1] & MASK_SIGN;	// 0 for positive, MASK_SIGN for negative
x_exp = x.w[1] & MASK_EXP;	// biased and shifted left 49 bit positions
C1.w[1] = x.w[1] & MASK_COEFF;
C1.w[0] = x.w[0];

  // check for NaN or Infinity
if ((x.w[1] & MASK_SPECIAL) == MASK_SPECIAL) {
    // x is special
if ((x.w[1] & MASK_NAN) == MASK_NAN) {	// x is NAN
  if ((x.w[1] & MASK_SNAN) == MASK_SNAN) {	// x is SNAN
    // set invalid flag
    *pfpsf |= BID_INVALID_EXCEPTION;
    // return Integer Indefinite
    res = 0x80000000;
  } else {	// x is QNaN
    // set invalid flag
    *pfpsf |= BID_INVALID_EXCEPTION;
    // return Integer Indefinite
    res = 0x80000000;
  }
  BID_RETURN_VAL (res);
} else {	// x is not a NaN, so it must be infinity
  if (!x_sign) {	// x is +inf
    // set invalid flag
    *pfpsf |= BID_INVALID_EXCEPTION;
    // return Integer Indefinite
    res = 0x80000000;
  } else {	// x is -inf
    // set invalid flag
    *pfpsf |= BID_INVALID_EXCEPTION;
    // return Integer Indefinite
    res = 0x80000000;
  }
  BID_RETURN_VAL (res);
}
}
  // check for non-canonical values (after the check for special values)
if ((C1.w[1] > 0x0001ed09bead87c0ull)
    || (C1.w[1] == 0x0001ed09bead87c0ull
	&& (C1.w[0] > 0x378d8e63ffffffffull))
    || ((x.w[1] & 0x6000000000000000ull) == 0x6000000000000000ull)) {
  res = 0x00000000;
  BID_RETURN_VAL (res);
} else if ((C1.w[1] == 0x0ull) && (C1.w[0] == 0x0ull)) {
  // x is 0
  res = 0x00000000;
  BID_RETURN_VAL (res);
} else {	// x is not special and is not zero

  // q = nr. of decimal digits in x
  //  determine first the nr. of bits in x
  if (C1.w[1] == 0) {
    if (C1.w[0] >= 0x0020000000000000ull) {	// x >= 2^53
      // split the 64-bit value in two 32-bit halves to avoid rounding errors
	tmp1.d = (double) (C1.w[0] >> 32);	// exact conversion
	x_nr_bits =
	  33 + ((((unsigned int) (tmp1.ui64 >> 52)) & 0x7ff) - 0x3ff);
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
	|| (C1.w[1] == bid_nr_digits[x_nr_bits - 1].threshold_hi
	    && C1.w[0] >= bid_nr_digits[x_nr_bits - 1].threshold_lo))
      q++;
  }
  exp = (x_exp >> 49) - 6176;
  if ((q + exp) > 10) {	// x >= 10^10 ~= 2^33.2... (cannot fit in 32 bits)
    // set invalid flag
    *pfpsf |= BID_INVALID_EXCEPTION;
    // return Integer Indefinite
    res = 0x80000000;
    BID_RETURN_VAL (res);
  } else if ((q + exp) == 10) {	// x = c(0)c(1)...c(9).c(10)...c(q-1)
    // in this case 2^29.89... ~= 10^9 <= x < 10^10 ~= 2^33.2...
    // so x rounded to an integer may or may not fit in a signed 32-bit int
    // the cases that do not fit are identified here; the ones that fit
    // fall through and will be handled with other cases further,
    // under '1 <= q + exp <= 10'
    if (x_sign) {	// if n < 0 and q + exp = 10
      // if n <= -2^31 - 1 then n is too large
      // too large if c(0)c(1)...c(9).c(10)...c(q-1) >= 2^31+1
      // <=> 0.c(0)c(1)...c(q-1) * 10^11 >= 0x50000000a, 1<=q<=34
      if (q <= 11) {
	tmp64 = C1.w[0] * bid_ten2k64[11 - q];	// C scaled up to 11-digit int
	// c(0)c(1)...c(9)c(10) or c(0)c(1)...c(q-1)0...0 (11 digits)
	if (tmp64 >= 0x50000000aull) {
	  // set invalid flag
	  *pfpsf |= BID_INVALID_EXCEPTION;
	  // return Integer Indefinite
	  res = 0x80000000;
	  BID_RETURN_VAL (res);
	}
	// else cases that can be rounded to a 32-bit int fall through
	// to '1 <= q + exp <= 10'
      } else {	// if (q > 11), i.e. 12 <= q <= 34 and so -24 <= exp <= -2
	// 0.c(0)c(1)...c(q-1) * 10^11 >= 0x50000000a <=>
	// C >= 0x50000000a * 10^(q-11) where 1 <= q - 11 <= 23
	// (scale 2^31+1 up)
	tmp64 = 0x50000000aull;
	if (q - 11 <= 19) {	// 1 <= q - 11 <= 19; 10^(q-11) requires 64 bits
	  __mul_64x64_to_128MACH (C, tmp64, bid_ten2k64[q - 11]);
	} else {	// 20 <= q - 11 <= 23, and 10^(q-11) requires 128 bits
	  __mul_128x64_to_128 (C, tmp64, bid_ten2k128[q - 31]);
	}
	if (C1.w[1] > C.w[1]
	    || (C1.w[1] == C.w[1] && C1.w[0] >= C.w[0])) {
	  // set invalid flag
	  *pfpsf |= BID_INVALID_EXCEPTION;
	  // return Integer Indefinite
	  res = 0x80000000;
	  BID_RETURN_VAL (res);
	}
	// else cases that can be rounded to a 32-bit int fall through
	// to '1 <= q + exp <= 10'
      }
    } else {	// if n > 0 and q + exp = 10
      // if n >= 2^31 then n is too large
      // too large if c(0)c(1)...c(9).c(10)...c(q-1) >= 2^31
      // too large if 0.c(0)c(1)...c(q-1) * 10^11 >= 0x500000000, 1<=q<=34
      if (q <= 11) {
	tmp64 = C1.w[0] * bid_ten2k64[11 - q];	// C scaled up to 11-digit int
	// c(0)c(1)...c(9)c(10) or c(0)c(1)...c(q-1)0...0 (11 digits)
	if (tmp64 >= 0x500000000ull) {
	  // set invalid flag
	  *pfpsf |= BID_INVALID_EXCEPTION;
	  // return Integer Indefinite
	  res = 0x80000000;
	  BID_RETURN_VAL (res);
	}
	// else cases that can be rounded to a 32-bit int fall through
	// to '1 <= q + exp <= 10'
      } else {	// if (q > 11), i.e. 12 <= q <= 34 and so -24 <= exp <= -2
	// 0.c(0)c(1)...c(q-1) * 10^11 >= 0x500000000 <=>
	// C >= 0x500000000 * 10^(q-11) where 1 <= q - 11 <= 23
	// (scale 2^31-1/2 up)
	tmp64 = 0x500000000ull;
	if (q - 11 <= 19) {	// 1 <= q - 11 <= 19; 10^(q-11) requires 64 bits
	  __mul_64x64_to_128MACH (C, tmp64, bid_ten2k64[q - 11]);
	} else {	// 20 <= q - 11 <= 23, and 10^(q-11) requires 128 bits
	  __mul_128x64_to_128 (C, tmp64, bid_ten2k128[q - 31]);
	}
	if (C1.w[1] > C.w[1]
	    || (C1.w[1] == C.w[1] && C1.w[0] >= C.w[0])) {
	  // set invalid flag
	  *pfpsf |= BID_INVALID_EXCEPTION;
	  // return Integer Indefinite
	  res = 0x80000000;
	  BID_RETURN_VAL (res);
	}
	// else cases that can be rounded to a 32-bit int fall through
	// to '1 <= q + exp <= 10'
      }
    }
  }
  // n is not too large to be converted to int32: -2^31 - 1 < n < 2^31
  // Note: some of the cases tested for above fall through to this point
  if ((q + exp) <= 0) {
    // n = +/-0.0...c(0)c(1)...c(q-1) or n = +/-0.c(0)c(1)...c(q-1)
    // return 0
    res = 0x00000000;
    BID_RETURN_VAL (res);
  } else {	// if (1 <= q + exp <= 10, 1 <= q <= 34, -33 <= exp <= 9)
    // -2^31-1 < x <= -1 or 1 <= x < 2^31 so x can be rounded
    // toward zero to a 32-bit signed integer
    if (exp < 0) {	// 2 <= q <= 34, -33 <= exp <= -1, 1 <= q + exp <= 10
      ind = -exp;	// 1 <= ind <= 33; ind is a synonym for 'x'
      // chop off ind digits from the lower part of C1
      // C1 = C1 + 1/2 * 10^ind where the result C1 fits in 127 bits
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
      // 1 <= x <= 33
      // kx = 10^(-x) = bid_ten2mk128[ind - 1]
      // C* = (C1 + 1/2 * 10^x) * 10^(-x)
      // the approximation of 10^(-x) was rounded up to 118 bits
      __mul_128x128_to_256 (P256, C1, bid_ten2mk128[ind - 1]);
      if (ind - 1 <= 21) {	// 0 <= ind - 1 <= 21
	Cstar.w[1] = P256.w[3];
	Cstar.w[0] = P256.w[2];
	fstar.w[3] = 0;
	fstar.w[2] = P256.w[2] & bid_maskhigh128[ind - 1];
	fstar.w[1] = P256.w[1];
	fstar.w[0] = P256.w[0];
      } else {	// 22 <= ind - 1 <= 33
	Cstar.w[1] = 0;
	Cstar.w[0] = P256.w[3];
	fstar.w[3] = P256.w[3] & bid_maskhigh128[ind - 1];
	fstar.w[2] = P256.w[2];
	fstar.w[1] = P256.w[1];
	fstar.w[0] = P256.w[0];
      }
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
      shift = bid_shiftright128[ind - 1];	// 0 <= shift <= 102
      if (ind - 1 <= 21) {	// 0 <= ind - 1 <= 21
	Cstar.w[0] =
	  (Cstar.w[0] >> shift) | (Cstar.w[1] << (64 - shift));
	// redundant, it will be 0! Cstar.w[1] = (Cstar.w[1] >> shift);
      } else {	// 22 <= ind - 1 <= 33
	Cstar.w[0] = (Cstar.w[0] >> (shift - 64));	// 2 <= shift - 64 <= 38
      }
      // determine inexactness of the rounding of C*
      // if (0 < f* - 1/2 < 10^(-x)) then
      //   the result is exact
      // else // if (f* - 1/2 > T*) then
      //   the result is inexact
      if (ind - 1 <= 2) {
	if (fstar.w[1] > 0x8000000000000000ull || (fstar.w[1] == 0x8000000000000000ull && fstar.w[0] > 0x0ull)) {	// f* > 1/2 and the result may be exact
	  tmp64 = fstar.w[1] - 0x8000000000000000ull;	// f* - 1/2
	  if ((tmp64 > bid_ten2mk128trunc[ind - 1].w[1]
	       || (tmp64 == bid_ten2mk128trunc[ind - 1].w[1]
		   && fstar.w[0] >= bid_ten2mk128trunc[ind - 1].w[0]))) {
	  }	// else the result is exact
	} else {	// the result is inexact; f2* <= 1/2
	  is_inexact_gt_midpoint = 1;
	}
      } else if (ind - 1 <= 21) {	// if 3 <= ind <= 21
	if (fstar.w[3] > 0x0 ||
	    (fstar.w[3] == 0x0 && fstar.w[2] > bid_onehalf128[ind - 1]) ||
	    (fstar.w[3] == 0x0 && fstar.w[2] == bid_onehalf128[ind - 1] &&
	     (fstar.w[1] || fstar.w[0]))) {
	  // f2* > 1/2 and the result may be exact
	  // Calculate f2* - 1/2
	  tmp64 = fstar.w[2] - bid_onehalf128[ind - 1];
	  tmp64A = fstar.w[3];
	  if (tmp64 > fstar.w[2])
	    tmp64A--;
	  if (tmp64A || tmp64
	      || fstar.w[1] > bid_ten2mk128trunc[ind - 1].w[1]
	      || (fstar.w[1] == bid_ten2mk128trunc[ind - 1].w[1]
		  && fstar.w[0] > bid_ten2mk128trunc[ind - 1].w[0])) {
	  }	// else the result is exact
	} else {	// the result is inexact; f2* <= 1/2
	  is_inexact_gt_midpoint = 1;
	}
      } else {	// if 22 <= ind <= 33
	if (fstar.w[3] > bid_onehalf128[ind - 1] ||
	    (fstar.w[3] == bid_onehalf128[ind - 1] &&
	     (fstar.w[2] || fstar.w[1] || fstar.w[0]))) {
	  // f2* > 1/2 and the result may be exact
	  // Calculate f2* - 1/2
	  tmp64 = fstar.w[3] - bid_onehalf128[ind - 1];
	  if (tmp64 || fstar.w[2]
	      || fstar.w[1] > bid_ten2mk128trunc[ind - 1].w[1]
	      || (fstar.w[1] == bid_ten2mk128trunc[ind - 1].w[1]
		  && fstar.w[0] > bid_ten2mk128trunc[ind - 1].w[0])) {
	  }	// else the result is exact
	} else {	// the result is inexact; f2* <= 1/2
	  is_inexact_gt_midpoint = 1;
	}
      }

      // if the result was a midpoint it was rounded away from zero, so
      // it will need a correction
      // check for midpoints
      if ((fstar.w[3] == 0) && (fstar.w[2] == 0) &&
	  (fstar.w[1] || fstar.w[0]) &&
	  (fstar.w[1] < bid_ten2mk128trunc[ind - 1].w[1] ||
	   (fstar.w[1] == bid_ten2mk128trunc[ind - 1].w[1] &&
	    fstar.w[0] <= bid_ten2mk128trunc[ind - 1].w[0]))) {
	// the result is a midpoint; round to nearest
	if (Cstar.w[0] & 0x01) {	// Cstar.w[0] is odd; MP in [EVEN, ODD]
	  // if floor(C*) is odd C = floor(C*) - 1; the result >= 1
	  Cstar.w[0]--;	// Cstar.w[0] is now even
	  is_inexact_gt_midpoint = 0;
	} else {	// else MP in [ODD, EVEN]
	  is_midpoint_lt_even = 1;
	  is_inexact_gt_midpoint = 0;
	}
      }
      // general correction for RZ
      if (is_midpoint_lt_even || is_inexact_gt_midpoint) {
	Cstar.w[0] = Cstar.w[0] - 1;
      } else {
	;	// exact, the result is already correct
      }
      if (x_sign)
	res = -Cstar.w[0];
      else
	res = Cstar.w[0];
    } else if (exp == 0) {
      // 1 <= q <= 10
      // res = +/-C (exact)
      if (x_sign)
	res = -C1.w[0];
      else
	res = C1.w[0];
    } else {	// if (exp > 0) => 1 <= exp <= 9, 1 <= q < 9, 2 <= q + exp <= 10
      // res = +/-C * 10^exp (exact)
      if (x_sign)
	res = -C1.w[0] * bid_ten2k64[exp];
      else
	res = C1.w[0] * bid_ten2k64[exp];
    }
  }
}

BID_RETURN_VAL (res);
}

/*****************************************************************************
 *  BID128_to_int32_xint
 ****************************************************************************/

BID128_FUNCTION_ARG1_NORND_CUSTOMRESTYPE (int, bid128_to_int32_xint, x)

     int res;
     BID_UINT64 x_sign;
     BID_UINT64 x_exp;
     int exp;			// unbiased exponent
  // Note: C1.w[1], C1.w[0] represent x_signif_hi, x_signif_lo (all are BID_UINT64)
     BID_UINT64 tmp64, tmp64A;
     BID_UI64DOUBLE tmp1;
     unsigned int x_nr_bits;
     int q, ind, shift;
     BID_UINT128 C1, C;
     BID_UINT128 Cstar;		// C* represents up to 34 decimal digits ~ 113 bits
     BID_UINT256 fstar;
     BID_UINT256 P256;
     int is_inexact_gt_midpoint = 0;
     int is_midpoint_lt_even = 0;

  // unpack x
x_sign = x.w[1] & MASK_SIGN;	// 0 for positive, MASK_SIGN for negative
x_exp = x.w[1] & MASK_EXP;	// biased and shifted left 49 bit positions
C1.w[1] = x.w[1] & MASK_COEFF;
C1.w[0] = x.w[0];

  // check for NaN or Infinity
if ((x.w[1] & MASK_SPECIAL) == MASK_SPECIAL) {
    // x is special
if ((x.w[1] & MASK_NAN) == MASK_NAN) {	// x is NAN
  if ((x.w[1] & MASK_SNAN) == MASK_SNAN) {	// x is SNAN
    // set invalid flag
    *pfpsf |= BID_INVALID_EXCEPTION;
    // return Integer Indefinite
    res = 0x80000000;
  } else {	// x is QNaN
    // set invalid flag
    *pfpsf |= BID_INVALID_EXCEPTION;
    // return Integer Indefinite
    res = 0x80000000;
  }
  BID_RETURN_VAL (res);
} else {	// x is not a NaN, so it must be infinity
  if (!x_sign) {	// x is +inf
    // set invalid flag
    *pfpsf |= BID_INVALID_EXCEPTION;
    // return Integer Indefinite
    res = 0x80000000;
  } else {	// x is -inf
    // set invalid flag
    *pfpsf |= BID_INVALID_EXCEPTION;
    // return Integer Indefinite
    res = 0x80000000;
  }
  BID_RETURN_VAL (res);
}
}
  // check for non-canonical values (after the check for special values)
if ((C1.w[1] > 0x0001ed09bead87c0ull)
    || (C1.w[1] == 0x0001ed09bead87c0ull
	&& (C1.w[0] > 0x378d8e63ffffffffull))
    || ((x.w[1] & 0x6000000000000000ull) == 0x6000000000000000ull)) {
  res = 0x00000000;
  BID_RETURN_VAL (res);
} else if ((C1.w[1] == 0x0ull) && (C1.w[0] == 0x0ull)) {
  // x is 0
  res = 0x00000000;
  BID_RETURN_VAL (res);
} else {	// x is not special and is not zero

  // q = nr. of decimal digits in x
  //  determine first the nr. of bits in x
  if (C1.w[1] == 0) {
    if (C1.w[0] >= 0x0020000000000000ull) {	// x >= 2^53
      // split the 64-bit value in two 32-bit halves to avoid rounding errors
	tmp1.d = (double) (C1.w[0] >> 32);	// exact conversion
	x_nr_bits =
	  33 + ((((unsigned int) (tmp1.ui64 >> 52)) & 0x7ff) - 0x3ff);
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
	|| (C1.w[1] == bid_nr_digits[x_nr_bits - 1].threshold_hi
	    && C1.w[0] >= bid_nr_digits[x_nr_bits - 1].threshold_lo))
      q++;
  }
  exp = (x_exp >> 49) - 6176;
  if ((q + exp) > 10) {	// x >= 10^10 ~= 2^33.2... (cannot fit in 32 bits)
    // set invalid flag
    *pfpsf |= BID_INVALID_EXCEPTION;
    // return Integer Indefinite
    res = 0x80000000;
    BID_RETURN_VAL (res);
  } else if ((q + exp) == 10) {	// x = c(0)c(1)...c(9).c(10)...c(q-1)
    // in this case 2^29.89... ~= 10^9 <= x < 10^10 ~= 2^33.2...
    // so x rounded to an integer may or may not fit in a signed 32-bit int
    // the cases that do not fit are identified here; the ones that fit
    // fall through and will be handled with other cases further,
    // under '1 <= q + exp <= 10'
    if (x_sign) {	// if n < 0 and q + exp = 10
      // if n <= -2^31 - 1 then n is too large
      // too large if c(0)c(1)...c(9).c(10)...c(q-1) >= 2^31+1
      // <=> 0.c(0)c(1)...c(q-1) * 10^11 >= 0x50000000a, 1<=q<=34
      if (q <= 11) {
	tmp64 = C1.w[0] * bid_ten2k64[11 - q];	// C scaled up to 11-digit int
	// c(0)c(1)...c(9)c(10) or c(0)c(1)...c(q-1)0...0 (11 digits)
	if (tmp64 >= 0x50000000aull) {
	  // set invalid flag
	  *pfpsf |= BID_INVALID_EXCEPTION;
	  // return Integer Indefinite
	  res = 0x80000000;
	  BID_RETURN_VAL (res);
	}
	// else cases that can be rounded to a 32-bit int fall through
	// to '1 <= q + exp <= 10'
      } else {	// if (q > 11), i.e. 12 <= q <= 34 and so -24 <= exp <= -2
	// 0.c(0)c(1)...c(q-1) * 10^11 >= 0x50000000a <=>
	// C >= 0x50000000a * 10^(q-11) where 1 <= q - 11 <= 23
	// (scale 2^31+1 up)
	tmp64 = 0x50000000aull;
	if (q - 11 <= 19) {	// 1 <= q - 11 <= 19; 10^(q-11) requires 64 bits
	  __mul_64x64_to_128MACH (C, tmp64, bid_ten2k64[q - 11]);
	} else {	// 20 <= q - 11 <= 23, and 10^(q-11) requires 128 bits
	  __mul_128x64_to_128 (C, tmp64, bid_ten2k128[q - 31]);
	}
	if (C1.w[1] > C.w[1]
	    || (C1.w[1] == C.w[1] && C1.w[0] >= C.w[0])) {
	  // set invalid flag
	  *pfpsf |= BID_INVALID_EXCEPTION;
	  // return Integer Indefinite
	  res = 0x80000000;
	  BID_RETURN_VAL (res);
	}
	// else cases that can be rounded to a 32-bit int fall through
	// to '1 <= q + exp <= 10'
      }
    } else {	// if n > 0 and q + exp = 10
      // if n >= 2^31 then n is too large
      // too large if c(0)c(1)...c(9).c(10)...c(q-1) >= 2^31
      // too large if 0.c(0)c(1)...c(q-1) * 10^11 >= 0x500000000, 1<=q<=34
      if (q <= 11) {
	tmp64 = C1.w[0] * bid_ten2k64[11 - q];	// C scaled up to 11-digit int
	// c(0)c(1)...c(9)c(10) or c(0)c(1)...c(q-1)0...0 (11 digits)
	if (tmp64 >= 0x500000000ull) {
	  // set invalid flag
	  *pfpsf |= BID_INVALID_EXCEPTION;
	  // return Integer Indefinite
	  res = 0x80000000;
	  BID_RETURN_VAL (res);
	}
	// else cases that can be rounded to a 32-bit int fall through
	// to '1 <= q + exp <= 10'
      } else {	// if (q > 11), i.e. 12 <= q <= 34 and so -24 <= exp <= -2
	// 0.c(0)c(1)...c(q-1) * 10^11 >= 0x500000000 <=>
	// C >= 0x500000000 * 10^(q-11) where 1 <= q - 11 <= 23
	// (scale 2^31-1/2 up)
	tmp64 = 0x500000000ull;
	if (q - 11 <= 19) {	// 1 <= q - 11 <= 19; 10^(q-11) requires 64 bits
	  __mul_64x64_to_128MACH (C, tmp64, bid_ten2k64[q - 11]);
	} else {	// 20 <= q - 11 <= 23, and 10^(q-11) requires 128 bits
	  __mul_128x64_to_128 (C, tmp64, bid_ten2k128[q - 31]);
	}
	if (C1.w[1] > C.w[1]
	    || (C1.w[1] == C.w[1] && C1.w[0] >= C.w[0])) {
	  // set invalid flag
	  *pfpsf |= BID_INVALID_EXCEPTION;
	  // return Integer Indefinite
	  res = 0x80000000;
	  BID_RETURN_VAL (res);
	}
	// else cases that can be rounded to a 32-bit int fall through
	// to '1 <= q + exp <= 10'
      }
    }
  }
  // n is not too large to be converted to int32: -2^31 - 1 < n < 2^31
  // Note: some of the cases tested for above fall through to this point
  if ((q + exp) <= 0) {
    // n = +/-0.0...c(0)c(1)...c(q-1) or n = +/-0.c(0)c(1)...c(q-1)
    // set inexact flag
    *pfpsf |= BID_INEXACT_EXCEPTION;
    // return 0
    res = 0x00000000;
    BID_RETURN_VAL (res);
  } else {	// if (1 <= q + exp <= 10, 1 <= q <= 34, -33 <= exp <= 9)
    // -2^31-1 < x <= -1 or 1 <= x < 2^31 so x can be rounded
    // toward zero to a 32-bit signed integer
    if (exp < 0) {	// 2 <= q <= 34, -33 <= exp <= -1, 1 <= q + exp <= 10
      ind = -exp;	// 1 <= ind <= 33; ind is a synonym for 'x'
      // chop off ind digits from the lower part of C1
      // C1 = C1 + 1/2 * 10^ind where the result C1 fits in 127 bits
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
      // 1 <= x <= 33
      // kx = 10^(-x) = bid_ten2mk128[ind - 1]
      // C* = (C1 + 1/2 * 10^x) * 10^(-x)
      // the approximation of 10^(-x) was rounded up to 118 bits
      __mul_128x128_to_256 (P256, C1, bid_ten2mk128[ind - 1]);
      if (ind - 1 <= 21) {	// 0 <= ind - 1 <= 21
	Cstar.w[1] = P256.w[3];
	Cstar.w[0] = P256.w[2];
	fstar.w[3] = 0;
	fstar.w[2] = P256.w[2] & bid_maskhigh128[ind - 1];
	fstar.w[1] = P256.w[1];
	fstar.w[0] = P256.w[0];
      } else {	// 22 <= ind - 1 <= 33
	Cstar.w[1] = 0;
	Cstar.w[0] = P256.w[3];
	fstar.w[3] = P256.w[3] & bid_maskhigh128[ind - 1];
	fstar.w[2] = P256.w[2];
	fstar.w[1] = P256.w[1];
	fstar.w[0] = P256.w[0];
      }
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
      shift = bid_shiftright128[ind - 1];	// 0 <= shift <= 102
      if (ind - 1 <= 21) {	// 0 <= ind - 1 <= 21
	Cstar.w[0] =
	  (Cstar.w[0] >> shift) | (Cstar.w[1] << (64 - shift));
	// redundant, it will be 0! Cstar.w[1] = (Cstar.w[1] >> shift);
      } else {	// 22 <= ind - 1 <= 33
	Cstar.w[0] = (Cstar.w[0] >> (shift - 64));	// 2 <= shift - 64 <= 38
      }
      // determine inexactness of the rounding of C*
      // if (0 < f* - 1/2 < 10^(-x)) then
      //   the result is exact
      // else // if (f* - 1/2 > T*) then
      //   the result is inexact
      if (ind - 1 <= 2) {
	if (fstar.w[1] > 0x8000000000000000ull || (fstar.w[1] == 0x8000000000000000ull && fstar.w[0] > 0x0ull)) {	// f* > 1/2 and the result may be exact
	  tmp64 = fstar.w[1] - 0x8000000000000000ull;	// f* - 1/2
	  if (tmp64 > bid_ten2mk128trunc[ind - 1].w[1]
	      || (tmp64 == bid_ten2mk128trunc[ind - 1].w[1]
		  && fstar.w[0] >= bid_ten2mk128trunc[ind - 1].w[0])) {
	    // set the inexact flag
	    *pfpsf |= BID_INEXACT_EXCEPTION;
	  }	// else the result is exact
	} else {	// the result is inexact; f2* <= 1/2
	  // set the inexact flag
	  *pfpsf |= BID_INEXACT_EXCEPTION;
	  is_inexact_gt_midpoint = 1;
	}
      } else if (ind - 1 <= 21) {	// if 3 <= ind <= 21
	if (fstar.w[3] > 0x0 ||
	    (fstar.w[3] == 0x0 && fstar.w[2] > bid_onehalf128[ind - 1]) ||
	    (fstar.w[3] == 0x0 && fstar.w[2] == bid_onehalf128[ind - 1] &&
	     (fstar.w[1] || fstar.w[0]))) {
	  // f2* > 1/2 and the result may be exact
	  // Calculate f2* - 1/2
	  tmp64 = fstar.w[2] - bid_onehalf128[ind - 1];
	  tmp64A = fstar.w[3];
	  if (tmp64 > fstar.w[2])
	    tmp64A--;
	  if (tmp64A || tmp64
	      || fstar.w[1] > bid_ten2mk128trunc[ind - 1].w[1]
	      || (fstar.w[1] == bid_ten2mk128trunc[ind - 1].w[1]
		  && fstar.w[0] > bid_ten2mk128trunc[ind - 1].w[0])) {
	    // set the inexact flag
	    *pfpsf |= BID_INEXACT_EXCEPTION;
	  }	// else the result is exact
	} else {	// the result is inexact; f2* <= 1/2
	  // set the inexact flag
	  *pfpsf |= BID_INEXACT_EXCEPTION;
	  is_inexact_gt_midpoint = 1;
	}
      } else {	// if 22 <= ind <= 33
	if (fstar.w[3] > bid_onehalf128[ind - 1] ||
	    (fstar.w[3] == bid_onehalf128[ind - 1] && (fstar.w[2] ||
						   fstar.w[1]
						   || fstar.w[0]))) {
	  // f2* > 1/2 and the result may be exact
	  // Calculate f2* - 1/2
	  tmp64 = fstar.w[3] - bid_onehalf128[ind - 1];
	  if (tmp64 || fstar.w[2]
	      || fstar.w[1] > bid_ten2mk128trunc[ind - 1].w[1]
	      || (fstar.w[1] == bid_ten2mk128trunc[ind - 1].w[1]
		  && fstar.w[0] > bid_ten2mk128trunc[ind - 1].w[0])) {
	    // set the inexact flag
	    *pfpsf |= BID_INEXACT_EXCEPTION;
	  }	// else the result is exact
	} else {	// the result is inexact; f2* <= 1/2
	  // set the inexact flag
	  *pfpsf |= BID_INEXACT_EXCEPTION;
	  is_inexact_gt_midpoint = 1;
	}
      }

      // if the result was a midpoint it was rounded away from zero, so
      // it will need a correction
      // check for midpoints
      if ((fstar.w[3] == 0) && (fstar.w[2] == 0)
	  && (fstar.w[1] || fstar.w[0])
	  && (fstar.w[1] < bid_ten2mk128trunc[ind - 1].w[1]
	      || (fstar.w[1] == bid_ten2mk128trunc[ind - 1].w[1]
		  && fstar.w[0] <= bid_ten2mk128trunc[ind - 1].w[0]))) {
	// the result is a midpoint; round to nearest
	if (Cstar.w[0] & 0x01) {	// Cstar.w[0] is odd; MP in [EVEN, ODD]
	  // if floor(C*) is odd C = floor(C*) - 1; the result >= 1
	  Cstar.w[0]--;	// Cstar.w[0] is now even
	  is_inexact_gt_midpoint = 0;
	} else {	// else MP in [ODD, EVEN]
	  is_midpoint_lt_even = 1;
	  is_inexact_gt_midpoint = 0;
	}
      }
      // general correction for RZ
      if (is_midpoint_lt_even || is_inexact_gt_midpoint) {
	Cstar.w[0] = Cstar.w[0] - 1;
      } else {
	;	// exact, the result is already correct
      }
      if (x_sign)
	res = -Cstar.w[0];
      else
	res = Cstar.w[0];
    } else if (exp == 0) {
      // 1 <= q <= 10
      // res = +/-C (exact)
      if (x_sign)
	res = -C1.w[0];
      else
	res = C1.w[0];
    } else {	// if (exp > 0) => 1 <= exp <= 9, 1 <= q < 9, 2 <= q + exp <= 10
      // res = +/-C * 10^exp (exact)
      if (x_sign)
	res = -C1.w[0] * bid_ten2k64[exp];
      else
	res = C1.w[0] * bid_ten2k64[exp];
    }
  }
}

BID_RETURN_VAL (res);
}

/*****************************************************************************
 *  BID128_to_int32_rninta
 ****************************************************************************/

BID128_FUNCTION_ARG1_NORND_CUSTOMRESTYPE (int, bid128_to_int32_rninta,
					  x)

     int res;
     BID_UINT64 x_sign;
     BID_UINT64 x_exp;
     int exp;			// unbiased exponent
  // Note: C1.w[1], C1.w[0] represent x_signif_hi, x_signif_lo (all are BID_UINT64)
     BID_UINT64 tmp64;
     BID_UI64DOUBLE tmp1;
     unsigned int x_nr_bits;
     int q, ind, shift;
     BID_UINT128 C1, C;
     BID_UINT128 Cstar;		// C* represents up to 34 decimal digits ~ 113 bits
     BID_UINT256 P256;

  // unpack x
x_sign = x.w[1] & MASK_SIGN;	// 0 for positive, MASK_SIGN for negative
x_exp = x.w[1] & MASK_EXP;	// biased and shifted left 49 bit positions
C1.w[1] = x.w[1] & MASK_COEFF;
C1.w[0] = x.w[0];

  // check for NaN or Infinity
if ((x.w[1] & MASK_SPECIAL) == MASK_SPECIAL) {
    // x is special
if ((x.w[1] & MASK_NAN) == MASK_NAN) {	// x is NAN
  if ((x.w[1] & MASK_SNAN) == MASK_SNAN) {	// x is SNAN
    // set invalid flag
    *pfpsf |= BID_INVALID_EXCEPTION;
    // return Integer Indefinite
    res = 0x80000000;
  } else {	// x is QNaN
    // set invalid flag
    *pfpsf |= BID_INVALID_EXCEPTION;
    // return Integer Indefinite
    res = 0x80000000;
  }
  BID_RETURN_VAL (res);
} else {	// x is not a NaN, so it must be infinity
  if (!x_sign) {	// x is +inf
    // set invalid flag
    *pfpsf |= BID_INVALID_EXCEPTION;
    // return Integer Indefinite
    res = 0x80000000;
  } else {	// x is -inf
    // set invalid flag
    *pfpsf |= BID_INVALID_EXCEPTION;
    // return Integer Indefinite
    res = 0x80000000;
  }
  BID_RETURN_VAL (res);
}
}
  // check for non-canonical values (after the check for special values)
if ((C1.w[1] > 0x0001ed09bead87c0ull)
    || (C1.w[1] == 0x0001ed09bead87c0ull
	&& (C1.w[0] > 0x378d8e63ffffffffull))
    || ((x.w[1] & 0x6000000000000000ull) == 0x6000000000000000ull)) {
  res = 0x00000000;
  BID_RETURN_VAL (res);
} else if ((C1.w[1] == 0x0ull) && (C1.w[0] == 0x0ull)) {
  // x is 0
  res = 0x00000000;
  BID_RETURN_VAL (res);
} else {	// x is not special and is not zero

  // q = nr. of decimal digits in x
  //  determine first the nr. of bits in x
  if (C1.w[1] == 0) {
    if (C1.w[0] >= 0x0020000000000000ull) {	// x >= 2^53
      // split the 64-bit value in two 32-bit halves to avoid rounding errors
	tmp1.d = (double) (C1.w[0] >> 32);	// exact conversion
	x_nr_bits =
	  33 + ((((unsigned int) (tmp1.ui64 >> 52)) & 0x7ff) - 0x3ff);
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
	|| (C1.w[1] == bid_nr_digits[x_nr_bits - 1].threshold_hi
	    && C1.w[0] >= bid_nr_digits[x_nr_bits - 1].threshold_lo))
      q++;
  }
  exp = (x_exp >> 49) - 6176;
  if ((q + exp) > 10) {	// x >= 10^10 ~= 2^33.2... (cannot fit in 32 bits)
    // set invalid flag
    *pfpsf |= BID_INVALID_EXCEPTION;
    // return Integer Indefinite
    res = 0x80000000;
    BID_RETURN_VAL (res);
  } else if ((q + exp) == 10) {	// x = c(0)c(1)...c(9).c(10)...c(q-1)
    // in this case 2^29.89... ~= 10^9 <= x < 10^10 ~= 2^33.2...
    // so x rounded to an integer may or may not fit in a signed 32-bit int
    // the cases that do not fit are identified here; the ones that fit
    // fall through and will be handled with other cases further,
    // under '1 <= q + exp <= 10'
    if (x_sign) {	// if n < 0 and q + exp = 10
      // if n <= -2^31 - 1/2 then n is too large
      // too large if c(0)c(1)...c(9).c(10)...c(q-1) >= 2^31+1/2
      // <=> 0.c(0)c(1)...c(q-1) * 10^11 >= 0x500000005, 1<=q<=34
      if (q <= 11) {
	tmp64 = C1.w[0] * bid_ten2k64[11 - q];	// C scaled up to 11-digit int
	// c(0)c(1)...c(9)c(10) or c(0)c(1)...c(q-1)0...0 (11 digits)
	if (tmp64 >= 0x500000005ull) {
	  // set invalid flag
	  *pfpsf |= BID_INVALID_EXCEPTION;
	  // return Integer Indefinite
	  res = 0x80000000;
	  BID_RETURN_VAL (res);
	}
	// else cases that can be rounded to a 32-bit int fall through
	// to '1 <= q + exp <= 10'
      } else {	// if (q > 11), i.e. 12 <= q <= 34 and so -24 <= exp <= -2
	// 0.c(0)c(1)...c(q-1) * 10^11 >= 0x500000005 <=>
	// C >= 0x500000005 * 10^(q-11) where 1 <= q - 11 <= 23
	// (scale 2^31+1/2 up)
	tmp64 = 0x500000005ull;
	if (q - 11 <= 19) {	// 1 <= q - 11 <= 19; 10^(q-11) requires 64 bits
	  __mul_64x64_to_128MACH (C, tmp64, bid_ten2k64[q - 11]);
	} else {	// 20 <= q - 11 <= 23, and 10^(q-11) requires 128 bits
	  __mul_128x64_to_128 (C, tmp64, bid_ten2k128[q - 31]);
	}
	if (C1.w[1] > C.w[1]
	    || (C1.w[1] == C.w[1] && C1.w[0] >= C.w[0])) {
	  // set invalid flag
	  *pfpsf |= BID_INVALID_EXCEPTION;
	  // return Integer Indefinite
	  res = 0x80000000;
	  BID_RETURN_VAL (res);
	}
	// else cases that can be rounded to a 32-bit int fall through
	// to '1 <= q + exp <= 10'
      }
    } else {	// if n > 0 and q + exp = 10
      // if n >= 2^31 - 1/2 then n is too large
      // too large if c(0)c(1)...c(9).c(10)...c(q-1) >= 2^31-1/2
      // too large if 0.c(0)c(1)...c(q-1) * 10^11 >= 0x4fffffffb, 1<=q<=34
      if (q <= 11) {
	tmp64 = C1.w[0] * bid_ten2k64[11 - q];	// C scaled up to 11-digit int
	// c(0)c(1)...c(9)c(10) or c(0)c(1)...c(q-1)0...0 (11 digits)
	if (tmp64 >= 0x4fffffffbull) {
	  // set invalid flag
	  *pfpsf |= BID_INVALID_EXCEPTION;
	  // return Integer Indefinite
	  res = 0x80000000;
	  BID_RETURN_VAL (res);
	}
	// else cases that can be rounded to a 32-bit int fall through
	// to '1 <= q + exp <= 10'
      } else {	// if (q > 11), i.e. 12 <= q <= 34 and so -24 <= exp <= -2
	// 0.c(0)c(1)...c(q-1) * 10^11 >= 0x4fffffffb <=>
	// C >= 0x4fffffffb * 10^(q-11) where 1 <= q - 11 <= 23
	// (scale 2^31-1/2 up)
	tmp64 = 0x4fffffffbull;
	if (q - 11 <= 19) {	// 1 <= q - 11 <= 19; 10^(q-11) requires 64 bits
	  __mul_64x64_to_128MACH (C, tmp64, bid_ten2k64[q - 11]);
	} else {	// 20 <= q - 11 <= 23, and 10^(q-11) requires 128 bits
	  __mul_128x64_to_128 (C, tmp64, bid_ten2k128[q - 31]);
	}
	if (C1.w[1] > C.w[1]
	    || (C1.w[1] == C.w[1] && C1.w[0] >= C.w[0])) {
	  // set invalid flag
	  *pfpsf |= BID_INVALID_EXCEPTION;
	  // return Integer Indefinite
	  res = 0x80000000;
	  BID_RETURN_VAL (res);
	}
	// else cases that can be rounded to a 32-bit int fall through
	// to '1 <= q + exp <= 10'
      }
    }
  }
  // n is not too large to be converted to int32: -2^31 - 1/2 < n < 2^31 - 1/2
  // Note: some of the cases tested for above fall through to this point
  if ((q + exp) < 0) {	// n = +/-0.0...c(0)c(1)...c(q-1)
    // return 0
    res = 0x00000000;
    BID_RETURN_VAL (res);
  } else if ((q + exp) == 0) {	// n = +/-0.c(0)c(1)...c(q-1)
    // if 0.c(0)c(1)...c(q-1) < 0.5 <=> c(0)c(1)...c(q-1) < 5 * 10^(q-1)
    //   res = 0
    // else
    //   res = +/-1
    ind = q - 1;
    if (ind <= 18) {	// 0 <= ind <= 18
      if ((C1.w[1] == 0) && (C1.w[0] < bid_midpoint64[ind])) {
	res = 0x00000000;	// return 0
      } else if (x_sign) {	// n < 0
	res = 0xffffffff;	// return -1
      } else {	// n > 0
	res = 0x00000001;	// return +1
      }
    } else {	// 19 <= ind <= 33
      if ((C1.w[1] < bid_midpoint128[ind - 19].w[1])
	  || ((C1.w[1] == bid_midpoint128[ind - 19].w[1])
	      && (C1.w[0] < bid_midpoint128[ind - 19].w[0]))) {
	res = 0x00000000;	// return 0
      } else if (x_sign) {	// n < 0
	res = 0xffffffff;	// return -1
      } else {	// n > 0
	res = 0x00000001;	// return +1
      }
    }
  } else {	// if (1 <= q + exp <= 10, 1 <= q <= 34, -33 <= exp <= 9)
    // -2^31-1/2 < x <= -1 or 1 <= x < 2^31-1/2 so x can be rounded
    // to nearest-away to a 32-bit signed integer
    if (exp < 0) {	// 2 <= q <= 34, -33 <= exp <= -1, 1 <= q + exp <= 10
      ind = -exp;	// 1 <= ind <= 33; ind is a synonym for 'x'
      // chop off ind digits from the lower part of C1
      // C1 = C1 + 1/2 * 10^ind where the result C1 fits in 127 bits
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
      // 1 <= x <= 33
      // kx = 10^(-x) = bid_ten2mk128[ind - 1]
      // C* = (C1 + 1/2 * 10^x) * 10^(-x)
      // the approximation of 10^(-x) was rounded up to 118 bits
      __mul_128x128_to_256 (P256, C1, bid_ten2mk128[ind - 1]);
      if (ind - 1 <= 21) {	// 0 <= ind - 1 <= 21
	Cstar.w[1] = P256.w[3];
	Cstar.w[0] = P256.w[2];
      } else {	// 22 <= ind - 1 <= 33
	Cstar.w[1] = 0;
	Cstar.w[0] = P256.w[3];
      }
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
      shift = bid_shiftright128[ind - 1];	// 0 <= shift <= 102
      if (ind - 1 <= 21) {	// 0 <= ind - 1 <= 21
	Cstar.w[0] =
	  (Cstar.w[0] >> shift) | (Cstar.w[1] << (64 - shift));
	// redundant, it will be 0! Cstar.w[1] = (Cstar.w[1] >> shift);
      } else {	// 22 <= ind - 1 <= 33
	Cstar.w[0] = (Cstar.w[0] >> (shift - 64));	// 2 <= shift - 64 <= 38
      }
      // if the result was a midpoint, it was already rounded away from zero
      if (x_sign)
	res = -Cstar.w[0];
      else
	res = Cstar.w[0];
      // no need to check for midpoints - already rounded away from zero!
    } else if (exp == 0) {
      // 1 <= q <= 10
      // res = +/-C (exact)
      if (x_sign)
	res = -C1.w[0];
      else
	res = C1.w[0];
    } else {	// if (exp > 0) => 1 <= exp <= 9, 1 <= q < 9, 2 <= q + exp <= 10
      // res = +/-C * 10^exp (exact)
      if (x_sign)
	res = -C1.w[0] * bid_ten2k64[exp];
      else
	res = C1.w[0] * bid_ten2k64[exp];
    }
  }
}

BID_RETURN_VAL (res);
}

/*****************************************************************************
 *  BID128_to_int32_xrninta
 ****************************************************************************/

BID128_FUNCTION_ARG1_NORND_CUSTOMRESTYPE (int, bid128_to_int32_xrninta,
					  x)

     int res;
     BID_UINT64 x_sign;
     BID_UINT64 x_exp;
     int exp;			// unbiased exponent
  // Note: C1.w[1], C1.w[0] represent x_signif_hi, x_signif_lo (all are BID_UINT64)
     BID_UINT64 tmp64, tmp64A;
     BID_UI64DOUBLE tmp1;
     unsigned int x_nr_bits;
     int q, ind, shift;
     BID_UINT128 C1, C;
     BID_UINT128 Cstar;		// C* represents up to 34 decimal digits ~ 113 bits
     BID_UINT256 fstar;
     BID_UINT256 P256;

  // unpack x
x_sign = x.w[1] & MASK_SIGN;	// 0 for positive, MASK_SIGN for negative
x_exp = x.w[1] & MASK_EXP;	// biased and shifted left 49 bit positions
C1.w[1] = x.w[1] & MASK_COEFF;
C1.w[0] = x.w[0];

  // check for NaN or Infinity
if ((x.w[1] & MASK_SPECIAL) == MASK_SPECIAL) {
    // x is special
if ((x.w[1] & MASK_NAN) == MASK_NAN) {	// x is NAN
  if ((x.w[1] & MASK_SNAN) == MASK_SNAN) {	// x is SNAN
    // set invalid flag
    *pfpsf |= BID_INVALID_EXCEPTION;
    // return Integer Indefinite
    res = 0x80000000;
  } else {	// x is QNaN
    // set invalid flag
    *pfpsf |= BID_INVALID_EXCEPTION;
    // return Integer Indefinite
    res = 0x80000000;
  }
  BID_RETURN_VAL (res);
} else {	// x is not a NaN, so it must be infinity
  if (!x_sign) {	// x is +inf
    // set invalid flag
    *pfpsf |= BID_INVALID_EXCEPTION;
    // return Integer Indefinite
    res = 0x80000000;
  } else {	// x is -inf
    // set invalid flag
    *pfpsf |= BID_INVALID_EXCEPTION;
    // return Integer Indefinite
    res = 0x80000000;
  }
  BID_RETURN_VAL (res);
}
}
  // check for non-canonical values (after the check for special values)
if ((C1.w[1] > 0x0001ed09bead87c0ull)
    || (C1.w[1] == 0x0001ed09bead87c0ull
	&& (C1.w[0] > 0x378d8e63ffffffffull))
    || ((x.w[1] & 0x6000000000000000ull) == 0x6000000000000000ull)) {
  res = 0x00000000;
  BID_RETURN_VAL (res);
} else if ((C1.w[1] == 0x0ull) && (C1.w[0] == 0x0ull)) {
  // x is 0
  res = 0x00000000;
  BID_RETURN_VAL (res);
} else {	// x is not special and is not zero

  // q = nr. of decimal digits in x
  //  determine first the nr. of bits in x
  if (C1.w[1] == 0) {
    if (C1.w[0] >= 0x0020000000000000ull) {	// x >= 2^53
      // split the 64-bit value in two 32-bit halves to avoid rounding errors
	tmp1.d = (double) (C1.w[0] >> 32);	// exact conversion
	x_nr_bits =
	  33 + ((((unsigned int) (tmp1.ui64 >> 52)) & 0x7ff) - 0x3ff);
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
	|| (C1.w[1] == bid_nr_digits[x_nr_bits - 1].threshold_hi
	    && C1.w[0] >= bid_nr_digits[x_nr_bits - 1].threshold_lo))
      q++;
  }
  exp = (x_exp >> 49) - 6176;
  if ((q + exp) > 10) {	// x >= 10^10 ~= 2^33.2... (cannot fit in 32 bits)
    // set invalid flag
    *pfpsf |= BID_INVALID_EXCEPTION;
    // return Integer Indefinite
    res = 0x80000000;
    BID_RETURN_VAL (res);
  } else if ((q + exp) == 10) {	// x = c(0)c(1)...c(9).c(10)...c(q-1)
    // in this case 2^29.89... ~= 10^9 <= x < 10^10 ~= 2^33.2...
    // so x rounded to an integer may or may not fit in a signed 32-bit int
    // the cases that do not fit are identified here; the ones that fit
    // fall through and will be handled with other cases further,
    // under '1 <= q + exp <= 10'
    if (x_sign) {	// if n < 0 and q + exp = 10
      // if n <= -2^31 - 1/2 then n is too large
      // too large if c(0)c(1)...c(9).c(10)...c(q-1) >= 2^31+1/2
      // <=> 0.c(0)c(1)...c(q-1) * 10^11 >= 0x500000005, 1<=q<=34
      if (q <= 11) {
	tmp64 = C1.w[0] * bid_ten2k64[11 - q];	// C scaled up to 11-digit int
	// c(0)c(1)...c(9)c(10) or c(0)c(1)...c(q-1)0...0 (11 digits)
	if (tmp64 >= 0x500000005ull) {
	  // set invalid flag
	  *pfpsf |= BID_INVALID_EXCEPTION;
	  // return Integer Indefinite
	  res = 0x80000000;
	  BID_RETURN_VAL (res);
	}
	// else cases that can be rounded to a 32-bit int fall through
	// to '1 <= q + exp <= 10'
      } else {	// if (q > 11), i.e. 12 <= q <= 34 and so -24 <= exp <= -2
	// 0.c(0)c(1)...c(q-1) * 10^11 >= 0x500000005 <=>
	// C >= 0x500000005 * 10^(q-11) where 1 <= q - 11 <= 23
	// (scale 2^31+1/2 up)
	tmp64 = 0x500000005ull;
	if (q - 11 <= 19) {	// 1 <= q - 11 <= 19; 10^(q-11) requires 64 bits
	  __mul_64x64_to_128MACH (C, tmp64, bid_ten2k64[q - 11]);
	} else {	// 20 <= q - 11 <= 23, and 10^(q-11) requires 128 bits
	  __mul_128x64_to_128 (C, tmp64, bid_ten2k128[q - 31]);
	}
	if (C1.w[1] > C.w[1]
	    || (C1.w[1] == C.w[1] && C1.w[0] >= C.w[0])) {
	  // set invalid flag
	  *pfpsf |= BID_INVALID_EXCEPTION;
	  // return Integer Indefinite
	  res = 0x80000000;
	  BID_RETURN_VAL (res);
	}
	// else cases that can be rounded to a 32-bit int fall through
	// to '1 <= q + exp <= 10'
      }
    } else {	// if n > 0 and q + exp = 10
      // if n >= 2^31 - 1/2 then n is too large
      // too large if c(0)c(1)...c(9).c(10)...c(q-1) >= 2^31-1/2
      // too large if 0.c(0)c(1)...c(q-1) * 10^11 >= 0x4fffffffb, 1<=q<=34
      if (q <= 11) {
	tmp64 = C1.w[0] * bid_ten2k64[11 - q];	// C scaled up to 11-digit int
	// c(0)c(1)...c(9)c(10) or c(0)c(1)...c(q-1)0...0 (11 digits)
	if (tmp64 >= 0x4fffffffbull) {
	  // set invalid flag
	  *pfpsf |= BID_INVALID_EXCEPTION;
	  // return Integer Indefinite
	  res = 0x80000000;
	  BID_RETURN_VAL (res);
	}
	// else cases that can be rounded to a 32-bit int fall through
	// to '1 <= q + exp <= 10'
      } else {	// if (q > 11), i.e. 12 <= q <= 34 and so -24 <= exp <= -2
	// 0.c(0)c(1)...c(q-1) * 10^11 >= 0x4fffffffb <=>
	// C >= 0x4fffffffb * 10^(q-11) where 1 <= q - 11 <= 23
	// (scale 2^31-1/2 up)
	tmp64 = 0x4fffffffbull;
	if (q - 11 <= 19) {	// 1 <= q - 11 <= 19; 10^(q-11) requires 64 bits
	  __mul_64x64_to_128MACH (C, tmp64, bid_ten2k64[q - 11]);
	} else {	// 20 <= q - 11 <= 23, and 10^(q-11) requires 128 bits
	  __mul_128x64_to_128 (C, tmp64, bid_ten2k128[q - 31]);
	}
	if (C1.w[1] > C.w[1]
	    || (C1.w[1] == C.w[1] && C1.w[0] >= C.w[0])) {
	  // set invalid flag
	  *pfpsf |= BID_INVALID_EXCEPTION;
	  // return Integer Indefinite
	  res = 0x80000000;
	  BID_RETURN_VAL (res);
	}
	// else cases that can be rounded to a 32-bit int fall through
	// to '1 <= q + exp <= 10'
      }
    }
  }
  // n is not too large to be converted to int32: -2^31 - 1/2 < n < 2^31 - 1/2
  // Note: some of the cases tested for above fall through to this point
  if ((q + exp) < 0) {	// n = +/-0.0...c(0)c(1)...c(q-1)
    // set inexact flag
    *pfpsf |= BID_INEXACT_EXCEPTION;
    // return 0
    res = 0x00000000;
    BID_RETURN_VAL (res);
  } else if ((q + exp) == 0) {	// n = +/-0.c(0)c(1)...c(q-1)
    // if 0.c(0)c(1)...c(q-1) < 0.5 <=> c(0)c(1)...c(q-1) < 5 * 10^(q-1)
    //   res = 0
    // else
    //   res = +/-1
    ind = q - 1;
    if (ind <= 18) {	// 0 <= ind <= 18
      if ((C1.w[1] == 0) && (C1.w[0] < bid_midpoint64[ind])) {
	res = 0x00000000;	// return 0
      } else if (x_sign) {	// n < 0
	res = 0xffffffff;	// return -1
      } else {	// n > 0
	res = 0x00000001;	// return +1
      }
    } else {	// 19 <= ind <= 33
      if ((C1.w[1] < bid_midpoint128[ind - 19].w[1])
	  || ((C1.w[1] == bid_midpoint128[ind - 19].w[1])
	      && (C1.w[0] < bid_midpoint128[ind - 19].w[0]))) {
	res = 0x00000000;	// return 0
      } else if (x_sign) {	// n < 0
	res = 0xffffffff;	// return -1
      } else {	// n > 0
	res = 0x00000001;	// return +1
      }
    }
    // set inexact flag
    *pfpsf |= BID_INEXACT_EXCEPTION;
  } else {	// if (1 <= q + exp <= 10, 1 <= q <= 34, -33 <= exp <= 9)
    // -2^31-1/2 < x <= -1 or 1 <= x < 2^31-1/2 so x can be rounded
    // to nearest-away to a 32-bit signed integer
    if (exp < 0) {	// 2 <= q <= 34, -33 <= exp <= -1, 1 <= q + exp <= 10
      ind = -exp;	// 1 <= ind <= 33; ind is a synonym for 'x'
      // chop off ind digits from the lower part of C1
      // C1 = C1 + 1/2 * 10^ind where the result C1 fits in 127 bits
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
      // 1 <= x <= 33
      // kx = 10^(-x) = bid_ten2mk128[ind - 1]
      // C* = (C1 + 1/2 * 10^x) * 10^(-x)
      // the approximation of 10^(-x) was rounded up to 118 bits
      __mul_128x128_to_256 (P256, C1, bid_ten2mk128[ind - 1]);
      if (ind - 1 <= 21) {	// 0 <= ind - 1 <= 21
	Cstar.w[1] = P256.w[3];
	Cstar.w[0] = P256.w[2];
	fstar.w[3] = 0;
	fstar.w[2] = P256.w[2] & bid_maskhigh128[ind - 1];
	fstar.w[1] = P256.w[1];
	fstar.w[0] = P256.w[0];
      } else {	// 22 <= ind - 1 <= 33
	Cstar.w[1] = 0;
	Cstar.w[0] = P256.w[3];
	fstar.w[3] = P256.w[3] & bid_maskhigh128[ind - 1];
	fstar.w[2] = P256.w[2];
	fstar.w[1] = P256.w[1];
	fstar.w[0] = P256.w[0];
      }
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
      shift = bid_shiftright128[ind - 1];	// 0 <= shift <= 102
      if (ind - 1 <= 21) {	// 0 <= ind - 1 <= 21
	Cstar.w[0] =
	  (Cstar.w[0] >> shift) | (Cstar.w[1] << (64 - shift));
	// redundant, it will be 0! Cstar.w[1] = (Cstar.w[1] >> shift);
      } else {	// 22 <= ind - 1 <= 33
	Cstar.w[0] = (Cstar.w[0] >> (shift - 64));	// 2 <= shift - 64 <= 38
      }
      // if the result was a midpoint, it was already rounded away from zero
      if (x_sign)
	res = -Cstar.w[0];
      else
	res = Cstar.w[0];
      // determine inexactness of the rounding of C*
      // if (0 < f* - 1/2 < 10^(-x)) then
      //   the result is exact
      // else // if (f* - 1/2 > T*) then
      //   the result is inexact
      if (ind - 1 <= 2) {
	if (fstar.w[1] > 0x8000000000000000ull || (fstar.w[1] == 0x8000000000000000ull && fstar.w[0] > 0x0ull)) {	// f* > 1/2 and the result may be exact
	  tmp64 = fstar.w[1] - 0x8000000000000000ull;	// f* - 1/2
	  if ((tmp64 > bid_ten2mk128trunc[ind - 1].w[1]
	       || (tmp64 == bid_ten2mk128trunc[ind - 1].w[1]
		   && fstar.w[0] >= bid_ten2mk128trunc[ind - 1].w[0]))) {
	    // set the inexact flag
	    *pfpsf |= BID_INEXACT_EXCEPTION;
	  }	// else the result is exact
	} else {	// the result is inexact; f2* <= 1/2
	  // set the inexact flag
	  *pfpsf |= BID_INEXACT_EXCEPTION;
	}
      } else if (ind - 1 <= 21) {	// if 3 <= ind <= 21
	if (fstar.w[3] > 0x0 ||
	    (fstar.w[3] == 0x0 && fstar.w[2] > bid_onehalf128[ind - 1]) ||
	    (fstar.w[3] == 0x0 && fstar.w[2] == bid_onehalf128[ind - 1] &&
	     (fstar.w[1] || fstar.w[0]))) {
	  // f2* > 1/2 and the result may be exact
	  // Calculate f2* - 1/2
	  tmp64 = fstar.w[2] - bid_onehalf128[ind - 1];
	  tmp64A = fstar.w[3];
	  if (tmp64 > fstar.w[2])
	    tmp64A--;
	  if (tmp64A || tmp64
	      || fstar.w[1] > bid_ten2mk128trunc[ind - 1].w[1]
	      || (fstar.w[1] == bid_ten2mk128trunc[ind - 1].w[1]
		  && fstar.w[0] > bid_ten2mk128trunc[ind - 1].w[0])) {
	    // set the inexact flag
	    *pfpsf |= BID_INEXACT_EXCEPTION;
	  }	// else the result is exact
	} else {	// the result is inexact; f2* <= 1/2
	  // set the inexact flag
	  *pfpsf |= BID_INEXACT_EXCEPTION;
	}
      } else {	// if 22 <= ind <= 33
	if (fstar.w[3] > bid_onehalf128[ind - 1] ||
	    (fstar.w[3] == bid_onehalf128[ind - 1] &&
	     (fstar.w[2] || fstar.w[1] || fstar.w[0]))) {
	  // f2* > 1/2 and the result may be exact
	  // Calculate f2* - 1/2
	  tmp64 = fstar.w[3] - bid_onehalf128[ind - 1];
	  if (tmp64 || fstar.w[2] ||
	      fstar.w[1] > bid_ten2mk128trunc[ind - 1].w[1]
	      || (fstar.w[1] == bid_ten2mk128trunc[ind - 1].w[1]
		  && fstar.w[0] > bid_ten2mk128trunc[ind - 1].w[0])) {
	    // set the inexact flag
	    *pfpsf |= BID_INEXACT_EXCEPTION;
	  }	// else the result is exact
	} else {	// the result is inexact; f2* <= 1/2
	  // set the inexact flag
	  *pfpsf |= BID_INEXACT_EXCEPTION;
	}
      }
      // no need to check for midpoints - already rounded away from zero!
    } else if (exp == 0) {
      // 1 <= q <= 10
      // res = +/-C (exact)
      if (x_sign)
	res = -C1.w[0];
      else
	res = C1.w[0];
    } else {	// if (exp > 0) => 1 <= exp <= 9, 1 <= q < 9, 2 <= q + exp <= 10
      // res = +/-C * 10^exp (exact)
      if (x_sign)
	res = -C1.w[0] * bid_ten2k64[exp];
      else
	res = C1.w[0] * bid_ten2k64[exp];
    }
  }
}

BID_RETURN_VAL (res);
}
