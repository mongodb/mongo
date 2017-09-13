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

#if DECIMAL_CALL_BY_REFERENCE
void
bid64dq_mul (BID_UINT64 * pres, BID_UINT64 * px, BID_UINT128 * py
	     _RND_MODE_PARAM _EXC_FLAGS_PARAM _EXC_MASKS_PARAM
	     _EXC_INFO_PARAM) {
  BID_UINT64 x = *px;
#if !DECIMAL_GLOBAL_ROUNDING
  unsigned int rnd_mode = *prnd_mode;
#endif
#else
BID_UINT64
bid64dq_mul (BID_UINT64 x, BID_UINT128 y
	     _RND_MODE_PARAM _EXC_FLAGS_PARAM _EXC_MASKS_PARAM
	     _EXC_INFO_PARAM) {
#endif
  BID_UINT64 res = 0xbaddbaddbaddbaddull;
  BID_UINT128 x1;

#if DECIMAL_CALL_BY_REFERENCE
  bid64_to_bid128 (&x1, &x _EXC_FLAGS_ARG _EXC_MASKS_ARG _EXC_INFO_ARG);
  bid64qq_mul (&res, &x1, py
	       _RND_MODE_ARG _EXC_FLAGS_ARG _EXC_MASKS_ARG
	       _EXC_INFO_ARG);
#else
  x1 = bid64_to_bid128 (x _EXC_FLAGS_ARG _EXC_MASKS_ARG _EXC_INFO_ARG);
  res = bid64qq_mul (x1, y
		     _RND_MODE_ARG _EXC_FLAGS_ARG _EXC_MASKS_ARG
		     _EXC_INFO_ARG);
#endif
  BID_RETURN (res);
}


#if DECIMAL_CALL_BY_REFERENCE
void
bid64qd_mul (BID_UINT64 * pres, BID_UINT128 * px, BID_UINT64 * py
	     _RND_MODE_PARAM _EXC_FLAGS_PARAM _EXC_MASKS_PARAM
	     _EXC_INFO_PARAM) {
  BID_UINT64 y = *py;
#if !DECIMAL_GLOBAL_ROUNDING
  unsigned int rnd_mode = *prnd_mode;
#endif
#else
BID_UINT64
bid64qd_mul (BID_UINT128 x, BID_UINT64 y
	     _RND_MODE_PARAM _EXC_FLAGS_PARAM _EXC_MASKS_PARAM
	     _EXC_INFO_PARAM) {
#endif
  BID_UINT64 res = 0xbaddbaddbaddbaddull;
  BID_UINT128 y1;

#if DECIMAL_CALL_BY_REFERENCE
  bid64_to_bid128 (&y1, &y _EXC_FLAGS_ARG _EXC_MASKS_ARG _EXC_INFO_ARG);
  bid64qq_mul (&res, px, &y1
	       _RND_MODE_ARG _EXC_FLAGS_ARG _EXC_MASKS_ARG
	       _EXC_INFO_ARG);
#else
  y1 = bid64_to_bid128 (y _EXC_FLAGS_ARG _EXC_MASKS_ARG _EXC_INFO_ARG);
  res = bid64qq_mul (x, y1
		     _RND_MODE_ARG _EXC_FLAGS_ARG _EXC_MASKS_ARG
		     _EXC_INFO_ARG);
#endif
  BID_RETURN (res);
}


#if DECIMAL_CALL_BY_REFERENCE
void
bid64qq_mul (BID_UINT64 * pres, BID_UINT128 * px, BID_UINT128 * py
	     _RND_MODE_PARAM _EXC_FLAGS_PARAM _EXC_MASKS_PARAM
	     _EXC_INFO_PARAM) {
  BID_UINT128 x = *px, y = *py;
#if !DECIMAL_GLOBAL_ROUNDING
  unsigned int rnd_mode = *prnd_mode;
#endif
#else
BID_UINT64
bid64qq_mul (BID_UINT128 x, BID_UINT128 y
	     _RND_MODE_PARAM _EXC_FLAGS_PARAM _EXC_MASKS_PARAM
	     _EXC_INFO_PARAM) {
#endif

  BID_UINT128 z = { {0x0000000000000000ull, 0x5ffe000000000000ull}
  };
  BID_UINT64 res = 0xbaddbaddbaddbaddull;
  BID_UINT64 x_sign, y_sign, p_sign;
  BID_UINT64 x_exp, y_exp, p_exp;
  int true_p_exp;
  BID_UINT128 C1, C2;

  BID_SWAP128 (z);
  // skip cases where at least one operand is NaN or infinity
  if (!(((x.w[BID_HIGH_128W] & MASK_NAN) == MASK_NAN) ||
	((y.w[BID_HIGH_128W] & MASK_NAN) == MASK_NAN) ||
	((x.w[BID_HIGH_128W] & MASK_ANY_INF) == MASK_INF) ||
	((y.w[BID_HIGH_128W] & MASK_ANY_INF) == MASK_INF))) {
    // x, y are 0 or f but not inf or NaN => unpack the arguments and check
    // for non-canonical values

    x_sign = x.w[BID_HIGH_128W] & MASK_SIGN;	// 0 for positive, MASK_SIGN for negative
    C1.w[1] = x.w[BID_HIGH_128W] & MASK_COEFF;
    C1.w[0] = x.w[BID_LOW_128W];
    // check for non-canonical values - treated as zero
    if ((x.w[BID_HIGH_128W] & 0x6000000000000000ull) ==
	0x6000000000000000ull) {
      // G0_G1=11 => non-canonical
      x_exp = (x.w[BID_HIGH_128W] << 2) & MASK_EXP;	// biased and shifted left 49 bits
      C1.w[1] = 0;	// significand high
      C1.w[0] = 0;	// significand low
    } else {	// G0_G1 != 11
      x_exp = x.w[BID_HIGH_128W] & MASK_EXP;	// biased and shifted left 49 bits
      if (C1.w[1] > 0x0001ed09bead87c0ull ||
	  (C1.w[1] == 0x0001ed09bead87c0ull &&
	   C1.w[0] > 0x378d8e63ffffffffull)) {
	// x is non-canonical if coefficient is larger than 10^34 -1
	C1.w[1] = 0;
	C1.w[0] = 0;
      } else {	// canonical          
	;
      }
    }
    y_sign = y.w[BID_HIGH_128W] & MASK_SIGN;	// 0 for positive, MASK_SIGN for negative
    C2.w[1] = y.w[BID_HIGH_128W] & MASK_COEFF;
    C2.w[0] = y.w[BID_LOW_128W];
    // check for non-canonical values - treated as zero
    if ((y.w[BID_HIGH_128W] & 0x6000000000000000ull) ==
	0x6000000000000000ull) {
      // G0_G1=11 => non-canonical
      y_exp = (y.w[BID_HIGH_128W] << 2) & MASK_EXP;	// biased and shifted left 49 bits
      C2.w[1] = 0;	// significand high
      C2.w[0] = 0;	// significand low 
    } else {	// G0_G1 != 11
      y_exp = y.w[BID_HIGH_128W] & MASK_EXP;	// biased and shifted left 49 bits
      if (C2.w[1] > 0x0001ed09bead87c0ull ||
	  (C2.w[1] == 0x0001ed09bead87c0ull &&
	   C2.w[0] > 0x378d8e63ffffffffull)) {
	// y is non-canonical if coefficient is larger than 10^34 -1
	C2.w[1] = 0;
	C2.w[0] = 0;
      } else {	// canonical
	;
      }
    }
    p_sign = x_sign ^ y_sign;	// sign of the product

    true_p_exp = (x_exp >> 49) - 6176 + (y_exp >> 49) - 6176;
    // true_p_exp, p_exp are used only for 0 * 0, 0 * f, or f * 0 
    if (true_p_exp < -398)
      p_exp = 0;	// cannot be less than EXP_MIN
    else if (true_p_exp > 369)
      p_exp = (BID_UINT64) (369 + 398) << 53;	// cannot be more than EXP_MAX
    else
      p_exp = (BID_UINT64) (true_p_exp + 398) << 53;

    if ((C1.w[1] == 0x0 && C1.w[0] == 0x0) ||
	(C2.w[1] == 0x0 && C2.w[0] == 0x0)) {
      // x = 0 or y = 0
      // the result is 0
      res = p_sign | p_exp;	// preferred exponent in [EXP_MIN, EXP_MAX]
      BID_RETURN (res)
    }	// else continue
  }
  // swap x and y - ensure that a NaN in x has 'higher precedence' than one in y
#if DECIMAL_CALL_BY_REFERENCE
  bid64qqq_fma (&res, &y, &x, &z
		_RND_MODE_ARG _EXC_FLAGS_ARG _EXC_MASKS_ARG
		_EXC_INFO_ARG);
#else
  res = bid64qqq_fma (y, x, z
		      _RND_MODE_ARG _EXC_FLAGS_ARG _EXC_MASKS_ARG
		      _EXC_INFO_ARG);
#endif
  BID_RETURN (res);
}


#if DECIMAL_CALL_BY_REFERENCE
void
bid128dd_mul (BID_UINT128 * pres, BID_UINT64 * px, BID_UINT64 * py
	      _RND_MODE_PARAM _EXC_FLAGS_PARAM _EXC_MASKS_PARAM
	      _EXC_INFO_PARAM) {
  BID_UINT64 x = *px, y = *py;
#if !DECIMAL_GLOBAL_ROUNDING
  unsigned int rnd_mode = *prnd_mode;
#endif
#else
BID_UINT128
bid128dd_mul (BID_UINT64 x, BID_UINT64 y
	      _RND_MODE_PARAM _EXC_FLAGS_PARAM _EXC_MASKS_PARAM
	      _EXC_INFO_PARAM) {
#endif
  BID_UINT128 res = { {0xbaddbaddbaddbaddull, 0xbaddbaddbaddbaddull}
  };
  BID_UINT128 x1, y1;

#if DECIMAL_CALL_BY_REFERENCE
  bid64_to_bid128 (&x1, &x _EXC_FLAGS_ARG _EXC_MASKS_ARG _EXC_INFO_ARG);
  bid64_to_bid128 (&y1, &y _EXC_FLAGS_ARG _EXC_MASKS_ARG _EXC_INFO_ARG);
  bid128_mul (&res, &x1, &y1
	      _RND_MODE_ARG _EXC_FLAGS_ARG _EXC_MASKS_ARG
	      _EXC_INFO_ARG);
#else
  x1 = bid64_to_bid128 (x _EXC_FLAGS_ARG _EXC_MASKS_ARG _EXC_INFO_ARG);
  y1 = bid64_to_bid128 (y _EXC_FLAGS_ARG _EXC_MASKS_ARG _EXC_INFO_ARG);
  res = bid128_mul (x1, y1
		    _RND_MODE_ARG _EXC_FLAGS_ARG _EXC_MASKS_ARG
		    _EXC_INFO_ARG);
#endif
  BID_RETURN (res);
}


#if DECIMAL_CALL_BY_REFERENCE
void
bid128dq_mul (BID_UINT128 * pres, BID_UINT64 * px, BID_UINT128 * py
	      _RND_MODE_PARAM _EXC_FLAGS_PARAM _EXC_MASKS_PARAM
	      _EXC_INFO_PARAM) {
  BID_UINT64 x = *px;
#if !DECIMAL_GLOBAL_ROUNDING
  unsigned int rnd_mode = *prnd_mode;
#endif
#else
BID_UINT128
bid128dq_mul (BID_UINT64 x, BID_UINT128 y
	      _RND_MODE_PARAM _EXC_FLAGS_PARAM _EXC_MASKS_PARAM
	      _EXC_INFO_PARAM) {
#endif
  BID_UINT128 res = { {0xbaddbaddbaddbaddull, 0xbaddbaddbaddbaddull}
  };
  BID_UINT128 x1;

#if DECIMAL_CALL_BY_REFERENCE
  bid64_to_bid128 (&x1, &x _EXC_FLAGS_ARG _EXC_MASKS_ARG _EXC_INFO_ARG);
  bid128_mul (&res, &x1, py
	      _RND_MODE_ARG _EXC_FLAGS_ARG _EXC_MASKS_ARG
	      _EXC_INFO_ARG);
#else
  x1 = bid64_to_bid128 (x _EXC_FLAGS_ARG _EXC_MASKS_ARG _EXC_INFO_ARG);
  res = bid128_mul (x1, y
		    _RND_MODE_ARG _EXC_FLAGS_ARG _EXC_MASKS_ARG
		    _EXC_INFO_ARG);
#endif
  BID_RETURN (res);
}


#if DECIMAL_CALL_BY_REFERENCE
void
bid128qd_mul (BID_UINT128 * pres, BID_UINT128 * px, BID_UINT64 * py
	      _RND_MODE_PARAM _EXC_FLAGS_PARAM _EXC_MASKS_PARAM
	      _EXC_INFO_PARAM) {
  BID_UINT64 y = *py;
#if !DECIMAL_GLOBAL_ROUNDING
  unsigned int rnd_mode = *prnd_mode;
#endif
#else
BID_UINT128
bid128qd_mul (BID_UINT128 x, BID_UINT64 y
	      _RND_MODE_PARAM _EXC_FLAGS_PARAM _EXC_MASKS_PARAM
	      _EXC_INFO_PARAM) {
#endif
  BID_UINT128 res = { {0xbaddbaddbaddbaddull, 0xbaddbaddbaddbaddull}
  };
  BID_UINT128 y1;

#if DECIMAL_CALL_BY_REFERENCE
  bid64_to_bid128 (&y1, &y _EXC_FLAGS_ARG _EXC_MASKS_ARG _EXC_INFO_ARG);
  bid128_mul (&res, px, &y1
	      _RND_MODE_ARG _EXC_FLAGS_ARG _EXC_MASKS_ARG
	      _EXC_INFO_ARG);
#else
  y1 = bid64_to_bid128 (y _EXC_FLAGS_ARG _EXC_MASKS_ARG _EXC_INFO_ARG);
  res = bid128_mul (x, y1
		    _RND_MODE_ARG _EXC_FLAGS_ARG _EXC_MASKS_ARG
		    _EXC_INFO_ARG);
#endif
  BID_RETURN (res);
}


// bid128_mul stands for bid128qq_mul
#if DECIMAL_CALL_BY_REFERENCE
void
bid128_mul (BID_UINT128 * pres, BID_UINT128 * px,
	    BID_UINT128 *
	    py _RND_MODE_PARAM _EXC_FLAGS_PARAM _EXC_MASKS_PARAM
	    _EXC_INFO_PARAM) {
  BID_UINT128 x = *px, y = *py;

#if !DECIMAL_GLOBAL_ROUNDING
  unsigned int rnd_mode = *prnd_mode;

#endif
#else
DFP_WRAPFN_DFP_DFP(128, bid128_mul, 128, 128)
BID_UINT128
bid128_mul (BID_UINT128 x,
	    BID_UINT128 y _RND_MODE_PARAM _EXC_FLAGS_PARAM _EXC_MASKS_PARAM
	    _EXC_INFO_PARAM) {

#endif
  BID_UINT128 z = { {0x0000000000000000ull, 0x5ffe000000000000ull}
  };
  BID_UINT128 res = { {0xbaddbaddbaddbaddull, 0xbaddbaddbaddbaddull}
  };
  BID_UINT64 x_sign, y_sign, p_sign;
  BID_UINT64 x_exp, y_exp, p_exp;
  int true_p_exp;
  BID_UINT128 C1, C2;

  BID_SWAP128 (x);
  BID_SWAP128 (y);
  // skip cases where at least one operand is NaN or infinity
  if (!(((x.w[1] & MASK_NAN) == MASK_NAN) ||
	((y.w[1] & MASK_NAN) == MASK_NAN) ||
	((x.w[1] & MASK_ANY_INF) == MASK_INF) ||
	((y.w[1] & MASK_ANY_INF) == MASK_INF))) {
    // x, y are 0 or f but not inf or NaN => unpack the arguments and check
    // for non-canonical values

    x_sign = x.w[1] & MASK_SIGN;	// 0 for positive, MASK_SIGN for negative
    C1.w[1] = x.w[1] & MASK_COEFF;
    C1.w[0] = x.w[0];
    // check for non-canonical values - treated as zero
    if ((x.w[1] & 0x6000000000000000ull) == 0x6000000000000000ull) {
      // G0_G1=11 => non-canonical
      x_exp = (x.w[1] << 2) & MASK_EXP;	// biased and shifted left 49 bits
      C1.w[1] = 0;	// significand high
      C1.w[0] = 0;	// significand low
    } else {	// G0_G1 != 11
      x_exp = x.w[1] & MASK_EXP;	// biased and shifted left 49 bits
      if (C1.w[1] > 0x0001ed09bead87c0ull ||
	  (C1.w[1] == 0x0001ed09bead87c0ull &&
	   C1.w[0] > 0x378d8e63ffffffffull)) {
	// x is non-canonical if coefficient is larger than 10^34 -1
	C1.w[1] = 0;
	C1.w[0] = 0;
      } else {	// canonical          
	;
      }
    }
    y_sign = y.w[1] & MASK_SIGN;	// 0 for positive, MASK_SIGN for negative
    C2.w[1] = y.w[1] & MASK_COEFF;
    C2.w[0] = y.w[0];
    // check for non-canonical values - treated as zero
    if ((y.w[1] & 0x6000000000000000ull) == 0x6000000000000000ull) {
      // G0_G1=11 => non-canonical
      y_exp = (y.w[1] << 2) & MASK_EXP;	// biased and shifted left 49 bits
      C2.w[1] = 0;	// significand high
      C2.w[0] = 0;	// significand low 
    } else {	// G0_G1 != 11
      y_exp = y.w[1] & MASK_EXP;	// biased and shifted left 49 bits
      if (C2.w[1] > 0x0001ed09bead87c0ull ||
	  (C2.w[1] == 0x0001ed09bead87c0ull &&
	   C2.w[0] > 0x378d8e63ffffffffull)) {
	// y is non-canonical if coefficient is larger than 10^34 -1
	C2.w[1] = 0;
	C2.w[0] = 0;
      } else {	// canonical
	;
      }
    }
    p_sign = x_sign ^ y_sign;	// sign of the product

    true_p_exp = (x_exp >> 49) - 6176 + (y_exp >> 49) - 6176;
    // true_p_exp, p_exp are used only for 0 * 0, 0 * f, or f * 0 
    if (true_p_exp < -6176)
      p_exp = 0;	// cannot be less than EXP_MIN
    else if (true_p_exp > 6111)
      p_exp = (BID_UINT64) (6111 + 6176) << 49;	// cannot be more than EXP_MAX
    else
      p_exp = (BID_UINT64) (true_p_exp + 6176) << 49;

    if ((C1.w[1] == 0x0 && C1.w[0] == 0x0) ||
	(C2.w[1] == 0x0 && C2.w[0] == 0x0)) {
      // x = 0 or y = 0
      // the result is 0
      res.w[1] = p_sign | p_exp;	// preferred exponent in [EXP_MIN, EXP_MAX]
      res.w[0] = 0x0;
      BID_SWAP128 (res);
      BID_RETURN (res)
    }	// else continue
  }

  BID_SWAP128 (x);
  BID_SWAP128 (y);
  BID_SWAP128 (z);
  // swap x and y - ensure that a NaN in x has 'higher precedence' than one in y
#if DECIMAL_CALL_BY_REFERENCE
  bid128_fma (&res, &y, &x, &z
	      _RND_MODE_ARG _EXC_FLAGS_ARG _EXC_MASKS_ARG
	      _EXC_INFO_ARG);
#else
  res = bid128_fma (y, x, z
		    _RND_MODE_ARG _EXC_FLAGS_ARG _EXC_MASKS_ARG
		    _EXC_INFO_ARG);
#endif
  BID_RETURN (res);
}
