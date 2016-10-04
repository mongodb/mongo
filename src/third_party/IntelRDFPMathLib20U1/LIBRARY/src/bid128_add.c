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
bid64dq_add (BID_UINT64 * pres, BID_UINT64 * px, BID_UINT128 * py
	     _RND_MODE_PARAM _EXC_FLAGS_PARAM _EXC_MASKS_PARAM
	     _EXC_INFO_PARAM) {
  BID_UINT64 x = *px;
#if !DECIMAL_GLOBAL_ROUNDING
  unsigned int rnd_mode = *prnd_mode;
#endif
#else
BID_UINT64
bid64dq_add (BID_UINT64 x, BID_UINT128 y
	     _RND_MODE_PARAM _EXC_FLAGS_PARAM _EXC_MASKS_PARAM
	     _EXC_INFO_PARAM) {
#endif
  BID_UINT64 res = 0xbaddbaddbaddbaddull;
  BID_UINT128 x1;

#if DECIMAL_CALL_BY_REFERENCE
  bid64_to_bid128 (&x1, &x _EXC_FLAGS_ARG _EXC_MASKS_ARG _EXC_INFO_ARG);
  bid64qq_add (&res, &x1, py
	       _RND_MODE_ARG _EXC_FLAGS_ARG _EXC_MASKS_ARG
	       _EXC_INFO_ARG);
#else
  x1 = bid64_to_bid128 (x _EXC_FLAGS_ARG _EXC_MASKS_ARG _EXC_INFO_ARG);
  res = bid64qq_add (x1, y
		     _RND_MODE_ARG _EXC_FLAGS_ARG _EXC_MASKS_ARG
		     _EXC_INFO_ARG);
#endif
  BID_RETURN (res);
}


#if DECIMAL_CALL_BY_REFERENCE
void
bid64qd_add (BID_UINT64 * pres, BID_UINT128 * px, BID_UINT64 * py
	     _RND_MODE_PARAM _EXC_FLAGS_PARAM _EXC_MASKS_PARAM
	     _EXC_INFO_PARAM) {
  BID_UINT64 y = *py;
#if !DECIMAL_GLOBAL_ROUNDING
  unsigned int rnd_mode = *prnd_mode;
#endif
#else
BID_UINT64
bid64qd_add (BID_UINT128 x, BID_UINT64 y
	     _RND_MODE_PARAM _EXC_FLAGS_PARAM _EXC_MASKS_PARAM
	     _EXC_INFO_PARAM) {
#endif
  BID_UINT64 res = 0xbaddbaddbaddbaddull;
  BID_UINT128 y1;

#if DECIMAL_CALL_BY_REFERENCE
  bid64_to_bid128 (&y1, &y _EXC_FLAGS_ARG _EXC_MASKS_ARG _EXC_INFO_ARG);
  bid64qq_add (&res, px, &y1
	       _RND_MODE_ARG _EXC_FLAGS_ARG _EXC_MASKS_ARG
	       _EXC_INFO_ARG);
#else
  y1 = bid64_to_bid128 (y _EXC_FLAGS_ARG _EXC_MASKS_ARG _EXC_INFO_ARG);
  res = bid64qq_add (x, y1
		     _RND_MODE_ARG _EXC_FLAGS_ARG _EXC_MASKS_ARG
		     _EXC_INFO_ARG);
#endif
  BID_RETURN (res);
}


#if DECIMAL_CALL_BY_REFERENCE
void
bid64qq_add (BID_UINT64 * pres, BID_UINT128 * px, BID_UINT128 * py
	     _RND_MODE_PARAM _EXC_FLAGS_PARAM _EXC_MASKS_PARAM
	     _EXC_INFO_PARAM) {
  BID_UINT128 x = *px, y = *py;
#if !DECIMAL_GLOBAL_ROUNDING
  unsigned int rnd_mode = *prnd_mode;
#endif
#else
BID_UINT64
bid64qq_add (BID_UINT128 x, BID_UINT128 y
	     _RND_MODE_PARAM _EXC_FLAGS_PARAM _EXC_MASKS_PARAM
	     _EXC_INFO_PARAM) {
#endif

  BID_UINT128 one = { {0x0000000000000001ull, 0x3040000000000000ull}
  };
  BID_UINT64 res = 0xbaddbaddbaddbaddull;

  BID_SWAP128 (one);
#if DECIMAL_CALL_BY_REFERENCE
  bid64qqq_fma (&res, &one, &x, &y
		_RND_MODE_ARG _EXC_FLAGS_ARG _EXC_MASKS_ARG
		_EXC_INFO_ARG);
#else
  res = bid64qqq_fma (one, x, y
		      _RND_MODE_ARG _EXC_FLAGS_ARG _EXC_MASKS_ARG
		      _EXC_INFO_ARG);
#endif
  BID_RETURN (res);
}


#if DECIMAL_CALL_BY_REFERENCE
void
bid128dd_add (BID_UINT128 * pres, BID_UINT64 * px, BID_UINT64 * py
	      _RND_MODE_PARAM _EXC_FLAGS_PARAM _EXC_MASKS_PARAM
	      _EXC_INFO_PARAM) {
  BID_UINT64 x = *px, y = *py;
#if !DECIMAL_GLOBAL_ROUNDING
  unsigned int rnd_mode = *prnd_mode;
#endif
#else
BID_UINT128
bid128dd_add (BID_UINT64 x, BID_UINT64 y
	      _RND_MODE_PARAM _EXC_FLAGS_PARAM _EXC_MASKS_PARAM
	      _EXC_INFO_PARAM) {
#endif
  BID_UINT128 res = { {0xbaddbaddbaddbaddull, 0xbaddbaddbaddbaddull}
  };
  BID_UINT128 x1, y1;

#if DECIMAL_CALL_BY_REFERENCE
  bid64_to_bid128 (&x1, &x _EXC_FLAGS_ARG _EXC_MASKS_ARG _EXC_INFO_ARG);
  bid64_to_bid128 (&y1, &y _EXC_FLAGS_ARG _EXC_MASKS_ARG _EXC_INFO_ARG);
  bid128_add (&res, &x1, &y1
	      _RND_MODE_ARG _EXC_FLAGS_ARG _EXC_MASKS_ARG
	      _EXC_INFO_ARG);
#else
  x1 = bid64_to_bid128 (x _EXC_FLAGS_ARG _EXC_MASKS_ARG _EXC_INFO_ARG);
  y1 = bid64_to_bid128 (y _EXC_FLAGS_ARG _EXC_MASKS_ARG _EXC_INFO_ARG);
  res = bid128_add (x1, y1
		    _RND_MODE_ARG _EXC_FLAGS_ARG _EXC_MASKS_ARG
		    _EXC_INFO_ARG);
#endif
  BID_RETURN (res);
}


#if DECIMAL_CALL_BY_REFERENCE
void
bid128dq_add (BID_UINT128 * pres, BID_UINT64 * px, BID_UINT128 * py
	      _RND_MODE_PARAM _EXC_FLAGS_PARAM _EXC_MASKS_PARAM
	      _EXC_INFO_PARAM) {
  BID_UINT64 x = *px;
#if !DECIMAL_GLOBAL_ROUNDING
  unsigned int rnd_mode = *prnd_mode;
#endif
#else
BID_UINT128
bid128dq_add (BID_UINT64 x, BID_UINT128 y
	      _RND_MODE_PARAM _EXC_FLAGS_PARAM _EXC_MASKS_PARAM
	      _EXC_INFO_PARAM) {
#endif
  BID_UINT128 res = { {0xbaddbaddbaddbaddull, 0xbaddbaddbaddbaddull}
  };
  BID_UINT128 x1;

#if DECIMAL_CALL_BY_REFERENCE
  bid64_to_bid128 (&x1, &x _EXC_FLAGS_ARG _EXC_MASKS_ARG _EXC_INFO_ARG);
  bid128_add (&res, &x1, py
	      _RND_MODE_ARG _EXC_FLAGS_ARG _EXC_MASKS_ARG
	      _EXC_INFO_ARG);
#else
  x1 = bid64_to_bid128 (x _EXC_FLAGS_ARG _EXC_MASKS_ARG _EXC_INFO_ARG);
  res = bid128_add (x1, y
		    _RND_MODE_ARG _EXC_FLAGS_ARG _EXC_MASKS_ARG
		    _EXC_INFO_ARG);
#endif
  BID_RETURN (res);
}


#if DECIMAL_CALL_BY_REFERENCE
void
bid128qd_add (BID_UINT128 * pres, BID_UINT128 * px, BID_UINT64 * py
	      _RND_MODE_PARAM _EXC_FLAGS_PARAM _EXC_MASKS_PARAM
	      _EXC_INFO_PARAM) {
  BID_UINT64 y = *py;
#if !DECIMAL_GLOBAL_ROUNDING
  unsigned int rnd_mode = *prnd_mode;
#endif
#else
BID_UINT128
bid128qd_add (BID_UINT128 x, BID_UINT64 y
	      _RND_MODE_PARAM _EXC_FLAGS_PARAM _EXC_MASKS_PARAM
	      _EXC_INFO_PARAM) {
#endif
  BID_UINT128 res = { {0xbaddbaddbaddbaddull, 0xbaddbaddbaddbaddull}
  };
  BID_UINT128 y1;

#if DECIMAL_CALL_BY_REFERENCE
  bid64_to_bid128 (&y1, &y _EXC_FLAGS_ARG _EXC_MASKS_ARG _EXC_INFO_ARG);
  bid128_add (&res, px, &y1
	      _RND_MODE_ARG _EXC_FLAGS_ARG _EXC_MASKS_ARG
	      _EXC_INFO_ARG);
#else
  y1 = bid64_to_bid128 (y _EXC_FLAGS_ARG _EXC_MASKS_ARG _EXC_INFO_ARG);
  res = bid128_add (x, y1
		    _RND_MODE_ARG _EXC_FLAGS_ARG _EXC_MASKS_ARG
		    _EXC_INFO_ARG);
#endif
  BID_RETURN (res);
}


// bid128_add stands for bid128qq_add


/*****************************************************************************
 *  BID64/BID128 sub
 ****************************************************************************/

#if DECIMAL_CALL_BY_REFERENCE
void
bid64dq_sub (BID_UINT64 * pres, BID_UINT64 * px, BID_UINT128 * py
	     _RND_MODE_PARAM _EXC_FLAGS_PARAM _EXC_MASKS_PARAM
	     _EXC_INFO_PARAM) {
  BID_UINT64 x = *px;
#if !DECIMAL_GLOBAL_ROUNDING
  unsigned int rnd_mode = *prnd_mode;
#endif
#else
BID_UINT64
bid64dq_sub (BID_UINT64 x, BID_UINT128 y
	     _RND_MODE_PARAM _EXC_FLAGS_PARAM _EXC_MASKS_PARAM
	     _EXC_INFO_PARAM) {
#endif
  BID_UINT64 res = 0xbaddbaddbaddbaddull;
  BID_UINT128 x1;

#if DECIMAL_CALL_BY_REFERENCE
  bid64_to_bid128 (&x1, &x _EXC_FLAGS_ARG _EXC_MASKS_ARG _EXC_INFO_ARG);
  bid64qq_sub (&res, &x1, py
	       _RND_MODE_ARG _EXC_FLAGS_ARG _EXC_MASKS_ARG
	       _EXC_INFO_ARG);
#else
  x1 = bid64_to_bid128 (x _EXC_FLAGS_ARG _EXC_MASKS_ARG _EXC_INFO_ARG);
  res = bid64qq_sub (x1, y
		     _RND_MODE_ARG _EXC_FLAGS_ARG _EXC_MASKS_ARG
		     _EXC_INFO_ARG);
#endif
  BID_RETURN (res);
}


#if DECIMAL_CALL_BY_REFERENCE
void
bid64qd_sub (BID_UINT64 * pres, BID_UINT128 * px, BID_UINT64 * py
	     _RND_MODE_PARAM _EXC_FLAGS_PARAM _EXC_MASKS_PARAM
	     _EXC_INFO_PARAM) {
  BID_UINT64 y = *py;
#if !DECIMAL_GLOBAL_ROUNDING
  unsigned int rnd_mode = *prnd_mode;
#endif
#else
BID_UINT64
bid64qd_sub (BID_UINT128 x, BID_UINT64 y
	     _RND_MODE_PARAM _EXC_FLAGS_PARAM _EXC_MASKS_PARAM
	     _EXC_INFO_PARAM) {
#endif
  BID_UINT64 res = 0xbaddbaddbaddbaddull;
  BID_UINT128 y1;

#if DECIMAL_CALL_BY_REFERENCE
  bid64_to_bid128 (&y1, &y _EXC_FLAGS_ARG _EXC_MASKS_ARG _EXC_INFO_ARG);
  bid64qq_sub (&res, px, &y1
	       _RND_MODE_ARG _EXC_FLAGS_ARG _EXC_MASKS_ARG
	       _EXC_INFO_ARG);
#else
  y1 = bid64_to_bid128 (y _EXC_FLAGS_ARG _EXC_MASKS_ARG _EXC_INFO_ARG);
  res = bid64qq_sub (x, y1
		     _RND_MODE_ARG _EXC_FLAGS_ARG _EXC_MASKS_ARG
		     _EXC_INFO_ARG);
#endif
  BID_RETURN (res);
}


#if DECIMAL_CALL_BY_REFERENCE
void
bid64qq_sub (BID_UINT64 * pres, BID_UINT128 * px, BID_UINT128 * py
	     _RND_MODE_PARAM _EXC_FLAGS_PARAM _EXC_MASKS_PARAM
	     _EXC_INFO_PARAM) {
  BID_UINT128 x = *px, y = *py;
#if !DECIMAL_GLOBAL_ROUNDING
  unsigned int rnd_mode = *prnd_mode;
#endif
#else
BID_UINT64
bid64qq_sub (BID_UINT128 x, BID_UINT128 y
	     _RND_MODE_PARAM _EXC_FLAGS_PARAM _EXC_MASKS_PARAM
	     _EXC_INFO_PARAM) {
#endif

  BID_UINT128 one = { {0x0000000000000001ull, 0x3040000000000000ull}
  };
  BID_UINT64 res = 0xbaddbaddbaddbaddull;
  BID_UINT64 y_sign;

  BID_SWAP128 (one);
  if ((y.w[BID_HIGH_128W] & MASK_NAN) != MASK_NAN) {	// y is not NAN
    // change its sign
    y_sign = y.w[BID_HIGH_128W] & MASK_SIGN;	// 0 for positive, MASK_SIGN for negative
    if (y_sign)
      y.w[BID_HIGH_128W] = y.w[BID_HIGH_128W] & 0x7fffffffffffffffull;
    else
      y.w[BID_HIGH_128W] = y.w[BID_HIGH_128W] | 0x8000000000000000ull;
  }
#if DECIMAL_CALL_BY_REFERENCE
  bid64qqq_fma (&res, &one, &x, &y
		_RND_MODE_ARG _EXC_FLAGS_ARG _EXC_MASKS_ARG
		_EXC_INFO_ARG);
#else
  res = bid64qqq_fma (one, x, y
		      _RND_MODE_ARG _EXC_FLAGS_ARG _EXC_MASKS_ARG
		      _EXC_INFO_ARG);
#endif
  BID_RETURN (res);
}


#if DECIMAL_CALL_BY_REFERENCE
void
bid128dd_sub (BID_UINT128 * pres, BID_UINT64 * px, BID_UINT64 * py
	      _RND_MODE_PARAM _EXC_FLAGS_PARAM _EXC_MASKS_PARAM
	      _EXC_INFO_PARAM) {
  BID_UINT64 x = *px, y = *py;
#if !DECIMAL_GLOBAL_ROUNDING
  unsigned int rnd_mode = *prnd_mode;
#endif
#else
BID_UINT128
bid128dd_sub (BID_UINT64 x, BID_UINT64 y
	      _RND_MODE_PARAM _EXC_FLAGS_PARAM _EXC_MASKS_PARAM
	      _EXC_INFO_PARAM) {
#endif
  BID_UINT128 res = { {0xbaddbaddbaddbaddull, 0xbaddbaddbaddbaddull}
  };
  BID_UINT128 x1, y1;

#if DECIMAL_CALL_BY_REFERENCE
  bid64_to_bid128 (&x1, &x _EXC_FLAGS_ARG _EXC_MASKS_ARG _EXC_INFO_ARG);
  bid64_to_bid128 (&y1, &y _EXC_FLAGS_ARG _EXC_MASKS_ARG _EXC_INFO_ARG);
  bid128_sub (&res, &x1, &y1
	      _RND_MODE_ARG _EXC_FLAGS_ARG _EXC_MASKS_ARG
	      _EXC_INFO_ARG);
#else
  x1 = bid64_to_bid128 (x _EXC_FLAGS_ARG _EXC_MASKS_ARG _EXC_INFO_ARG);
  y1 = bid64_to_bid128 (y _EXC_FLAGS_ARG _EXC_MASKS_ARG _EXC_INFO_ARG);
  res = bid128_sub (x1, y1
		    _RND_MODE_ARG _EXC_FLAGS_ARG _EXC_MASKS_ARG
		    _EXC_INFO_ARG);
#endif
  BID_RETURN (res);
}


#if DECIMAL_CALL_BY_REFERENCE
void
bid128dq_sub (BID_UINT128 * pres, BID_UINT64 * px, BID_UINT128 * py
	      _RND_MODE_PARAM _EXC_FLAGS_PARAM _EXC_MASKS_PARAM
	      _EXC_INFO_PARAM) {
  BID_UINT64 x = *px;
#if !DECIMAL_GLOBAL_ROUNDING
  unsigned int rnd_mode = *prnd_mode;
#endif
#else
BID_UINT128
bid128dq_sub (BID_UINT64 x, BID_UINT128 y
	      _RND_MODE_PARAM _EXC_FLAGS_PARAM _EXC_MASKS_PARAM
	      _EXC_INFO_PARAM) {
#endif
  BID_UINT128 res = { {0xbaddbaddbaddbaddull, 0xbaddbaddbaddbaddull}
  };
  BID_UINT128 x1;

#if DECIMAL_CALL_BY_REFERENCE
  bid64_to_bid128 (&x1, &x _EXC_FLAGS_ARG _EXC_MASKS_ARG _EXC_INFO_ARG);
  bid128_sub (&res, &x1, py
	      _RND_MODE_ARG _EXC_FLAGS_ARG _EXC_MASKS_ARG
	      _EXC_INFO_ARG);
#else
  x1 = bid64_to_bid128 (x _EXC_FLAGS_ARG _EXC_MASKS_ARG _EXC_INFO_ARG);
  res = bid128_sub (x1, y
		    _RND_MODE_ARG _EXC_FLAGS_ARG _EXC_MASKS_ARG
		    _EXC_INFO_ARG);
#endif
  BID_RETURN (res);
}


#if DECIMAL_CALL_BY_REFERENCE
void
bid128qd_sub (BID_UINT128 * pres, BID_UINT128 * px, BID_UINT64 * py
	      _RND_MODE_PARAM _EXC_FLAGS_PARAM _EXC_MASKS_PARAM
	      _EXC_INFO_PARAM) {
  BID_UINT64 y = *py;
#if !DECIMAL_GLOBAL_ROUNDING
  unsigned int rnd_mode = *prnd_mode;
#endif
#else
BID_UINT128
bid128qd_sub (BID_UINT128 x, BID_UINT64 y
	      _RND_MODE_PARAM _EXC_FLAGS_PARAM _EXC_MASKS_PARAM
	      _EXC_INFO_PARAM) {
#endif
  BID_UINT128 res = { {0xbaddbaddbaddbaddull, 0xbaddbaddbaddbaddull}
  };
  BID_UINT128 y1;

#if DECIMAL_CALL_BY_REFERENCE
  bid64_to_bid128 (&y1, &y _EXC_FLAGS_ARG _EXC_MASKS_ARG _EXC_INFO_ARG);
  bid128_sub (&res, px, &y1
	      _RND_MODE_ARG _EXC_FLAGS_ARG _EXC_MASKS_ARG
	      _EXC_INFO_ARG);
#else
  y1 = bid64_to_bid128 (y _EXC_FLAGS_ARG _EXC_MASKS_ARG _EXC_INFO_ARG);
  res = bid128_sub (x, y1
		    _RND_MODE_ARG _EXC_FLAGS_ARG _EXC_MASKS_ARG
		    _EXC_INFO_ARG);
#endif
  BID_RETURN (res);
}

#if DECIMAL_CALL_BY_REFERENCE
void
bid128_add (BID_UINT128 * pres, BID_UINT128 * px, BID_UINT128 * py
	    _RND_MODE_PARAM _EXC_FLAGS_PARAM _EXC_MASKS_PARAM
	    _EXC_INFO_PARAM) {
  BID_UINT128 x = *px, y = *py;
#if !DECIMAL_GLOBAL_ROUNDING
  unsigned int rnd_mode = *prnd_mode;
#endif
#else
DFP_WRAPFN_DFP_DFP(128, bid128_add, 128, 128)
BID_UINT128
bid128_add (BID_UINT128 x, BID_UINT128 y
	    _RND_MODE_PARAM _EXC_FLAGS_PARAM _EXC_MASKS_PARAM
	    _EXC_INFO_PARAM) {
#endif
  BID_UINT128 res = { {0xbaddbaddbaddbaddull, 0xbaddbaddbaddbaddull}
  };
  BID_UINT64 x_sign, y_sign, tmp_sign;
  BID_UINT64 x_exp, y_exp, tmp_exp;	// e1 = x_exp, e2 = y_exp
  BID_UINT64 C1_hi, C2_hi, tmp_signif_hi;
  BID_UINT64 C1_lo, C2_lo, tmp_signif_lo;
  // Note: C1.w[1], C1.w[0] represent C1_hi, C1_lo (all BID_UINT64)
  // Note: C2.w[1], C2.w[0] represent C2_hi, C2_lo (all BID_UINT64)
  BID_UINT64 tmp64, tmp64A, tmp64B;
  BID_UI64DOUBLE tmp1, tmp2;
  int x_nr_bits, y_nr_bits;
  int q1, q2, delta, scale, x1, ind, shift, tmp_inexact = 0;
  BID_UINT64 halfulp64;
  BID_UINT128 halfulp128;
  BID_UINT128 C1, C2;
  BID_UINT128 ten2m1;
  BID_UINT128 highf2star;		// top 128 bits in f2*; low 128 bits in R256[1], R256[0]
  BID_UINT256 P256, Q256, R256;
  int is_inexact = 0, is_midpoint_lt_even = 0, is_midpoint_gt_even = 0;
  int is_inexact_lt_midpoint = 0, is_inexact_gt_midpoint = 0;
  int second_pass = 0;

  BID_SWAP128 (x);
  BID_SWAP128 (y);
  x_sign = x.w[1] & MASK_SIGN;	// 0 for positive, MASK_SIGN for negative
  y_sign = y.w[1] & MASK_SIGN;	// 0 for positive, MASK_SIGN for negative

  // check for NaN or Infinity
  if (((x.w[1] & MASK_SPECIAL) == MASK_SPECIAL)
      || ((y.w[1] & MASK_SPECIAL) == MASK_SPECIAL)) {
    // x is special or y is special
    if ((x.w[1] & MASK_NAN) == MASK_NAN) {	// x is NAN
      // check first for non-canonical NaN payload
      if (((x.w[1] & 0x00003fffffffffffull) > 0x0000314dc6448d93ull) ||
	  (((x.w[1] & 0x00003fffffffffffull) == 0x0000314dc6448d93ull)
	   && (x.w[0] > 0x38c15b09ffffffffull))) {
	x.w[1] = x.w[1] & 0xffffc00000000000ull;
	x.w[0] = 0x0ull;
      }
      if ((x.w[1] & MASK_SNAN) == MASK_SNAN) {	// x is SNAN
	// set invalid flag
	*pfpsf |= BID_INVALID_EXCEPTION;
	// return quiet (x)
	res.w[1] = x.w[1] & 0xfc003fffffffffffull;
	// clear out also G[6]-G[16]
	res.w[0] = x.w[0];
      } else {	// x is QNaN
	// return x
	res.w[1] = x.w[1] & 0xfc003fffffffffffull;
	// clear out G[6]-G[16]
	res.w[0] = x.w[0];
	// if y = SNaN signal invalid exception
	if ((y.w[1] & MASK_SNAN) == MASK_SNAN) {
	  // set invalid flag
	  *pfpsf |= BID_INVALID_EXCEPTION;
	}
      }
      BID_SWAP128 (res);
      BID_RETURN (res);
    } else if ((y.w[1] & MASK_NAN) == MASK_NAN) {	// y is NAN
      // check first for non-canonical NaN payload
      if (((y.w[1] & 0x00003fffffffffffull) > 0x0000314dc6448d93ull) ||
	  (((y.w[1] & 0x00003fffffffffffull) == 0x0000314dc6448d93ull)
	   && (y.w[0] > 0x38c15b09ffffffffull))) {
	y.w[1] = y.w[1] & 0xffffc00000000000ull;
	y.w[0] = 0x0ull;
      }
      if ((y.w[1] & MASK_SNAN) == MASK_SNAN) {	// y is SNAN
	// set invalid flag
	*pfpsf |= BID_INVALID_EXCEPTION;
	// return quiet (y)
	res.w[1] = y.w[1] & 0xfc003fffffffffffull;
	// clear out also G[6]-G[16]
	res.w[0] = y.w[0];
      } else {	// y is QNaN
	// return y
	res.w[1] = y.w[1] & 0xfc003fffffffffffull;
	// clear out G[6]-G[16]
	res.w[0] = y.w[0];
      }
      BID_SWAP128 (res);
      BID_RETURN (res);
    } else {	// neither x not y is NaN; at least one is infinity
      if ((x.w[1] & MASK_ANY_INF) == MASK_INF) {	// x is infinity
	if ((y.w[1] & MASK_ANY_INF) == MASK_INF) {	// y is infinity
	  // if same sign, return either of them
	  if ((x.w[1] & MASK_SIGN) == (y.w[1] & MASK_SIGN)) {
	    res.w[1] = x_sign | MASK_INF;
	    res.w[0] = 0x0ull;
	  } else {	// x and y are infinities of opposite signs
	    // set invalid flag
	    *pfpsf |= BID_INVALID_EXCEPTION;
	    // return QNaN Indefinite
	    res.w[1] = 0x7c00000000000000ull;
	    res.w[0] = 0x0000000000000000ull;
	  }
	} else {	// y is 0 or finite
	  // return x
	  res.w[1] = x_sign | MASK_INF;
	  res.w[0] = 0x0ull;
	}
      } else {	// x is not NaN or infinity, so y must be infinity
	res.w[1] = y_sign | MASK_INF;
	res.w[0] = 0x0ull;
      }
      BID_SWAP128 (res);
      BID_RETURN (res);
    }
  }
  // unpack the arguments

  // unpack x 
  C1_hi = x.w[1] & MASK_COEFF;
  C1_lo = x.w[0];
  // test for non-canonical values:
  // - values whose encoding begins with x00, x01, or x10 and whose 
  //   coefficient is larger than 10^34 -1, or
  // - values whose encoding begins with x1100, x1101, x1110 (if NaNs 
  //   and infinitis were eliminated already this test is reduced to 
  //   checking for x10x) 

  // x is not infinity; check for non-canonical values - treated as zero
  if ((x.w[1] & 0x6000000000000000ull) == 0x6000000000000000ull) {
    // G0_G1=11; non-canonical
    x_exp = (x.w[1] << 2) & MASK_EXP;	// biased and shifted left 49 bits
    C1_hi = 0;	// significand high
    C1_lo = 0;	// significand low
  } else {	// G0_G1 != 11
    x_exp = x.w[1] & MASK_EXP;	// biased and shifted left 49 bits
    if (C1_hi > 0x0001ed09bead87c0ull ||
	(C1_hi == 0x0001ed09bead87c0ull
	 && C1_lo > 0x378d8e63ffffffffull)) {
      // x is non-canonical if coefficient is larger than 10^34 -1
      C1_hi = 0;
      C1_lo = 0;
    } else {	// canonical
      ;
    }
  }

  // unpack y  
  C2_hi = y.w[1] & MASK_COEFF;
  C2_lo = y.w[0];
  // y is not infinity; check for non-canonical values - treated as zero 
  if ((y.w[1] & 0x6000000000000000ull) == 0x6000000000000000ull) {
    // G0_G1=11; non-canonical 
    y_exp = (y.w[1] << 2) & MASK_EXP;	// biased and shifted left 49 bits
    C2_hi = 0;	// significand high
    C2_lo = 0;	// significand low 
  } else {	// G0_G1 != 11 
    y_exp = y.w[1] & MASK_EXP;	// biased and shifted left 49 bits
    if (C2_hi > 0x0001ed09bead87c0ull ||
	(C2_hi == 0x0001ed09bead87c0ull
	 && C2_lo > 0x378d8e63ffffffffull)) {
      // y is non-canonical if coefficient is larger than 10^34 -1 
      C2_hi = 0;
      C2_lo = 0;
    } else {	// canonical
      ;
    }
  }

  if ((C1_hi == 0x0ull) && (C1_lo == 0x0ull)) {
    // x is 0 and y is not special
    // if y is 0 return 0 with the smaller exponent
    if ((C2_hi == 0x0ull) && (C2_lo == 0x0ull)) {
      if (x_exp < y_exp)
	res.w[1] = x_exp;
      else
	res.w[1] = y_exp;
      if (x_sign && y_sign)
	res.w[1] = res.w[1] | x_sign;	// both negative
      else if (rnd_mode == BID_ROUNDING_DOWN && x_sign != y_sign)
	res.w[1] = res.w[1] | 0x8000000000000000ull;	// -0
      // else; // res = +0
      res.w[0] = 0;
    } else {
      // for 0 + y return y, with the preferred exponent
      if (y_exp <= x_exp) {
	res.w[1] = y.w[1];
	res.w[0] = y.w[0];
      } else {	// if y_exp > x_exp
	// return (C2 * 10^scale) * 10^(y_exp - scale)
	// where scale = min (P34-q2, y_exp-x_exp)
	// determine q2 = nr. of decimal digits in y
	//  determine first the nr. of bits in y (y_nr_bits)

	if (C2_hi == 0) {	// y_bits is the nr. of bits in C2_lo
	  if (C2_lo >= 0x0020000000000000ull) {	// y >= 2^53
	    // split the 64-bit value in two 32-bit halves to avoid 
	    // rounding errors
            tmp2.d = (double) (C2_lo >> 32);	// exact conversion
            y_nr_bits = 
                32 + ((((unsigned int) (tmp2.ui64 >> 52)) & 0x7ff) - 0x3ff);
	  } else {	// if y < 2^53
	    tmp2.d = (double) C2_lo;	// exact conversion
	    y_nr_bits =
	      ((((unsigned int) (tmp2.ui64 >> 52)) & 0x7ff) - 0x3ff);
	  }
	} else {	// C2_hi != 0 => nr. bits = 64 + nr_bits (C2_hi)
	  tmp2.d = (double) C2_hi;	// exact conversion
	  y_nr_bits =
	    64 + ((((unsigned int) (tmp2.ui64 >> 52)) & 0x7ff) - 0x3ff);
	}
	q2 = bid_nr_digits[y_nr_bits].digits;
	if (q2 == 0) {
	  q2 = bid_nr_digits[y_nr_bits].digits1;
	  if (C2_hi > bid_nr_digits[y_nr_bits].threshold_hi ||
	      (C2_hi == bid_nr_digits[y_nr_bits].threshold_hi &&
	       C2_lo >= bid_nr_digits[y_nr_bits].threshold_lo))
	    q2++;
	}
	// return (C2 * 10^scale) * 10^(y_exp - scale)
	// where scale = min (P34-q2, y_exp-x_exp)
	scale = P34 - q2;
	ind = (y_exp - x_exp) >> 49;
	if (ind < scale)
	  scale = ind;
	if (scale == 0) {
	  res.w[1] = y.w[1];
	  res.w[0] = y.w[0];
	} else if (q2 <= 19) {	// y fits in 64 bits 
	  if (scale <= 19) {	// 10^scale fits in 64 bits
	    // 64 x 64 C2_lo * bid_ten2k64[scale]
	    __mul_64x64_to_128MACH (res, C2_lo, bid_ten2k64[scale]);
	  } else {	// 10^scale fits in 128 bits
	    // 64 x 128 C2_lo * bid_ten2k128[scale - 20]
	    __mul_128x64_to_128 (res, C2_lo, bid_ten2k128[scale - 20]);
	  }
	} else {	// y fits in 128 bits, but 10^scale must fit in 64 bits 
	  // 64 x 128 bid_ten2k64[scale] * C2
	  C2.w[1] = C2_hi;
	  C2.w[0] = C2_lo;
	  __mul_128x64_to_128 (res, bid_ten2k64[scale], C2);
	}
	// subtract scale from the exponent
	y_exp = y_exp - ((BID_UINT64) scale << 49);
	res.w[1] = res.w[1] | y_sign | y_exp;
      }
    }
    BID_SWAP128 (res);
    BID_RETURN (res);
  } else if ((C2_hi == 0x0ull) && (C2_lo == 0x0ull)) {
    // y is 0 and x is not special, and not zero
    // for x + 0 return x, with the preferred exponent
    if (x_exp <= y_exp) {
      res.w[1] = x.w[1];
      res.w[0] = x.w[0];
    } else {	// if x_exp > y_exp
      // return (C1 * 10^scale) * 10^(x_exp - scale)
      // where scale = min (P34-q1, x_exp-y_exp)
      // determine q1 = nr. of decimal digits in x
      //  determine first the nr. of bits in x
      if (C1_hi == 0) {	// x_bits is the nr. of bits in C1_lo
	if (C1_lo >= 0x0020000000000000ull) {	// x >= 2^53
	  // split the 64-bit value in two 32-bit halves to avoid 
	  // rounding errors
          tmp1.d = (double) (C1_lo >> 32);	// exact conversion
          x_nr_bits =
            32 + ((((unsigned int) (tmp1.ui64 >> 52)) & 0x7ff) -
	    0x3ff);
	} else {	// if x < 2^53
	  tmp1.d = (double) C1_lo;	// exact conversion
	  x_nr_bits =
	    ((((unsigned int) (tmp1.ui64 >> 52)) & 0x7ff) - 0x3ff);
	}
      } else {	// C1_hi != 0 => nr. bits = 64 + nr_bits (C1_hi)
	tmp1.d = (double) C1_hi;	// exact conversion
	x_nr_bits =
	  64 + ((((unsigned int) (tmp1.ui64 >> 52)) & 0x7ff) - 0x3ff);
      }
      q1 = bid_nr_digits[x_nr_bits].digits;
      if (q1 == 0) {
	q1 = bid_nr_digits[x_nr_bits].digits1;
	if (C1_hi > bid_nr_digits[x_nr_bits].threshold_hi ||
	    (C1_hi == bid_nr_digits[x_nr_bits].threshold_hi &&
	     C1_lo >= bid_nr_digits[x_nr_bits].threshold_lo))
	  q1++;
      }
      // return (C1 * 10^scale) * 10^(x_exp - scale)
      // where scale = min (P34-q1, x_exp-y_exp)  
      scale = P34 - q1;
      ind = (x_exp - y_exp) >> 49;
      if (ind < scale)
	scale = ind;
      if (scale == 0) {
	res.w[1] = x.w[1];
	res.w[0] = x.w[0];
      } else if (q1 <= 19) {	// x fits in 64 bits  
	if (scale <= 19) {	// 10^scale fits in 64 bits
	  // 64 x 64 C1_lo * bid_ten2k64[scale] 
	  __mul_64x64_to_128MACH (res, C1_lo, bid_ten2k64[scale]);
	} else {	// 10^scale fits in 128 bits
	  // 64 x 128 C1_lo * bid_ten2k128[scale - 20]
	  __mul_128x64_to_128 (res, C1_lo, bid_ten2k128[scale - 20]);
	}
      } else {	// x fits in 128 bits, but 10^scale must fit in 64 bits
	// 64 x 128 bid_ten2k64[scale] * C1
	C1.w[1] = C1_hi;
	C1.w[0] = C1_lo;
	__mul_128x64_to_128 (res, bid_ten2k64[scale], C1);
      }
      // subtract scale from the exponent
      x_exp = x_exp - ((BID_UINT64) scale << 49);
      res.w[1] = res.w[1] | x_sign | x_exp;
    }
    BID_SWAP128 (res);
    BID_RETURN (res);
  } else {	// x and y are not canonical, not special, and are not zero
    // note that the result may still be zero, and then it has to have the
    // preferred exponent
    if (x_exp < y_exp) {	// if exp_x < exp_y then swap x and y 
      tmp_sign = x_sign;
      tmp_exp = x_exp;
      tmp_signif_hi = C1_hi;
      tmp_signif_lo = C1_lo;
      x_sign = y_sign;
      x_exp = y_exp;
      C1_hi = C2_hi;
      C1_lo = C2_lo;
      y_sign = tmp_sign;
      y_exp = tmp_exp;
      C2_hi = tmp_signif_hi;
      C2_lo = tmp_signif_lo;
    }
    // q1 = nr. of decimal digits in x
    //  determine first the nr. of bits in x
    if (C1_hi == 0) {	// x_bits is the nr. of bits in C1_lo
      if (C1_lo >= 0x0020000000000000ull) {	// x >= 2^53
	//split the 64-bit value in two 32-bit halves to avoid rounding errors
        tmp1.d = (double) (C1_lo >> 32);	// exact conversion
        x_nr_bits =
          32 + ((((unsigned int) (tmp1.ui64 >> 52)) & 0x7ff) - 0x3ff);
      } else {	// if x < 2^53
	tmp1.d = (double) C1_lo;	// exact conversion
	x_nr_bits =
	  ((((unsigned int) (tmp1.ui64 >> 52)) & 0x7ff) - 0x3ff);
      }
    } else {	// C1_hi != 0 => nr. bits = 64 + nr_bits (C1_hi)
      tmp1.d = (double) C1_hi;	// exact conversion
      x_nr_bits =
	64 + ((((unsigned int) (tmp1.ui64 >> 52)) & 0x7ff) - 0x3ff);
    }

    q1 = bid_nr_digits[x_nr_bits].digits;
    if (q1 == 0) {
      q1 = bid_nr_digits[x_nr_bits].digits1;
      if (C1_hi > bid_nr_digits[x_nr_bits].threshold_hi ||
	  (C1_hi == bid_nr_digits[x_nr_bits].threshold_hi &&
	   C1_lo >= bid_nr_digits[x_nr_bits].threshold_lo))
	q1++;
    }
    // q2 = nr. of decimal digits in y
    //  determine first the nr. of bits in y (y_nr_bits)
    if (C2_hi == 0) {	// y_bits is the nr. of bits in C2_lo
      if (C2_lo >= 0x0020000000000000ull) {	// y >= 2^53
	//split the 64-bit value in two 32-bit halves to avoid rounding errors
        tmp2.d = (double) (C2_lo >> 32);	// exact conversion
        y_nr_bits =
          32 + ((((unsigned int) (tmp2.ui64 >> 52)) & 0x7ff) - 0x3ff);
      } else {	// if y < 2^53
	tmp2.d = (double) C2_lo;	// exact conversion
	y_nr_bits =
	  ((((unsigned int) (tmp2.ui64 >> 52)) & 0x7ff) - 0x3ff);
      }
    } else {	// C2_hi != 0 => nr. bits = 64 + nr_bits (C2_hi)
      tmp2.d = (double) C2_hi;	// exact conversion
      y_nr_bits =
	64 + ((((unsigned int) (tmp2.ui64 >> 52)) & 0x7ff) - 0x3ff);
    }

    q2 = bid_nr_digits[y_nr_bits].digits;
    if (q2 == 0) {
      q2 = bid_nr_digits[y_nr_bits].digits1;
      if (C2_hi > bid_nr_digits[y_nr_bits].threshold_hi ||
	  (C2_hi == bid_nr_digits[y_nr_bits].threshold_hi &&
	   C2_lo >= bid_nr_digits[y_nr_bits].threshold_lo))
	q2++;
    }

    delta = q1 + (int) (x_exp >> 49) - q2 - (int) (y_exp >> 49);

    if (delta >= P34) {
      // round the result directly because 0 < C2 < ulp (C1 * 10^(x_exp-e2))
      // n = C1 * 10^e1 or n = C1 +/- 10^(q1-P34)) * 10^e1
      // the result is inexact; the preferred exponent is the least possible

      if (delta >= P34 + 1) {
	// for RN the result is the operand with the larger magnitude,
	// possibly scaled up by 10^(P34-q1)
	// an overflow cannot occur in this case (rounding to nearest)
	if (q1 < P34) {	// scale C1 up by 10^(P34-q1)
	  // Note: because delta >= P34+1 it is certain that 
	  //     x_exp - ((BID_UINT64)scale << 49) will stay above e_min
	  scale = P34 - q1;
	  if (q1 <= 19) {	// C1 fits in 64 bits
	    // 1 <= q1 <= 19 => 15 <= scale <= 33
	    if (scale <= 19) {	// 10^scale fits in 64 bits
	      __mul_64x64_to_128MACH (C1, bid_ten2k64[scale], C1_lo);
	    } else {	// if 20 <= scale <= 33
	      // C1 * 10^scale = (C1 * 10^(scale-19)) * 10^19 where
	      // (C1 * 10^(scale-19)) fits in 64 bits
	      C1_lo = C1_lo * bid_ten2k64[scale - 19];
	      __mul_64x64_to_128MACH (C1, bid_ten2k64[19], C1_lo);
	    }
	  } else {	//if 20 <= q1 <= 33=P34-1 then C1 fits only in 128 bits
	    // => 1 <= P34 - q1 <= 14 so 10^(P34-q1) fits in 64 bits
	    C1.w[1] = C1_hi;
	    C1.w[0] = C1_lo;
	    // C1 = bid_ten2k64[P34 - q1] * C1
	    __mul_128x64_to_128 (C1, bid_ten2k64[P34 - q1], C1);
	  }
	  x_exp = x_exp - ((BID_UINT64) scale << 49);
	  C1_hi = C1.w[1];
	  C1_lo = C1.w[0];
	}
	// some special cases arise: if delta = P34 + 1 and C1 = 10^(P34-1) 
	// (after scaling) and x_sign != y_sign and C2 > 5*10^(q2-1) => 
	// subtract 1 ulp
	// Note: do this only for rounding to nearest; for other rounding 
	// modes the correction will be applied next
	if ((rnd_mode == BID_ROUNDING_TO_NEAREST || 
            rnd_mode == BID_ROUNDING_TIES_AWAY) && delta == (P34 + 1) && 
            C1_hi == 0x0000314dc6448d93ull && C1_lo == 0x38c15b0a00000000ull &&
            x_sign != y_sign && ((q2 <= 19 && C2_lo > bid_midpoint64[q2 - 1]) ||
            (q2 >= 20 && (C2_hi > bid_midpoint128 [q2 - 20].w[1] || 
            (C2_hi == bid_midpoint128 [q2 - 20].w[1] && 
             C2_lo > bid_midpoint128 [q2 - 20].w[0]))))) {
	  // C1 = 10^34 - 1 and decrement x_exp by 1 (no underflow possible)
	  C1_hi = 0x0001ed09bead87c0ull;
	  C1_lo = 0x378d8e63ffffffffull;
	  x_exp = x_exp - EXP_P1;
	}
	if (rnd_mode != BID_ROUNDING_TO_NEAREST) {
	  if ((rnd_mode == BID_ROUNDING_DOWN && x_sign && y_sign) ||
	      (rnd_mode == BID_ROUNDING_UP && !x_sign && !y_sign)) {
	    // add 1 ulp and then check for overflow
	    C1_lo = C1_lo + 1;
	    if (C1_lo == 0) {	// rounding overflow in the low 64 bits
	      C1_hi = C1_hi + 1;
	    }
	    if (C1_hi == 0x0001ed09bead87c0ull
		&& C1_lo == 0x378d8e6400000000ull) {
	      // C1 = 10^34 => rounding overflow
	      C1_hi = 0x0000314dc6448d93ull;
	      C1_lo = 0x38c15b0a00000000ull;	// 10^33
	      x_exp = x_exp + EXP_P1;
	      if (x_exp == EXP_MAX_P1) {	// overflow
		C1_hi = 0x7800000000000000ull;	// +inf
		C1_lo = 0x0ull;
		x_exp = 0;	// x_sign is preserved
		// set overflow flag (the inexact flag was set too)
		*pfpsf |= BID_OVERFLOW_EXCEPTION;
	      }
	    }
	  } else if ((rnd_mode == BID_ROUNDING_DOWN && !x_sign && y_sign) ||
		     (rnd_mode == BID_ROUNDING_UP && x_sign && !y_sign) ||
		     (rnd_mode == BID_ROUNDING_TO_ZERO
		      && x_sign != y_sign)) {
	    // subtract 1 ulp from C1
	    // Note: because delta >= P34 + 1 the result cannot be zero
	    C1_lo = C1_lo - 1;
	    if (C1_lo == 0xffffffffffffffffull)
	      C1_hi = C1_hi - 1;
	    // if the coefficient is 10^33 - 1 then make it 10^34 - 1 and 
	    // decrease the exponent by 1 (because delta >= P34 + 1 the
	    // exponent will not become less than e_min)
	    // 10^33 - 1 = 0x0000314dc6448d9338c15b09ffffffff
	    // 10^34 - 1 = 0x0001ed09bead87c0378d8e63ffffffff
	    if (C1_hi == 0x0000314dc6448d93ull
		&& C1_lo == 0x38c15b09ffffffffull) {
	      // make C1 = 10^34  - 1
	      C1_hi = 0x0001ed09bead87c0ull;
	      C1_lo = 0x378d8e63ffffffffull;
	      x_exp = x_exp - EXP_P1;
	    }
	  } else {
	    ;	// the result is already correct
	  }
	}
	// set the inexact flag
	*pfpsf |= BID_INEXACT_EXCEPTION;
	// assemble the result
	res.w[1] = x_sign | x_exp | C1_hi;
	res.w[0] = C1_lo;
      } else {	// delta = P34 
	// in most cases, the smaller operand may be < or = or > 1/2 ulp of the
	// larger operand
	// however, the case C1 = 10^(q1-1) and x_sign != y_sign is special due
	// to accuracy loss after subtraction, and will be treated separately
	if (x_sign == y_sign || (q1 <= 20
				 && (C1_hi != 0
				     || C1_lo != bid_ten2k64[q1 - 1]))
	    || (q1 >= 21 && (C1_hi != bid_ten2k128[q1 - 21].w[1]
			     || C1_lo != bid_ten2k128[q1 - 21].w[0]))) {
	  // if x_sign == y_sign or C1 != 10^(q1-1)
	  // compare C2 with 1/2 ulp = 5 * 10^(q2-1), the latter read from table
	  // Note: cases q1<=19 and q1>=20 can be coalesced at some latency cost
	  if (q2 <= 19) {	// C2 and 5*10^(q2-1) both fit in 64 bits
	    halfulp64 = bid_midpoint64[q2 - 1];	// 5 * 10^(q2-1)
	    if (C2_lo < halfulp64) {	// n2 < 1/2 ulp (n1)
	      // for RN the result is the operand with the larger magnitude, 
	      // possibly scaled up by 10^(P34-q1)
	      // an overflow cannot occur in this case (rounding to nearest)
	      if (q1 < P34) {	// scale C1 up by 10^(P34-q1)
		// Note: because delta = P34 it is certain that
		//     x_exp - ((BID_UINT64)scale << 49) will stay above e_min
		scale = P34 - q1;
		if (q1 <= 19) {	// C1 fits in 64 bits
		  // 1 <= q1 <= 19 => 15 <= scale <= 33
		  if (scale <= 19) {	// 10^scale fits in 64 bits
		    __mul_64x64_to_128MACH (C1, bid_ten2k64[scale], C1_lo);
		  } else {	// if 20 <= scale <= 33
		    // C1 * 10^scale = (C1 * 10^(scale-19)) * 10^19 where
		    // (C1 * 10^(scale-19)) fits in 64 bits
		    C1_lo = C1_lo * bid_ten2k64[scale - 19];
		    __mul_64x64_to_128MACH (C1, bid_ten2k64[19], C1_lo);
		  }
		} else { //if 20 <= q1 <= 33=P34-1 then C1 fits only in 128 bits
		  // => 1 <= P34 - q1 <= 14 so 10^(P34-q1) fits in 64 bits
		  C1.w[1] = C1_hi;
		  C1.w[0] = C1_lo;
		  // C1 = bid_ten2k64[P34 - q1] * C1
		  __mul_128x64_to_128 (C1, bid_ten2k64[P34 - q1], C1);
		}
		x_exp = x_exp - ((BID_UINT64) scale << 49);
		C1_hi = C1.w[1];
		C1_lo = C1.w[0];
	      }
	      if (rnd_mode != BID_ROUNDING_TO_NEAREST) {
		if ((rnd_mode == BID_ROUNDING_DOWN && x_sign && y_sign) ||
		    (rnd_mode == BID_ROUNDING_UP && !x_sign && !y_sign)) {
		  // add 1 ulp and then check for overflow
		  C1_lo = C1_lo + 1;
		  if (C1_lo == 0) {	// rounding overflow in the low 64 bits
		    C1_hi = C1_hi + 1;
		  }
		  if (C1_hi == 0x0001ed09bead87c0ull
		      && C1_lo == 0x378d8e6400000000ull) {
		    // C1 = 10^34 => rounding overflow
		    C1_hi = 0x0000314dc6448d93ull;
		    C1_lo = 0x38c15b0a00000000ull;	// 10^33
		    x_exp = x_exp + EXP_P1;
		    if (x_exp == EXP_MAX_P1) {	// overflow
		      C1_hi = 0x7800000000000000ull;	// +inf
		      C1_lo = 0x0ull;
		      x_exp = 0;	// x_sign is preserved
		      // set overflow flag (the inexact flag was set too)
		      *pfpsf |= BID_OVERFLOW_EXCEPTION;
		    }
		  }
		} else
		  if ((rnd_mode == BID_ROUNDING_DOWN && !x_sign && y_sign)
		      || (rnd_mode == BID_ROUNDING_UP && x_sign && !y_sign)
		      || (rnd_mode == BID_ROUNDING_TO_ZERO
			  && x_sign != y_sign)) {
		  // subtract 1 ulp from C1
		  // Note: because delta >= P34 + 1 the result cannot be zero
		  C1_lo = C1_lo - 1;
		  if (C1_lo == 0xffffffffffffffffull)
		    C1_hi = C1_hi - 1;
		  // if the coefficient is 10^33-1 then make it 10^34-1 and 
		  // decrease the exponent by 1 (because delta >= P34 + 1 the
		  // exponent will not become less than e_min)
		  // 10^33 - 1 = 0x0000314dc6448d9338c15b09ffffffff
		  // 10^34 - 1 = 0x0001ed09bead87c0378d8e63ffffffff
		  if (C1_hi == 0x0000314dc6448d93ull
		      && C1_lo == 0x38c15b09ffffffffull) {
		    // make C1 = 10^34  - 1
		    C1_hi = 0x0001ed09bead87c0ull;
		    C1_lo = 0x378d8e63ffffffffull;
		    x_exp = x_exp - EXP_P1;
		  }
		} else {
		  ;	// the result is already correct
		}
	      }
	      // set the inexact flag
	      *pfpsf |= BID_INEXACT_EXCEPTION;
	      // assemble the result
	      res.w[1] = x_sign | x_exp | C1_hi;
	      res.w[0] = C1_lo;
	    } else if ((C2_lo == halfulp64)
		       && (q1 < P34 || ((C1_lo & 0x1) == 0))) {
	      // n2 = 1/2 ulp (n1) and q1 < P34 or C1 is even
	      // the result is the operand with the larger magnitude,
	      // possibly scaled up by 10^(P34-q1)
	      // an overflow cannot occur in this case (rounding to nearest)
	      if (q1 < P34) {	// scale C1 up by 10^(P34-q1)
		// Note: because delta = P34 it is certain that
		//     x_exp - ((BID_UINT64)scale << 49) will stay above e_min
		scale = P34 - q1;
		if (q1 <= 19) {	// C1 fits in 64 bits
		  // 1 <= q1 <= 19 => 15 <= scale <= 33
		  if (scale <= 19) {	// 10^scale fits in 64 bits
		    __mul_64x64_to_128MACH (C1, bid_ten2k64[scale], C1_lo);
		  } else {	// if 20 <= scale <= 33 
		    // C1 * 10^scale = (C1 * 10^(scale-19)) * 10^19 where
		    // (C1 * 10^(scale-19)) fits in 64 bits  
		    C1_lo = C1_lo * bid_ten2k64[scale - 19];
		    __mul_64x64_to_128MACH (C1, bid_ten2k64[19], C1_lo);
		  }
		} else {	//if 20 <= q1 <= 33=P34-1 then C1 fits only in 128 bits
		  // => 1 <= P34 - q1 <= 14 so 10^(P34-q1) fits in 64 bits 
		  C1.w[1] = C1_hi;
		  C1.w[0] = C1_lo;
		  // C1 = bid_ten2k64[P34 - q1] * C1 
		  __mul_128x64_to_128 (C1, bid_ten2k64[P34 - q1], C1);
		}
		x_exp = x_exp - ((BID_UINT64) scale << 49);
		C1_hi = C1.w[1];
		C1_lo = C1.w[0];
	      }
	      if ((rnd_mode == BID_ROUNDING_TO_NEAREST && x_sign == y_sign
		   && (C1_lo & 0x01)) || (rnd_mode == BID_ROUNDING_TIES_AWAY
					  && x_sign == y_sign)
		  || (rnd_mode == BID_ROUNDING_UP && !x_sign && !y_sign)
		  || (rnd_mode == BID_ROUNDING_DOWN && x_sign && y_sign)) {
		// add 1 ulp and then check for overflow
		C1_lo = C1_lo + 1;
		if (C1_lo == 0) {	// rounding overflow in the low 64 bits
		  C1_hi = C1_hi + 1;
		}
		if (C1_hi == 0x0001ed09bead87c0ull
		    && C1_lo == 0x378d8e6400000000ull) {
		  // C1 = 10^34 => rounding overflow
		  C1_hi = 0x0000314dc6448d93ull;
		  C1_lo = 0x38c15b0a00000000ull;	// 10^33
		  x_exp = x_exp + EXP_P1;
		  if (x_exp == EXP_MAX_P1) {	// overflow
		    C1_hi = 0x7800000000000000ull;	// +inf
		    C1_lo = 0x0ull;
		    x_exp = 0;	// x_sign is preserved
		    // set overflow flag (the inexact flag was set too)
		    *pfpsf |= BID_OVERFLOW_EXCEPTION;
		  }
		}
	      } else
		if ((rnd_mode == BID_ROUNDING_TO_NEAREST && x_sign != y_sign &&
                    (C1_lo & 0x01)) || 
                    (rnd_mode == BID_ROUNDING_DOWN && !x_sign && y_sign) || 
                    (rnd_mode == BID_ROUNDING_UP && x_sign && !y_sign) || 
                    (rnd_mode == BID_ROUNDING_TO_ZERO && x_sign != y_sign)) {
		// subtract 1 ulp from C1
		// Note: because delta >= P34 + 1 the result cannot be zero
		C1_lo = C1_lo - 1;
		if (C1_lo == 0xffffffffffffffffull)
		  C1_hi = C1_hi - 1;
		// if the coefficient is 10^33 - 1 then make it 10^34 - 1
		// and decrease the exponent by 1 (because delta >= P34 + 1
		// the exponent will not become less than e_min)
		// 10^33 - 1 = 0x0000314dc6448d9338c15b09ffffffff
		// 10^34 - 1 = 0x0001ed09bead87c0378d8e63ffffffff
		if (C1_hi == 0x0000314dc6448d93ull
		    && C1_lo == 0x38c15b09ffffffffull) {
		  // make C1 = 10^34  - 1
		  C1_hi = 0x0001ed09bead87c0ull;
		  C1_lo = 0x378d8e63ffffffffull;
		  x_exp = x_exp - EXP_P1;
		}
	      } else {
		;	// the result is already correct
	      }
	      // set the inexact flag
	      *pfpsf |= BID_INEXACT_EXCEPTION;
	      // assemble the result 
	      res.w[1] = x_sign | x_exp | C1_hi;
	      res.w[0] = C1_lo;
	    } else {	// if C2_lo > halfulp64 || 
	      // (C2_lo == halfulp64 && q1 == P34 && ((C1_lo & 0x1) == 1)), i.e.
	      // 1/2 ulp(n1) < n2 < 1 ulp(n1) or n2 = 1/2 ulp(n1) and C1 odd
	      // res = x+1 ulp if n1*n2 > 0 and res = x-1 ulp if n1*n2 < 0
	      if (q1 < P34) {	// then 1 ulp = 10^(e1+q1-P34) < 10^e1
		// Note: if (q1 == P34) then 1 ulp = 10^(e1+q1-P34) = 10^e1
		// because q1 < P34 we must first replace C1 by 
		// C1 * 10^(P34-q1), and must decrease the exponent by 
		// (P34-q1) (it will still be at least e_min)
		scale = P34 - q1;
		if (q1 <= 19) {	// C1 fits in 64 bits
		  // 1 <= q1 <= 19 => 15 <= scale <= 33
		  if (scale <= 19) {	// 10^scale fits in 64 bits
		    __mul_64x64_to_128MACH (C1, bid_ten2k64[scale], C1_lo);
		  } else {	// if 20 <= scale <= 33
		    // C1 * 10^scale = (C1 * 10^(scale-19)) * 10^19 where
		    // (C1 * 10^(scale-19)) fits in 64 bits
		    C1_lo = C1_lo * bid_ten2k64[scale - 19];
		    __mul_64x64_to_128MACH (C1, bid_ten2k64[19], C1_lo);
		  }
		} else {	//if 20 <= q1 <= 33=P34-1 then C1 fits only in 128 bits
		  // => 1 <= P34 - q1 <= 14 so 10^(P34-q1) fits in 64 bits
		  C1.w[1] = C1_hi;
		  C1.w[0] = C1_lo;
		  // C1 = bid_ten2k64[P34 - q1] * C1
		  __mul_128x64_to_128 (C1, bid_ten2k64[P34 - q1], C1);
		}
		x_exp = x_exp - ((BID_UINT64) scale << 49);
		C1_hi = C1.w[1];
		C1_lo = C1.w[0];
		// check for rounding overflow
		if (C1_hi == 0x0001ed09bead87c0ull
		    && C1_lo == 0x378d8e6400000000ull) {
		  // C1 = 10^34 => rounding overflow 
		  C1_hi = 0x0000314dc6448d93ull;
		  C1_lo = 0x38c15b0a00000000ull;	// 10^33
		  x_exp = x_exp + EXP_P1;
		}
	      }
	      if ((rnd_mode == BID_ROUNDING_TO_NEAREST && x_sign != y_sign)
		  || (rnd_mode == BID_ROUNDING_TIES_AWAY && x_sign != y_sign
		      && C2_lo != halfulp64)
		  || (rnd_mode == BID_ROUNDING_DOWN && !x_sign && y_sign)
		  || (rnd_mode == BID_ROUNDING_UP && x_sign && !y_sign)
		  || (rnd_mode == BID_ROUNDING_TO_ZERO
		      && x_sign != y_sign)) {
		// the result is x - 1
		// for RN n1 * n2 < 0; underflow not possible
		C1_lo = C1_lo - 1;
		if (C1_lo == 0xffffffffffffffffull)
		  C1_hi--;
		// check if we crossed into the lower decade
		if (C1_hi == 0x0000314dc6448d93ull && C1_lo == 0x38c15b09ffffffffull) {	// 10^33 - 1
		  C1_hi = 0x0001ed09bead87c0ull;	// 10^34 - 1
		  C1_lo = 0x378d8e63ffffffffull;
		  x_exp = x_exp - EXP_P1;	// no underflow, because n1 >> n2
		}
	      } else
		if ((rnd_mode == BID_ROUNDING_TO_NEAREST
		     && x_sign == y_sign)
		    || (rnd_mode == BID_ROUNDING_TIES_AWAY
			&& x_sign == y_sign)
		    || (rnd_mode == BID_ROUNDING_DOWN && x_sign && y_sign)
		    || (rnd_mode == BID_ROUNDING_UP && !x_sign
			&& !y_sign)) {
		// the result is x + 1
		// for RN x_sign = y_sign, i.e. n1*n2 > 0
		C1_lo = C1_lo + 1;
		if (C1_lo == 0) {	// rounding overflow in the low 64 bits
		  C1_hi = C1_hi + 1;
		}
		if (C1_hi == 0x0001ed09bead87c0ull
		    && C1_lo == 0x378d8e6400000000ull) {
		  // C1 = 10^34 => rounding overflow
		  C1_hi = 0x0000314dc6448d93ull;
		  C1_lo = 0x38c15b0a00000000ull;	// 10^33
		  x_exp = x_exp + EXP_P1;
		  if (x_exp == EXP_MAX_P1) {	// overflow
		    C1_hi = 0x7800000000000000ull;	// +inf
		    C1_lo = 0x0ull;
		    x_exp = 0;	// x_sign is preserved
		    // set the overflow flag
		    *pfpsf |= BID_OVERFLOW_EXCEPTION;
		  }
		}
	      } else {
		;	// the result is x
	      }
	      // set the inexact flag
	      *pfpsf |= BID_INEXACT_EXCEPTION;
	      // assemble the result
	      res.w[1] = x_sign | x_exp | C1_hi;
	      res.w[0] = C1_lo;
	    }
	  } else {	// if q2 >= 20 then 5*10^(q2-1) and C2 (the latter in 
	    // most cases) fit only in more than 64 bits
	    halfulp128 = bid_midpoint128[q2 - 20];	// 5 * 10^(q2-1)
	    if ((C2_hi < halfulp128.w[1])
		|| (C2_hi == halfulp128.w[1]
		    && C2_lo < halfulp128.w[0])) {
	      // n2 < 1/2 ulp (n1)
	      // the result is the operand with the larger magnitude,
	      // possibly scaled up by 10^(P34-q1)
	      // an overflow cannot occur in this case (rounding to nearest)
	      if (q1 < P34) {	// scale C1 up by 10^(P34-q1)
		// Note: because delta = P34 it is certain that
		//     x_exp - ((BID_UINT64)scale << 49) will stay above e_min
		scale = P34 - q1;
		if (q1 <= 19) {	// C1 fits in 64 bits
		  // 1 <= q1 <= 19 => 15 <= scale <= 33
		  if (scale <= 19) {	// 10^scale fits in 64 bits
		    __mul_64x64_to_128MACH (C1, bid_ten2k64[scale], C1_lo);
		  } else {	// if 20 <= scale <= 33 
		    // C1 * 10^scale = (C1 * 10^(scale-19)) * 10^19 where
		    // (C1 * 10^(scale-19)) fits in 64 bits  
		    C1_lo = C1_lo * bid_ten2k64[scale - 19];
		    __mul_64x64_to_128MACH (C1, bid_ten2k64[19], C1_lo);
		  }
		} else {	//if 20 <= q1 <= 33=P34-1 then C1 fits only in 128 bits
		  // => 1 <= P34 - q1 <= 14 so 10^(P34-q1) fits in 64 bits 
		  C1.w[1] = C1_hi;
		  C1.w[0] = C1_lo;
		  // C1 = bid_ten2k64[P34 - q1] * C1 
		  __mul_128x64_to_128 (C1, bid_ten2k64[P34 - q1], C1);
		}
		C1_hi = C1.w[1];
		C1_lo = C1.w[0];
		x_exp = x_exp - ((BID_UINT64) scale << 49);
	      }
	      if (rnd_mode != BID_ROUNDING_TO_NEAREST) {
		if ((rnd_mode == BID_ROUNDING_DOWN && x_sign && y_sign) ||
		    (rnd_mode == BID_ROUNDING_UP && !x_sign && !y_sign)) {
		  // add 1 ulp and then check for overflow
		  C1_lo = C1_lo + 1;
		  if (C1_lo == 0) {	// rounding overflow in the low 64 bits
		    C1_hi = C1_hi + 1;
		  }
		  if (C1_hi == 0x0001ed09bead87c0ull
		      && C1_lo == 0x378d8e6400000000ull) {
		    // C1 = 10^34 => rounding overflow
		    C1_hi = 0x0000314dc6448d93ull;
		    C1_lo = 0x38c15b0a00000000ull;	// 10^33
		    x_exp = x_exp + EXP_P1;
		    if (x_exp == EXP_MAX_P1) {	// overflow
		      C1_hi = 0x7800000000000000ull;	// +inf
		      C1_lo = 0x0ull;
		      x_exp = 0;	// x_sign is preserved
		      // set overflow flag (the inexact flag was set too)
		      *pfpsf |= BID_OVERFLOW_EXCEPTION;
		    }
		  }
		} else
		  if ((rnd_mode == BID_ROUNDING_DOWN && !x_sign && y_sign)
		      || (rnd_mode == BID_ROUNDING_UP && x_sign && !y_sign)
		      || (rnd_mode == BID_ROUNDING_TO_ZERO
			  && x_sign != y_sign)) {
		  // subtract 1 ulp from C1
		  // Note: because delta >= P34 + 1 the result cannot be zero
		  C1_lo = C1_lo - 1;
		  if (C1_lo == 0xffffffffffffffffull)
		    C1_hi = C1_hi - 1;
		  // if the coefficient is 10^33-1 then make it 10^34-1 and
		  // decrease the exponent by 1 (because delta >= P34 + 1 the
		  // exponent will not become less than e_min)
		  // 10^33 - 1 = 0x0000314dc6448d9338c15b09ffffffff
		  // 10^34 - 1 = 0x0001ed09bead87c0378d8e63ffffffff
		  if (C1_hi == 0x0000314dc6448d93ull
		      && C1_lo == 0x38c15b09ffffffffull) {
		    // make C1 = 10^34  - 1
		    C1_hi = 0x0001ed09bead87c0ull;
		    C1_lo = 0x378d8e63ffffffffull;
		    x_exp = x_exp - EXP_P1;
		  }
		} else {
		  ;	// the result is already correct
		}
	      }
	      // set the inexact flag 
	      *pfpsf |= BID_INEXACT_EXCEPTION;
	      // assemble the result 
	      res.w[1] = x_sign | x_exp | C1_hi;
	      res.w[0] = C1_lo;
	    } else if ((C2_hi == halfulp128.w[1]
			&& C2_lo == halfulp128.w[0])
		       && (q1 < P34 || ((C1_lo & 0x1) == 0))) {
	      // set the inexact flag 
	      // midpoint & lsb in C1 is 0
	      // n2 = 1/2 ulp (n1) and C1 is even
	      // the result is the operand with the larger magnitude,
	      // possibly scaled up by 10^(P34-q1)
	      // an overflow cannot occur in this case (rounding to nearest)
	      if (q1 < P34) {	// scale C1 up by 10^(P34-q1)
		// Note: because delta = P34 it is certain that
		//     x_exp - ((BID_UINT64)scale << 49) will stay above e_min
		scale = P34 - q1;
		if (q1 <= 19) {	// C1 fits in 64 bits
		  // 1 <= q1 <= 19 => 15 <= scale <= 33
		  if (scale <= 19) {	// 10^scale fits in 64 bits
		    __mul_64x64_to_128MACH (C1, bid_ten2k64[scale], C1_lo);
		  } else {	// if 20 <= scale <= 33
		    // C1 * 10^scale = (C1 * 10^(scale-19)) * 10^19 where
		    // (C1 * 10^(scale-19)) fits in 64 bits
		    C1_lo = C1_lo * bid_ten2k64[scale - 19];
		    __mul_64x64_to_128MACH (C1, bid_ten2k64[19], C1_lo);
		  }
		} else {	//if 20 <= q1 <= 33=P34-1 then C1 fits only in 128 bits
		  // => 1 <= P34 - q1 <= 14 so 10^(P34-q1) fits in 64 bits
		  C1.w[1] = C1_hi;
		  C1.w[0] = C1_lo;
		  // C1 = bid_ten2k64[P34 - q1] * C1
		  __mul_128x64_to_128 (C1, bid_ten2k64[P34 - q1], C1);
		}
		x_exp = x_exp - ((BID_UINT64) scale << 49);
		C1_hi = C1.w[1];
		C1_lo = C1.w[0];
	      }
	      if (rnd_mode != BID_ROUNDING_TO_NEAREST) {
		if ((rnd_mode == BID_ROUNDING_TIES_AWAY && x_sign == y_sign) ||
		    (rnd_mode == BID_ROUNDING_UP && !x_sign && !y_sign) ||
		    (rnd_mode == BID_ROUNDING_DOWN && x_sign && y_sign)) {
		  // add 1 ulp and then check for overflow
		  C1_lo = C1_lo + 1;
		  if (C1_lo == 0) {	// rounding overflow in the low 64 bits
		    C1_hi = C1_hi + 1;
		  }
		  if (C1_hi == 0x0001ed09bead87c0ull
		      && C1_lo == 0x378d8e6400000000ull) {
		    // C1 = 10^34 => rounding overflow
		    C1_hi = 0x0000314dc6448d93ull;
		    C1_lo = 0x38c15b0a00000000ull;	// 10^33
		    x_exp = x_exp + EXP_P1;
		    if (x_exp == EXP_MAX_P1) {	// overflow
		      C1_hi = 0x7800000000000000ull;	// +inf
		      C1_lo = 0x0ull;
		      x_exp = 0;	// x_sign is preserved
		      // set overflow flag (the inexact flag was set too)
		      *pfpsf |= BID_OVERFLOW_EXCEPTION;
		    }
		  }
		} else if ((rnd_mode == BID_ROUNDING_DOWN && !x_sign && y_sign)
                    || (rnd_mode == BID_ROUNDING_UP && x_sign && !y_sign)
                    || (rnd_mode == BID_ROUNDING_TO_ZERO && x_sign != y_sign)) {
		  // subtract 1 ulp from C1
		  // Note: because delta >= P34 + 1 the result cannot be zero
		  C1_lo = C1_lo - 1;
		  if (C1_lo == 0xffffffffffffffffull)
		    C1_hi = C1_hi - 1;
		  // if the coefficient is 10^33 - 1 then make it 10^34 - 1
		  // and decrease the exponent by 1 (because delta >= P34 + 1
		  // the exponent will not become less than e_min)
		  // 10^33 - 1 = 0x0000314dc6448d9338c15b09ffffffff
		  // 10^34 - 1 = 0x0001ed09bead87c0378d8e63ffffffff
		  if (C1_hi == 0x0000314dc6448d93ull
		      && C1_lo == 0x38c15b09ffffffffull) {
		    // make C1 = 10^34  - 1
		    C1_hi = 0x0001ed09bead87c0ull;
		    C1_lo = 0x378d8e63ffffffffull;
		    x_exp = x_exp - EXP_P1;
		  }
		} else {
		  ;	// the result is already correct
		}
	      }
	      // set the inexact flag
	      *pfpsf |= BID_INEXACT_EXCEPTION;
	      // assemble the result
	      res.w[1] = x_sign | x_exp | C1_hi;
	      res.w[0] = C1_lo;
	    } else {	// if C2 > halfulp128 ||
	      // (C2 == halfulp128 && q1 == P34 && ((C1 & 0x1) == 1)), i.e.
	      // 1/2 ulp(n1) < n2 < 1 ulp(n1) or n2 = 1/2 ulp(n1) and C1 odd
	      // res = x+1 ulp if n1*n2 > 0 and res = x-1 ulp if n1*n2 < 0
	      if (q1 < P34) {	// then 1 ulp = 10^(e1+q1-P34) < 10^e1
		// Note: if (q1 == P34) then 1 ulp = 10^(e1+q1-P34) = 10^e1
		// because q1 < P34 we must first replace C1 by C1*10^(P34-q1),
		// and must decrease the exponent by (P34-q1) (it will still be
		// at least e_min)
		scale = P34 - q1;
		if (q1 <= 19) {	// C1 fits in 64 bits
		  // 1 <= q1 <= 19 => 15 <= scale <= 33
		  if (scale <= 19) {	// 10^scale fits in 64 bits
		    __mul_64x64_to_128MACH (C1, bid_ten2k64[scale], C1_lo);
		  } else {	// if 20 <= scale <= 33
		    // C1 * 10^scale = (C1 * 10^(scale-19)) * 10^19 where
		    // (C1 * 10^(scale-19)) fits in 64 bits
		    C1_lo = C1_lo * bid_ten2k64[scale - 19];
		    __mul_64x64_to_128MACH (C1, bid_ten2k64[19], C1_lo);
		  }
		} else {	//if 20 <= q1 <= 33=P34-1 then C1 fits only in 128 bits
		  // => 1 <= P34 - q1 <= 14 so 10^(P34-q1) fits in 64 bits
		  C1.w[1] = C1_hi;
		  C1.w[0] = C1_lo;
		  // C1 = bid_ten2k64[P34 - q1] * C1
		  __mul_128x64_to_128 (C1, bid_ten2k64[P34 - q1], C1);
		}
		C1_hi = C1.w[1];
		C1_lo = C1.w[0];
		x_exp = x_exp - ((BID_UINT64) scale << 49);
	      }
	      if ((rnd_mode == BID_ROUNDING_TO_NEAREST && x_sign != y_sign)
		  || (rnd_mode == BID_ROUNDING_TIES_AWAY && x_sign != y_sign
		      && (C2_hi != halfulp128.w[1]
			  || C2_lo != halfulp128.w[0]))
		  || (rnd_mode == BID_ROUNDING_DOWN && !x_sign && y_sign)
		  || (rnd_mode == BID_ROUNDING_UP && x_sign && !y_sign)
		  || (rnd_mode == BID_ROUNDING_TO_ZERO
		      && x_sign != y_sign)) {
		// the result is x - 1
		// for RN n1 * n2 < 0; underflow not possible
		C1_lo = C1_lo - 1;
		if (C1_lo == 0xffffffffffffffffull)
		  C1_hi--;
		// check if we crossed into the lower decade
		if (C1_hi == 0x0000314dc6448d93ull && C1_lo == 0x38c15b09ffffffffull) {	// 10^33 - 1
		  C1_hi = 0x0001ed09bead87c0ull;	// 10^34 - 1
		  C1_lo = 0x378d8e63ffffffffull;
		  x_exp = x_exp - EXP_P1;	// no underflow, because n1 >> n2
		}
	      } else
		if ((rnd_mode == BID_ROUNDING_TO_NEAREST
		     && x_sign == y_sign)
		    || (rnd_mode == BID_ROUNDING_TIES_AWAY
			&& x_sign == y_sign)
		    || (rnd_mode == BID_ROUNDING_DOWN && x_sign && y_sign)
		    || (rnd_mode == BID_ROUNDING_UP && !x_sign && !y_sign)) {
		// the result is x + 1
		// for RN x_sign = y_sign, i.e. n1*n2 > 0
		C1_lo = C1_lo + 1;
		if (C1_lo == 0) {	// rounding overflow in the low 64 bits
		  C1_hi = C1_hi + 1;
		}
		if (C1_hi == 0x0001ed09bead87c0ull
		    && C1_lo == 0x378d8e6400000000ull) {
		  // C1 = 10^34 => rounding overflow
		  C1_hi = 0x0000314dc6448d93ull;
		  C1_lo = 0x38c15b0a00000000ull;	// 10^33
		  x_exp = x_exp + EXP_P1;
		  if (x_exp == EXP_MAX_P1) {	// overflow
		    C1_hi = 0x7800000000000000ull;	// +inf
		    C1_lo = 0x0ull;
		    x_exp = 0;	// x_sign is preserved
		    // set the overflow flag
		    *pfpsf |= BID_OVERFLOW_EXCEPTION;
		  }
		}
	      } else {
		;	// the result is x
	      }
	      // set the inexact flag
	      *pfpsf |= BID_INEXACT_EXCEPTION;
	      // assemble the result
	      res.w[1] = x_sign | x_exp | C1_hi;
	      res.w[0] = C1_lo;
	    }
	  }	// end q1 >= 20
	  // end case where C1 != 10^(q1-1)
	} else {	// C1 = 10^(q1-1) and x_sign != y_sign
	  // instead of C' = (C1 * 10^(e1-e2) + C2)rnd,P34
	  // calculate C' = C1 * 10^(e1-e2-x1) + (C2 * 10^(-x1))rnd,P34 
	  // where x1 = q2 - 1, 0 <= x1 <= P34 - 1
	  // Because C1 = 10^(q1-1) and x_sign != y_sign, C' will have P34 
	  // digits and n = C' * 10^(e2+x1)
	  // If the result has P34+1 digits, redo the steps above with x1+1
	  // If the result has P34-1 digits or less, redo the steps above with 
	  // x1-1 but only if initially x1 >= 1
	  // NOTE: these two steps can be improved, e.g we could guess if
	  // P34+1 or P34-1 digits will be obtained by adding/subtracting 
	  // just the top 64 bits of the two operands
	  // The result cannot be zero, and it cannot overflow
	  x1 = q2 - 1;	// 0 <= x1 <= P34-1
	  // Calculate C1 * 10^(e1-e2-x1) where 1 <= e1-e2-x1 <= P34
	  // scale = (int)(e1 >> 49) - (int)(e2 >> 49) - x1; 0 <= scale <= P34-1
	  scale = P34 - q1 + 1;	// scale=e1-e2-x1 = P34+1-q1; 1<=scale<=P34
	  // either C1 or 10^(e1-e2-x1) may not fit is 64 bits,
	  // but their product fits with certainty in 128 bits
	  if (scale >= 20) {	//10^(e1-e2-x1) doesn't fit in 64 bits, but C1 does
	    __mul_128x64_to_128 (C1, C1_lo, bid_ten2k128[scale - 20]);
	  } else {	// if (scale >= 1
	    // if 1 <= scale <= 19 then 10^(e1-e2-x1) fits in 64 bits
	    if (q1 <= 19) {	// C1 fits in 64 bits
	      __mul_64x64_to_128MACH (C1, C1_lo, bid_ten2k64[scale]);
	    } else {	// q1 >= 20
	      C1.w[1] = C1_hi;
	      C1.w[0] = C1_lo;
	      __mul_128x64_to_128 (C1, bid_ten2k64[scale], C1);
	    }
	  }
	  tmp64 = C1.w[0];	// C1.w[1], C1.w[0] contains C1 * 10^(e1-e2-x1)

	  // now round C2 to q2-x1 = 1 decimal digit
	  // C2' = C2 + 1/2 * 10^x1 = C2 + 5 * 10^(x1-1)
	  ind = x1 - 1;	// -1 <= ind <= P34 - 2
	  if (ind >= 0) {	// if (x1 >= 1)
	    C2.w[0] = C2_lo;
	    C2.w[1] = C2_hi;
	    if (ind <= 18) {
	      C2.w[0] = C2.w[0] + bid_midpoint64[ind];
	      if (C2.w[0] < C2_lo)
		C2.w[1]++;
	    } else {	// 19 <= ind <= 32
	      C2.w[0] = C2.w[0] + bid_midpoint128[ind - 19].w[0];
	      C2.w[1] = C2.w[1] + bid_midpoint128[ind - 19].w[1];
	      if (C2.w[0] < C2_lo)
		C2.w[1]++;
	    }
	    // the approximation of 10^(-x1) was rounded up to 118 bits
	    __mul_128x128_to_256 (R256, C2, bid_ten2mk128[ind]);	// R256 = C2*, f2*
	    // calculate C2* and f2*
	    // C2* is actually floor(C2*) in this case
	    // C2* and f2* need shifting and masking, as shown by
	    // bid_shiftright128[] and bid_maskhigh128[]
	    // the top Ex bits of 10^(-x1) are T* = bid_ten2mk128trunc[ind], e.g.
	    // if x1=1, T*=bid_ten2mk128trunc[0]=0x19999999999999999999999999999999
	    // if (0 < f2* < 10^(-x1)) then
	    //   if floor(C1+C2*) is even then C2* = floor(C2*) - logical right
	    //       shift; C2* has p decimal digits, correct by Prop. 1)
	    //   else if floor(C1+C2*) is odd C2* = floor(C2*)-1 (logical right
	    //       shift; C2* has p decimal digits, correct by Pr. 1)
	    // else
	    //   C2* = floor(C2*) (logical right shift; C has p decimal digits,
	    //       correct by Property 1)
	    // n = C2* * 10^(e2+x1)

	    if (ind <= 2) {
	      highf2star.w[1] = 0x0;
	      highf2star.w[0] = 0x0;	// low f2* ok
	    } else if (ind <= 21) {
	      highf2star.w[1] = 0x0;
	      highf2star.w[0] = R256.w[2] & bid_maskhigh128[ind];	// low f2* ok
	    } else {
	      highf2star.w[1] = R256.w[3] & bid_maskhigh128[ind];
	      highf2star.w[0] = R256.w[2];	// low f2* is ok
	    }
	    // shift right C2* by Ex-128 = bid_shiftright128[ind]
	    if (ind >= 3) {
	      shift = bid_shiftright128[ind];
	      if (shift < 64) {	// 3 <= shift <= 63
		R256.w[2] =
		  (R256.w[2] >> shift) | (R256.w[3] << (64 - shift));
		R256.w[3] = (R256.w[3] >> shift);
	      } else {	// 66 <= shift <= 102
		R256.w[2] = (R256.w[3] >> (shift - 64));
		R256.w[3] = 0x0ULL;
	      }
	    }
	    // redundant
	    is_inexact_lt_midpoint = 0;
	    is_inexact_gt_midpoint = 0;
	    is_midpoint_lt_even = 0;
	    is_midpoint_gt_even = 0;
	    // determine inexactness of the rounding of C2*
	    // (cannot be followed by a second rounding)
	    // if (0 < f2* - 1/2 < 10^(-x1)) then
	    //   the result is exact
	    // else (if f2* - 1/2 > T* then)
	    //   the result of is inexact
	    if (ind <= 2) {
	      if (R256.w[1] > 0x8000000000000000ull ||
		  (R256.w[1] == 0x8000000000000000ull
		   && R256.w[0] > 0x0ull)) {
		// f2* > 1/2 and the result may be exact
		tmp64A = R256.w[1] - 0x8000000000000000ull;	// f* - 1/2
		if ((tmp64A > bid_ten2mk128trunc[ind].w[1]
		     || (tmp64A == bid_ten2mk128trunc[ind].w[1]
			 && R256.w[0] >= bid_ten2mk128trunc[ind].w[0]))) {
		  // set the inexact flag
		  *pfpsf |= BID_INEXACT_EXCEPTION;
		  // this rounding is applied to C2 only!
		  // x_sign != y_sign
		  is_inexact_gt_midpoint = 1;
		}	// else the result is exact
		// rounding down, unless a midpoint in [ODD, EVEN]
	      } else {	// the result is inexact; f2* <= 1/2
		// set the inexact flag
		*pfpsf |= BID_INEXACT_EXCEPTION;
		// this rounding is applied to C2 only!
		// x_sign != y_sign
		is_inexact_lt_midpoint = 1;
	      }
	    } else if (ind <= 21) {	// if 3 <= ind <= 21
	      if (highf2star.w[1] > 0x0 || (highf2star.w[1] == 0x0
					    && highf2star.w[0] >
					    bid_onehalf128[ind])
		  || (highf2star.w[1] == 0x0
		      && highf2star.w[0] == bid_onehalf128[ind]
		      && (R256.w[1] || R256.w[0]))) {
		// f2* > 1/2 and the result may be exact
		// Calculate f2* - 1/2
		tmp64A = highf2star.w[0] - bid_onehalf128[ind];
		tmp64B = highf2star.w[1];
		if (tmp64A > highf2star.w[0])
		  tmp64B--;
		if (tmp64B || tmp64A
		    || R256.w[1] > bid_ten2mk128trunc[ind].w[1]
		    || (R256.w[1] == bid_ten2mk128trunc[ind].w[1]
			&& R256.w[0] > bid_ten2mk128trunc[ind].w[0])) {
		  // set the inexact flag
		  *pfpsf |= BID_INEXACT_EXCEPTION;
		  // this rounding is applied to C2 only!
		  // x_sign != y_sign
		  is_inexact_gt_midpoint = 1;
		}	// else the result is exact
	      } else {	// the result is inexact; f2* <= 1/2
		// set the inexact flag
		*pfpsf |= BID_INEXACT_EXCEPTION;
		// this rounding is applied to C2 only!
		// x_sign != y_sign
		is_inexact_lt_midpoint = 1;
	      }
	    } else {	// if 22 <= ind <= 33
	      if (highf2star.w[1] > bid_onehalf128[ind]
		  || (highf2star.w[1] == bid_onehalf128[ind]
		      && (highf2star.w[0] || R256.w[1]
			  || R256.w[0]))) {
		// f2* > 1/2 and the result may be exact
		// Calculate f2* - 1/2
		// tmp64A = highf2star.w[0];
		tmp64B = highf2star.w[1] - bid_onehalf128[ind];
		if (tmp64B || highf2star.w[0]
		    || R256.w[1] > bid_ten2mk128trunc[ind].w[1]
		    || (R256.w[1] == bid_ten2mk128trunc[ind].w[1]
			&& R256.w[0] > bid_ten2mk128trunc[ind].w[0])) {
		  // set the inexact flag
		  *pfpsf |= BID_INEXACT_EXCEPTION;
		  // this rounding is applied to C2 only!
		  // x_sign != y_sign
		  is_inexact_gt_midpoint = 1;
		}	// else the result is exact
	      } else {	// the result is inexact; f2* <= 1/2
		// set the inexact flag
		*pfpsf |= BID_INEXACT_EXCEPTION;
		// this rounding is applied to C2 only!
		// x_sign != y_sign
		is_inexact_lt_midpoint = 1;
	      }
	    }
	    // check for midpoints after determining inexactness
	    if ((R256.w[1] || R256.w[0]) && (highf2star.w[1] == 0)
		&& (highf2star.w[0] == 0)
		&& (R256.w[1] < bid_ten2mk128trunc[ind].w[1]
		    || (R256.w[1] == bid_ten2mk128trunc[ind].w[1]
			&& R256.w[0] <= bid_ten2mk128trunc[ind].w[0]))) {
	      // the result is a midpoint
	      if ((tmp64 + R256.w[2]) & 0x01) {	// MP in [EVEN, ODD]
		// if floor(C2*) is odd C = floor(C2*) - 1; the result may be 0
		R256.w[2]--;
		if (R256.w[2] == 0xffffffffffffffffull)
		  R256.w[3]--;
		// this rounding is applied to C2 only!
		// x_sign != y_sign
		is_midpoint_lt_even = 1;
		is_inexact_lt_midpoint = 0;
		is_inexact_gt_midpoint = 0;
	      } else {
		// else MP in [ODD, EVEN]
		// this rounding is applied to C2 only!
		// x_sign != y_sign
		is_midpoint_gt_even = 1;
		is_inexact_lt_midpoint = 0;
		is_inexact_gt_midpoint = 0;
	      }
	    }
	  } else {	// if (ind == -1) only when x1 = 0
	    R256.w[2] = C2_lo;
	    R256.w[3] = C2_hi;
	    is_midpoint_lt_even = 0;
	    is_midpoint_gt_even = 0;
	    is_inexact_lt_midpoint = 0;
	    is_inexact_gt_midpoint = 0;
	  }
	  // and now subtract C1 * 10^(e1-e2-x1) - (C2 * 10^(-x1))rnd,P34
	  // because x_sign != y_sign this last operation is exact
	  C1.w[0] = C1.w[0] - R256.w[2];
	  C1.w[1] = C1.w[1] - R256.w[3];
	  if (C1.w[0] > tmp64)
	    C1.w[1]--;	// borrow
	  if (C1.w[1] >= 0x8000000000000000ull) {	// negative coefficient!
	    C1.w[0] = ~C1.w[0];
	    C1.w[0]++;
	    C1.w[1] = ~C1.w[1];
	    if (C1.w[0] == 0x0)
	      C1.w[1]++;
	    tmp_sign = y_sign;	// the result will have the sign of y
	  } else {
	    tmp_sign = x_sign;
	  }
	  // the difference has exactly P34 digits
	  x_sign = tmp_sign;
	  if (x1 >= 1)
	    y_exp = y_exp + ((BID_UINT64) x1 << 49);
	  C1_hi = C1.w[1];
	  C1_lo = C1.w[0];
	  // general correction from RN to RA, RM, RP, RZ; result uses y_exp
	  if (rnd_mode != BID_ROUNDING_TO_NEAREST) {
	    if ((!x_sign
		 && ((rnd_mode == BID_ROUNDING_UP && is_inexact_lt_midpoint)
		     ||
		     ((rnd_mode == BID_ROUNDING_TIES_AWAY
		       || rnd_mode == BID_ROUNDING_UP)
		      && is_midpoint_gt_even))) || (x_sign
						    &&
						    ((rnd_mode ==
						      BID_ROUNDING_DOWN
						      &&
						      is_inexact_lt_midpoint)
						     ||
						     ((rnd_mode ==
						       BID_ROUNDING_TIES_AWAY
						       || rnd_mode ==
						       BID_ROUNDING_DOWN)
						      &&
						      is_midpoint_gt_even))))
	    {
	      // C1 = C1 + 1
	      C1_lo = C1_lo + 1;
	      if (C1_lo == 0) {	// rounding overflow in the low 64 bits
		C1_hi = C1_hi + 1;
	      }
	      if (C1_hi == 0x0001ed09bead87c0ull
		  && C1_lo == 0x378d8e6400000000ull) {
		// C1 = 10^34 => rounding overflow
		C1_hi = 0x0000314dc6448d93ull;
		C1_lo = 0x38c15b0a00000000ull;	// 10^33
		y_exp = y_exp + EXP_P1;
	      }
	    } else if ((is_midpoint_lt_even || is_inexact_gt_midpoint)
		       &&
		       ((x_sign
			 && (rnd_mode == BID_ROUNDING_UP
			     || rnd_mode == BID_ROUNDING_TO_ZERO))
			|| (!x_sign
			    && (rnd_mode == BID_ROUNDING_DOWN
				|| rnd_mode == BID_ROUNDING_TO_ZERO)))) {
	      // C1 = C1 - 1
	      C1_lo = C1_lo - 1;
	      if (C1_lo == 0xffffffffffffffffull)
		C1_hi--;
	      // check if we crossed into the lower decade
	      if (C1_hi == 0x0000314dc6448d93ull && C1_lo == 0x38c15b09ffffffffull) {	// 10^33 - 1
		C1_hi = 0x0001ed09bead87c0ull;	// 10^34 - 1
		C1_lo = 0x378d8e63ffffffffull;
		y_exp = y_exp - EXP_P1;
		// no underflow, because delta + q2 >= P34 + 1
	      }
	    } else {
	      ;	// exact, the result is already correct
	    }
	  }
	  // assemble the result
	  res.w[1] = x_sign | y_exp | C1_hi;
	  res.w[0] = C1_lo;
	}
      }	// end delta = P34
    } else {	// if (|delta| <= P34 - 1)
      if (delta >= 0) {	// if (0 <= delta <= P34 - 1)
	if (delta <= P34 - 1 - q2) {
	  // calculate C' directly; the result is exact
	  // in this case 1<=q1<=P34-1, 1<=q2<=P34-1 and 0 <= e1-e2 <= P34-2
	  // The coefficient of the result is C1 * 10^(e1-e2) + C2 and the
	  // exponent is e2; either C1 or 10^(e1-e2) may not fit is 64 bits,
	  // but their product fits with certainty in 128 bits (actually in 113)
	  scale = delta - q1 + q2;	// scale = (int)(e1 >> 49) - (int)(e2 >> 49) 

	  if (scale >= 20) {	// 10^(e1-e2) does not fit in 64 bits, but C1 does
	    __mul_128x64_to_128 (C1, C1_lo, bid_ten2k128[scale - 20]);
	    C1_hi = C1.w[1];
	    C1_lo = C1.w[0];
	  } else if (scale >= 1) {
	    // if 1 <= scale <= 19 then 10^(e1-e2) fits in 64 bits 
	    if (q1 <= 19) {	// C1 fits in 64 bits
	      __mul_64x64_to_128MACH (C1, C1_lo, bid_ten2k64[scale]);
	    } else {	// q1 >= 20
	      C1.w[1] = C1_hi;
	      C1.w[0] = C1_lo;
	      __mul_128x64_to_128 (C1, bid_ten2k64[scale], C1);
	    }
	    C1_hi = C1.w[1];
	    C1_lo = C1.w[0];
	  } else {	// if (scale == 0) C1 is unchanged
	    C1.w[0] = C1_lo;	// C1.w[1] = C1_hi; 
	  }
	  // now add C2
	  if (x_sign == y_sign) {
	    // the result cannot overflow
	    C1_lo = C1_lo + C2_lo;
	    C1_hi = C1_hi + C2_hi;
	    if (C1_lo < C1.w[0])
	      C1_hi++;
	  } else {	// if x_sign != y_sign
	    C1_lo = C1_lo - C2_lo;
	    C1_hi = C1_hi - C2_hi;
	    if (C1_lo > C1.w[0])
	      C1_hi--;
	    // the result can be zero, but it cannot overflow
	    if (C1_lo == 0 && C1_hi == 0) {
	      // assemble the result
	      if (x_exp < y_exp)
		res.w[1] = x_exp;
	      else
		res.w[1] = y_exp;
	      res.w[0] = 0;
	      if (rnd_mode == BID_ROUNDING_DOWN) {
		res.w[1] |= 0x8000000000000000ull;
	      }
	      BID_SWAP128 (res);
	      BID_RETURN (res);
	    }
	    if (C1_hi >= 0x8000000000000000ull) {	// negative coefficient!
	      C1_lo = ~C1_lo;
	      C1_lo++;
	      C1_hi = ~C1_hi;
	      if (C1_lo == 0x0)
		C1_hi++;
	      x_sign = y_sign;	// the result will have the sign of y
	    }
	  }
	  // assemble the result
	  res.w[1] = x_sign | y_exp | C1_hi;
	  res.w[0] = C1_lo;
	} else if (delta == P34 - q2) {
	  // calculate C' directly; the result may be inexact if it requires 
	  // P34+1 decimal digits; in this case the 'cutoff' point for addition
	  // is at the position of the lsb of C2, so 0 <= e1-e2 <= P34-1
	  // The coefficient of the result is C1 * 10^(e1-e2) + C2 and the
	  // exponent is e2; either C1 or 10^(e1-e2) may not fit is 64 bits,
	  // but their product fits with certainty in 128 bits (actually in 113)
	  scale = delta - q1 + q2;	// scale = (int)(e1 >> 49) - (int)(e2 >> 49)
	  if (scale >= 20) {	// 10^(e1-e2) does not fit in 64 bits, but C1 does
	    __mul_128x64_to_128 (C1, C1_lo, bid_ten2k128[scale - 20]);
	  } else if (scale >= 1) {
	    // if 1 <= scale <= 19 then 10^(e1-e2) fits in 64 bits
	    if (q1 <= 19) {	// C1 fits in 64 bits
	      __mul_64x64_to_128MACH (C1, C1_lo, bid_ten2k64[scale]);
	    } else {	// q1 >= 20
	      C1.w[1] = C1_hi;
	      C1.w[0] = C1_lo;
	      __mul_128x64_to_128 (C1, bid_ten2k64[scale], C1);
	    }
	  } else {	// if (scale == 0) C1 is unchanged
	    C1.w[1] = C1_hi;
	    C1.w[0] = C1_lo;	// only the low part is necessary
	  }
	  C1_hi = C1.w[1];
	  C1_lo = C1.w[0];
	  // now add C2
	  if (x_sign == y_sign) {
	    // the result can overflow!
	    C1_lo = C1_lo + C2_lo;
	    C1_hi = C1_hi + C2_hi;
	    if (C1_lo < C1.w[0])
	      C1_hi++;
	    // test for overflow, possible only when C1 >= 10^34
	    if (C1_hi > 0x0001ed09bead87c0ull || (C1_hi == 0x0001ed09bead87c0ull && C1_lo >= 0x378d8e6400000000ull)) {	// C1 >= 10^34
	      // in this case q = P34 + 1 and x = q - P34 = 1, so multiply 
	      // C'' = C'+ 5 = C1 + 5 by k1 ~ 10^(-1) calculated for P34 + 1 
	      // decimal digits
	      // Calculate C'' = C' + 1/2 * 10^x
	      if (C1_lo >= 0xfffffffffffffffbull) {	// low half add has carry
		C1_lo = C1_lo + 5;
		C1_hi = C1_hi + 1;
	      } else {
		C1_lo = C1_lo + 5;
	      }
	      // the approximation of 10^(-1) was rounded up to 118 bits
	      // 10^(-1) =~ 33333333333333333333333333333400 * 2^-129
	      // 10^(-1) =~ 19999999999999999999999999999a00 * 2^-128
	      C1.w[1] = C1_hi;
	      C1.w[0] = C1_lo;	// C''
	      ten2m1.w[1] = 0x1999999999999999ull;
	      ten2m1.w[0] = 0x9999999999999a00ull;
	      __mul_128x128_to_256 (P256, C1, ten2m1);	// P256 = C*, f*
	      // C* is actually floor(C*) in this case
	      // the top Ex = 128 bits of 10^(-1) are 
	      // T* = 0x00199999999999999999999999999999
	      // if (0 < f* < 10^(-x)) then
	      //   if floor(C*) is even then C = floor(C*) - logical right 
	      //       shift; C has p decimal digits, correct by Prop. 1)
	      //   else if floor(C*) is odd C = floor(C*) - 1 (logical right
	      //       shift; C has p decimal digits, correct by Pr. 1)
	      // else
	      //   C = floor(C*) (logical right shift; C has p decimal digits,
	      //       correct by Property 1)
	      // n = C * 10^(e2+x)
	      if ((P256.w[1] || P256.w[0])
		  && (P256.w[1] < 0x1999999999999999ull
		      || (P256.w[1] == 0x1999999999999999ull
			  && P256.w[0] <= 0x9999999999999999ull))) {
		// the result is a midpoint
		if (P256.w[2] & 0x01) {
		  is_midpoint_gt_even = 1;
		  // if floor(C*) is odd C = floor(C*) - 1; the result is not 0
		  P256.w[2]--;
		  if (P256.w[2] == 0xffffffffffffffffull)
		    P256.w[3]--;
		} else {
		  is_midpoint_lt_even = 1;
		}
	      }
	      // n = Cstar * 10^(e2+1)
	      y_exp = y_exp + EXP_P1;
	      // C* != 10^P because C* has P34 digits
	      // check for overflow
	      if (y_exp == EXP_MAX_P1
		  && (rnd_mode == BID_ROUNDING_TO_NEAREST
		      || rnd_mode == BID_ROUNDING_TIES_AWAY)) {
		// overflow for RN
		res.w[1] = x_sign | 0x7800000000000000ull;	// +/-inf
		res.w[0] = 0x0ull;
		// set the inexact flag
		*pfpsf |= BID_INEXACT_EXCEPTION;
		// set the overflow flag
		*pfpsf |= BID_OVERFLOW_EXCEPTION;
		BID_SWAP128 (res);
		BID_RETURN (res);
	      }
	      // if (0 < f* - 1/2 < 10^(-x)) then 
	      //   the result of the addition is exact 
	      // else 
	      //   the result of the addition is inexact
	      if (P256.w[1] > 0x8000000000000000ull || (P256.w[1] == 0x8000000000000000ull && P256.w[0] > 0x0ull)) {	// the result may be exact
		tmp64 = P256.w[1] - 0x8000000000000000ull;	// f* - 1/2
		if ((tmp64 > 0x1999999999999999ull
		     || (tmp64 == 0x1999999999999999ull
			 && P256.w[0] >= 0x9999999999999999ull))) {
		  // set the inexact flag
		  *pfpsf |= BID_INEXACT_EXCEPTION;
		  is_inexact = 1;
		}	// else the result is exact
	      } else {	// the result is inexact
		// set the inexact flag
		*pfpsf |= BID_INEXACT_EXCEPTION;
		is_inexact = 1;
	      }
	      C1_hi = P256.w[3];
	      C1_lo = P256.w[2];
	      if (!is_midpoint_gt_even && !is_midpoint_lt_even) {
		is_inexact_lt_midpoint = is_inexact
		  && (P256.w[1] & 0x8000000000000000ull);
		is_inexact_gt_midpoint = is_inexact
		  && !(P256.w[1] & 0x8000000000000000ull);
	      }
	      // general correction from RN to RA, RM, RP, RZ; 
	      // result uses y_exp
	      if (rnd_mode != BID_ROUNDING_TO_NEAREST) {
		if ((!x_sign
		     &&
		     ((rnd_mode == BID_ROUNDING_UP
		       && is_inexact_lt_midpoint)
		      ||
		      ((rnd_mode == BID_ROUNDING_TIES_AWAY
			|| rnd_mode == BID_ROUNDING_UP)
		       && is_midpoint_gt_even))) || (x_sign
						     &&
						     ((rnd_mode ==
						       BID_ROUNDING_DOWN
						       &&
						       is_inexact_lt_midpoint)
						      ||
						      ((rnd_mode ==
							BID_ROUNDING_TIES_AWAY
							|| rnd_mode ==
							BID_ROUNDING_DOWN)
						       &&
						       is_midpoint_gt_even))))
		{
		  // C1 = C1 + 1
		  C1_lo = C1_lo + 1;
		  if (C1_lo == 0) {	// rounding overflow in the low 64 bits
		    C1_hi = C1_hi + 1;
		  }
		  if (C1_hi == 0x0001ed09bead87c0ull
		      && C1_lo == 0x378d8e6400000000ull) {
		    // C1 = 10^34 => rounding overflow
		    C1_hi = 0x0000314dc6448d93ull;
		    C1_lo = 0x38c15b0a00000000ull;	// 10^33
		    y_exp = y_exp + EXP_P1;
		  }
		} else
		  if ((is_midpoint_lt_even || is_inexact_gt_midpoint)
		      &&
		      ((x_sign
			&& (rnd_mode == BID_ROUNDING_UP
			    || rnd_mode == BID_ROUNDING_TO_ZERO))
		       || (!x_sign
			   && (rnd_mode == BID_ROUNDING_DOWN
			       || rnd_mode == BID_ROUNDING_TO_ZERO)))) {
		  // C1 = C1 - 1
		  C1_lo = C1_lo - 1;
		  if (C1_lo == 0xffffffffffffffffull)
		    C1_hi--;
		  // check if we crossed into the lower decade
		  if (C1_hi == 0x0000314dc6448d93ull && C1_lo == 0x38c15b09ffffffffull) {	// 10^33 - 1
		    C1_hi = 0x0001ed09bead87c0ull;	// 10^34 - 1
		    C1_lo = 0x378d8e63ffffffffull;
		    y_exp = y_exp - EXP_P1;
		    // no underflow, because delta + q2 >= P34 + 1
		  }
		} else {
		  ;	// exact, the result is already correct
		}
		// in all cases check for overflow (RN and RA solved already)
		if (y_exp == EXP_MAX_P1) {	// overflow
		  if ((rnd_mode == BID_ROUNDING_DOWN && x_sign) ||	// RM and res < 0
		      (rnd_mode == BID_ROUNDING_UP && !x_sign)) {	// RP and res > 0
		    C1_hi = 0x7800000000000000ull;	// +inf
		    C1_lo = 0x0ull;
		  } else {	// RM and res > 0, RP and res < 0, or RZ
		    C1_hi = 0x5fffed09bead87c0ull;
		    C1_lo = 0x378d8e63ffffffffull;
		  }
		  y_exp = 0;	// x_sign is preserved
		  // set the inexact flag (in case the exact addition was exact)
		  *pfpsf |= BID_INEXACT_EXCEPTION;
		  // set the overflow flag
		  *pfpsf |= BID_OVERFLOW_EXCEPTION;
		}
	      }
	    }	// else if (C1 < 10^34) then C1 is the coeff.; the result is exact
	  } else {	// if x_sign != y_sign the result is exact
	    C1_lo = C1_lo - C2_lo;
	    C1_hi = C1_hi - C2_hi;
	    if (C1_lo > C1.w[0])
	      C1_hi--;
	    // the result can be zero, but it cannot overflow
	    if (C1_lo == 0 && C1_hi == 0) {
	      // assemble the result
	      if (x_exp < y_exp)
		res.w[1] = x_exp;
	      else
		res.w[1] = y_exp;
	      res.w[0] = 0;
	      if (rnd_mode == BID_ROUNDING_DOWN) {
		res.w[1] |= 0x8000000000000000ull;
	      }
	      BID_SWAP128 (res);
	      BID_RETURN (res);
	    }
	    if (C1_hi >= 0x8000000000000000ull) {	// negative coefficient!
	      C1_lo = ~C1_lo;
	      C1_lo++;
	      C1_hi = ~C1_hi;
	      if (C1_lo == 0x0)
		C1_hi++;
	      x_sign = y_sign;	// the result will have the sign of y
	    }
	  }
	  // assemble the result
	  res.w[1] = x_sign | y_exp | C1_hi;
	  res.w[0] = C1_lo;
	} else {	// if (delta >= P34 + 1 - q2)
	  // instead of C' = (C1 * 10^(e1-e2) + C2)rnd,P34
	  // calculate C' = C1 * 10^(e1-e2-x1) + (C2 * 10^(-x1))rnd,P34 
	  // where x1 = q1 + e1 - e2 - P34, 1 <= x1 <= P34 - 1
	  // In most cases C' will have P34 digits, and n = C' * 10^(e2+x1)
	  // If the result has P34+1 digits, redo the steps above with x1+1
	  // If the result has P34-1 digits or less, redo the steps above with 
	  // x1-1 but only if initially x1 >= 1
	  // NOTE: these two steps can be improved, e.g we could guess if
	  // P34+1 or P34-1 digits will be obtained by adding/subtracting just
	  // the top 64 bits of the two operands
	  // The result cannot be zero, but it can overflow
	  x1 = delta + q2 - P34;	// 1 <= x1 <= P34-1
	roundC2:
	  // Calculate C1 * 10^(e1-e2-x1) where 0 <= e1-e2-x1 <= P34 - 1
	  // scale = (int)(e1 >> 49) - (int)(e2 >> 49) - x1; 0 <= scale <= P34-1
	  scale = delta - q1 + q2 - x1;	// scale = e1 - e2 - x1 = P34 - q1
	  // either C1 or 10^(e1-e2-x1) may not fit is 64 bits,
	  // but their product fits with certainty in 128 bits (actually in 113)
	  if (scale >= 20) {	//10^(e1-e2-x1) doesn't fit in 64 bits, but C1 does
	    __mul_128x64_to_128 (C1, C1_lo, bid_ten2k128[scale - 20]);
	  } else if (scale >= 1) {
	    // if 1 <= scale <= 19 then 10^(e1-e2-x1) fits in 64 bits
	    if (q1 <= 19) {	// C1 fits in 64 bits
	      __mul_64x64_to_128MACH (C1, C1_lo, bid_ten2k64[scale]);
	    } else {	// q1 >= 20
	      C1.w[1] = C1_hi;
	      C1.w[0] = C1_lo;
	      __mul_128x64_to_128 (C1, bid_ten2k64[scale], C1);
	    }
	  } else {	// if (scale == 0) C1 is unchanged
	    C1.w[1] = C1_hi;
	    C1.w[0] = C1_lo;
	  }
	  tmp64 = C1.w[0];	// C1.w[1], C1.w[0] contains C1 * 10^(e1-e2-x1)

	  // now round C2 to q2-x1 decimal digits, where 1<=x1<=q2-1<=P34-1
	  // (but if we got here a second time after x1 = x1 - 1, then 
	  // x1 >= 0; note that for x1 = 0 C2 is unchanged)
	  // C2' = C2 + 1/2 * 10^x1 = C2 + 5 * 10^(x1-1)
	  ind = x1 - 1;	// 0 <= ind <= q2-2<=P34-2=32; but note that if x1 = 0
	  // during a second pass, then ind = -1
	  if (ind >= 0) {	// if (x1 >= 1)
	    C2.w[0] = C2_lo;
	    C2.w[1] = C2_hi;
	    if (ind <= 18) {
	      C2.w[0] = C2.w[0] + bid_midpoint64[ind];
	      if (C2.w[0] < C2_lo)
		C2.w[1]++;
	    } else {	// 19 <= ind <= 32
	      C2.w[0] = C2.w[0] + bid_midpoint128[ind - 19].w[0];
	      C2.w[1] = C2.w[1] + bid_midpoint128[ind - 19].w[1];
	      if (C2.w[0] < C2_lo)
		C2.w[1]++;
	    }
	    // the approximation of 10^(-x1) was rounded up to 118 bits
	    __mul_128x128_to_256 (R256, C2, bid_ten2mk128[ind]);	// R256 = C2*, f2*
	    // calculate C2* and f2*
	    // C2* is actually floor(C2*) in this case
	    // C2* and f2* need shifting and masking, as shown by
	    // bid_shiftright128[] and bid_maskhigh128[]
	    // the top Ex bits of 10^(-x1) are T* = bid_ten2mk128trunc[ind], e.g.
	    // if x1=1, T*=bid_ten2mk128trunc[0]=0x19999999999999999999999999999999
	    // if (0 < f2* < 10^(-x1)) then
	    //   if floor(C1+C2*) is even then C2* = floor(C2*) - logical right
	    //       shift; C2* has p decimal digits, correct by Prop. 1)
	    //   else if floor(C1+C2*) is odd C2* = floor(C2*)-1 (logical right
	    //       shift; C2* has p decimal digits, correct by Pr. 1)
	    // else
	    //   C2* = floor(C2*) (logical right shift; C has p decimal digits,
	    //       correct by Property 1)
	    // n = C2* * 10^(e2+x1)

	    if (ind <= 2) {
	      highf2star.w[1] = 0x0;
	      highf2star.w[0] = 0x0;	// low f2* ok
	    } else if (ind <= 21) {
	      highf2star.w[1] = 0x0;
	      highf2star.w[0] = R256.w[2] & bid_maskhigh128[ind];	// low f2* ok
	    } else {
	      highf2star.w[1] = R256.w[3] & bid_maskhigh128[ind];
	      highf2star.w[0] = R256.w[2];	// low f2* is ok
	    }
	    // shift right C2* by Ex-128 = bid_shiftright128[ind]
	    if (ind >= 3) {
	      shift = bid_shiftright128[ind];
	      if (shift < 64) {	// 3 <= shift <= 63
		R256.w[2] =
		  (R256.w[2] >> shift) | (R256.w[3] << (64 - shift));
		R256.w[3] = (R256.w[3] >> shift);
	      } else {	// 66 <= shift <= 102
		R256.w[2] = (R256.w[3] >> (shift - 64));
		R256.w[3] = 0x0ULL;
	      }
	    }
	    if (second_pass) {
	      is_inexact_lt_midpoint = 0;
	      is_inexact_gt_midpoint = 0;
	      is_midpoint_lt_even = 0;
	      is_midpoint_gt_even = 0;
	    }
	    // determine inexactness of the rounding of C2* (this may be 
	    // followed by a second rounding only if we get P34+1 
	    // decimal digits)
	    // if (0 < f2* - 1/2 < 10^(-x1)) then
	    //   the result is exact
	    // else (if f2* - 1/2 > T* then)
	    //   the result of is inexact
	    if (ind <= 2) {
	      if (R256.w[1] > 0x8000000000000000ull ||
		  (R256.w[1] == 0x8000000000000000ull
		   && R256.w[0] > 0x0ull)) {
		// f2* > 1/2 and the result may be exact
		tmp64A = R256.w[1] - 0x8000000000000000ull;	// f* - 1/2
		if ((tmp64A > bid_ten2mk128trunc[ind].w[1]
		     || (tmp64A == bid_ten2mk128trunc[ind].w[1]
			 && R256.w[0] >= bid_ten2mk128trunc[ind].w[0]))) {
		  // set the inexact flag
		  // *pfpsf |= BID_INEXACT_EXCEPTION;
		  tmp_inexact = 1;	// may be set again during a second pass
		  // this rounding is applied to C2 only!
		  if (x_sign == y_sign)
		    is_inexact_lt_midpoint = 1;
		  else	// if (x_sign != y_sign)
		    is_inexact_gt_midpoint = 1;
		}	// else the result is exact
		// rounding down, unless a midpoint in [ODD, EVEN]
	      } else {	// the result is inexact; f2* <= 1/2
		// set the inexact flag
		// *pfpsf |= BID_INEXACT_EXCEPTION;
		tmp_inexact = 1;	// just in case we will round a second time
		// rounding up, unless a midpoint in [EVEN, ODD]
		// this rounding is applied to C2 only!
		if (x_sign == y_sign)
		  is_inexact_gt_midpoint = 1;
		else	// if (x_sign != y_sign)
		  is_inexact_lt_midpoint = 1;
	      }
	    } else if (ind <= 21) {	// if 3 <= ind <= 21
	      if (highf2star.w[1] > 0x0 || (highf2star.w[1] == 0x0
					    && highf2star.w[0] >
					    bid_onehalf128[ind])
		  || (highf2star.w[1] == 0x0
		      && highf2star.w[0] == bid_onehalf128[ind]
		      && (R256.w[1] || R256.w[0]))) {
		// f2* > 1/2 and the result may be exact
		// Calculate f2* - 1/2
		tmp64A = highf2star.w[0] - bid_onehalf128[ind];
		tmp64B = highf2star.w[1];
		if (tmp64A > highf2star.w[0])
		  tmp64B--;
		if (tmp64B || tmp64A
		    || R256.w[1] > bid_ten2mk128trunc[ind].w[1]
		    || (R256.w[1] == bid_ten2mk128trunc[ind].w[1]
			&& R256.w[0] > bid_ten2mk128trunc[ind].w[0])) {
		  // set the inexact flag
		  // *pfpsf |= BID_INEXACT_EXCEPTION;
		  tmp_inexact = 1;	// may be set again during a second pass
		  // this rounding is applied to C2 only!
		  if (x_sign == y_sign)
		    is_inexact_lt_midpoint = 1;
		  else	// if (x_sign != y_sign)
		    is_inexact_gt_midpoint = 1;
		}	// else the result is exact
	      } else {	// the result is inexact; f2* <= 1/2
		// set the inexact flag
		// *pfpsf |= BID_INEXACT_EXCEPTION;
		tmp_inexact = 1;	// may be set again during a second pass
		// rounding up, unless a midpoint in [EVEN, ODD]
		// this rounding is applied to C2 only!
		if (x_sign == y_sign)
		  is_inexact_gt_midpoint = 1;
		else	// if (x_sign != y_sign)
		  is_inexact_lt_midpoint = 1;
	      }
	    } else {	// if 22 <= ind <= 33
	      if (highf2star.w[1] > bid_onehalf128[ind]
		  || (highf2star.w[1] == bid_onehalf128[ind]
		      && (highf2star.w[0] || R256.w[1]
			  || R256.w[0]))) {
		// f2* > 1/2 and the result may be exact
		// Calculate f2* - 1/2
		// tmp64A = highf2star.w[0];
		tmp64B = highf2star.w[1] - bid_onehalf128[ind];
		if (tmp64B || highf2star.w[0]
		    || R256.w[1] > bid_ten2mk128trunc[ind].w[1]
		    || (R256.w[1] == bid_ten2mk128trunc[ind].w[1]
			&& R256.w[0] > bid_ten2mk128trunc[ind].w[0])) {
		  // set the inexact flag
		  // *pfpsf |= BID_INEXACT_EXCEPTION;
		  tmp_inexact = 1;	// may be set again during a second pass
		  // this rounding is applied to C2 only!
		  if (x_sign == y_sign)
		    is_inexact_lt_midpoint = 1;
		  else	// if (x_sign != y_sign)
		    is_inexact_gt_midpoint = 1;
		}	// else the result is exact
	      } else {	// the result is inexact; f2* <= 1/2
		// set the inexact flag
		// *pfpsf |= BID_INEXACT_EXCEPTION;
		tmp_inexact = 1;	// may be set again during a second pass
		// rounding up, unless a midpoint in [EVEN, ODD]
		// this rounding is applied to C2 only!
		if (x_sign == y_sign)
		  is_inexact_gt_midpoint = 1;
		else	// if (x_sign != y_sign)
		  is_inexact_lt_midpoint = 1;
	      }
	    }
	    // check for midpoints
	    if ((R256.w[1] || R256.w[0]) && (highf2star.w[1] == 0)
		&& (highf2star.w[0] == 0)
		&& (R256.w[1] < bid_ten2mk128trunc[ind].w[1]
		    || (R256.w[1] == bid_ten2mk128trunc[ind].w[1]
			&& R256.w[0] <= bid_ten2mk128trunc[ind].w[0]))) {
	      // the result is a midpoint
	      if ((tmp64 + R256.w[2]) & 0x01) {	// MP in [EVEN, ODD]
		// if floor(C2*) is odd C = floor(C2*) - 1; the result may be 0
		R256.w[2]--;
		if (R256.w[2] == 0xffffffffffffffffull)
		  R256.w[3]--;
		// this rounding is applied to C2 only!
		if (x_sign == y_sign)
		  is_midpoint_gt_even = 1;
		else	// if (x_sign != y_sign)
		  is_midpoint_lt_even = 1;
		is_inexact_lt_midpoint = 0;
		is_inexact_gt_midpoint = 0;
	      } else {
		// else MP in [ODD, EVEN]
		// this rounding is applied to C2 only!
		if (x_sign == y_sign)
		  is_midpoint_lt_even = 1;
		else	// if (x_sign != y_sign)
		  is_midpoint_gt_even = 1;
		is_inexact_lt_midpoint = 0;
		is_inexact_gt_midpoint = 0;
	      }
	    }
	    // end if (ind >= 0)
	  } else {	// if (ind == -1); only during a 2nd pass, and when x1 = 0
	    R256.w[2] = C2_lo;
	    R256.w[3] = C2_hi;
	    tmp_inexact = 0;
	    // to correct a possible setting to 1 from 1st pass
	    if (second_pass) {
	      is_midpoint_lt_even = 0;
	      is_midpoint_gt_even = 0;
	      is_inexact_lt_midpoint = 0;
	      is_inexact_gt_midpoint = 0;
	    }
	  }
	  // and now add/subtract C1 * 10^(e1-e2-x1) +/- (C2 * 10^(-x1))rnd,P34
	  if (x_sign == y_sign) {	// addition; could overflow
	    // no second pass is possible this way (only for x_sign != y_sign)
	    C1.w[0] = C1.w[0] + R256.w[2];
	    C1.w[1] = C1.w[1] + R256.w[3];
	    if (C1.w[0] < tmp64)
	      C1.w[1]++;	// carry
	    // if the sum has P34+1 digits, i.e. C1>=10^34 redo the calculation
	    // with x1=x1+1 
	    if (C1.w[1] > 0x0001ed09bead87c0ull || (C1.w[1] == 0x0001ed09bead87c0ull && C1.w[0] >= 0x378d8e6400000000ull)) {	// C1 >= 10^34
	      // chop off one more digit from the sum, but make sure there is
	      // no double-rounding error (see table - double rounding logic)
	      // now round C1 from P34+1 to P34 decimal digits
	      // C1' = C1 + 1/2 * 10 = C1 + 5
	      if (C1.w[0] >= 0xfffffffffffffffbull) {	// low half add has carry
		C1.w[0] = C1.w[0] + 5;
		C1.w[1] = C1.w[1] + 1;
	      } else {
		C1.w[0] = C1.w[0] + 5;
	      }
	      // the approximation of 10^(-1) was rounded up to 118 bits
	      __mul_128x128_to_256 (Q256, C1, bid_ten2mk128[0]);	// Q256 = C1*, f1*
	      // C1* is actually floor(C1*) in this case
	      // the top 128 bits of 10^(-1) are
	      // T* = bid_ten2mk128trunc[0]=0x19999999999999999999999999999999
	      // if (0 < f1* < 10^(-1)) then
	      //   if floor(C1*) is even then C1* = floor(C1*) - logical right
	      //       shift; C1* has p decimal digits, correct by Prop. 1)
	      //   else if floor(C1*) is odd C1* = floor(C1*) - 1 (logical right
	      //       shift; C1* has p decimal digits, correct by Pr. 1)
	      // else
	      //   C1* = floor(C1*) (logical right shift; C has p decimal digits
	      //       correct by Property 1)
	      // n = C1* * 10^(e2+x1+1)
	      if ((Q256.w[1] || Q256.w[0])
		  && (Q256.w[1] < bid_ten2mk128trunc[0].w[1]
		      || (Q256.w[1] == bid_ten2mk128trunc[0].w[1]
			  && Q256.w[0] <= bid_ten2mk128trunc[0].w[0]))) {
		// the result is a midpoint
		if (is_inexact_lt_midpoint) {	// for the 1st rounding
		  is_inexact_gt_midpoint = 1;
		  is_inexact_lt_midpoint = 0;
		  is_midpoint_gt_even = 0;
		  is_midpoint_lt_even = 0;
		} else if (is_inexact_gt_midpoint) {	// for the 1st rounding
		  Q256.w[2]--;
		  if (Q256.w[2] == 0xffffffffffffffffull)
		    Q256.w[3]--;
		  is_inexact_gt_midpoint = 0;
		  is_inexact_lt_midpoint = 1;
		  is_midpoint_gt_even = 0;
		  is_midpoint_lt_even = 0;
		} else if (is_midpoint_gt_even) {	// for the 1st rounding
		  // Note: cannot have is_midpoint_lt_even
		  is_inexact_gt_midpoint = 0;
		  is_inexact_lt_midpoint = 1;
		  is_midpoint_gt_even = 0;
		  is_midpoint_lt_even = 0;
		} else {	// the first rounding must have been exact
		  if (Q256.w[2] & 0x01) {	// MP in [EVEN, ODD]
		    // the truncated result is correct
		    Q256.w[2]--;
		    if (Q256.w[2] == 0xffffffffffffffffull)
		      Q256.w[3]--;
		    is_inexact_gt_midpoint = 0;
		    is_inexact_lt_midpoint = 0;
		    is_midpoint_gt_even = 1;
		    is_midpoint_lt_even = 0;
		  } else {	// MP in [ODD, EVEN]
		    is_inexact_gt_midpoint = 0;
		    is_inexact_lt_midpoint = 0;
		    is_midpoint_gt_even = 0;
		    is_midpoint_lt_even = 1;
		  }
		}
		tmp_inexact = 1;	// in all cases
	      } else {	// the result is not a midpoint 
		// determine inexactness of the rounding of C1 (the sum C1+C2*)
		// if (0 < f1* - 1/2 < 10^(-1)) then
		//   the result is exact
		// else (if f1* - 1/2 > T* then)
		//   the result of is inexact
		// ind = 0
		if (Q256.w[1] > 0x8000000000000000ull
		    || (Q256.w[1] == 0x8000000000000000ull
			&& Q256.w[0] > 0x0ull)) {
		  // f1* > 1/2 and the result may be exact
		  Q256.w[1] = Q256.w[1] - 0x8000000000000000ull;	// f1* - 1/2
		  if ((Q256.w[1] > bid_ten2mk128trunc[0].w[1]
		       || (Q256.w[1] == bid_ten2mk128trunc[0].w[1]
			   && Q256.w[0] > bid_ten2mk128trunc[0].w[0]))) {
		    is_inexact_gt_midpoint = 0;
		    is_inexact_lt_midpoint = 1;
		    is_midpoint_gt_even = 0;
		    is_midpoint_lt_even = 0;
		    // set the inexact flag
		    tmp_inexact = 1;
		    // *pfpsf |= BID_INEXACT_EXCEPTION;
		  } else {	// else the result is exact for the 2nd rounding
		    if (tmp_inexact) {	// if the previous rounding was inexact
		      if (is_midpoint_lt_even) {
			is_inexact_gt_midpoint = 1;
			is_midpoint_lt_even = 0;
		      } else if (is_midpoint_gt_even) {
			is_inexact_lt_midpoint = 1;
			is_midpoint_gt_even = 0;
		      } else {
			;	// no change
		      }
		    }
		  }
		  // rounding down, unless a midpoint in [ODD, EVEN]
		} else {	// the result is inexact; f1* <= 1/2
		  is_inexact_gt_midpoint = 1;
		  is_inexact_lt_midpoint = 0;
		  is_midpoint_gt_even = 0;
		  is_midpoint_lt_even = 0;
		  // set the inexact flag
		  tmp_inexact = 1;
		  // *pfpsf |= BID_INEXACT_EXCEPTION;
		}
	      }	// end 'the result is not a midpoint'
	      // n = C1 * 10^(e2+x1)
	      C1.w[1] = Q256.w[3];
	      C1.w[0] = Q256.w[2];
	      y_exp = y_exp + ((BID_UINT64) (x1 + 1) << 49);
	    } else {	// C1 < 10^34
	      // C1.w[1] and C1.w[0] already set
	      // n = C1 * 10^(e2+x1)
	      y_exp = y_exp + ((BID_UINT64) x1 << 49);
	    }
	    // check for overflow
	    if (y_exp == EXP_MAX_P1
		&& (rnd_mode == BID_ROUNDING_TO_NEAREST
		    || rnd_mode == BID_ROUNDING_TIES_AWAY)) {
	      res.w[1] = 0x7800000000000000ull | x_sign;	// +/-inf
	      res.w[0] = 0x0ull;
	      // set the inexact flag
	      *pfpsf |= BID_INEXACT_EXCEPTION;
	      // set the overflow flag
	      *pfpsf |= BID_OVERFLOW_EXCEPTION;
	      BID_SWAP128 (res);
	      BID_RETURN (res);
	    }	// else no overflow
	  } else {	// if x_sign != y_sign the result of this subtract. is exact
	    C1.w[0] = C1.w[0] - R256.w[2];
	    C1.w[1] = C1.w[1] - R256.w[3];
	    if (C1.w[0] > tmp64)
	      C1.w[1]--;	// borrow
	    if (C1.w[1] >= 0x8000000000000000ull) {	// negative coefficient!
	      C1.w[0] = ~C1.w[0];
	      C1.w[0]++;
	      C1.w[1] = ~C1.w[1];
	      if (C1.w[0] == 0x0)
		C1.w[1]++;
	      tmp_sign = y_sign;
	      // the result will have the sign of y if last rnd
	    } else {
	      tmp_sign = x_sign;
	    }
	    // if the difference has P34-1 digits or less, i.e. C1 < 10^33 then
	    //   redo the calculation with x1=x1-1;
	    // redo the calculation also if C1 = 10^33 and 
	    //   (is_inexact_gt_midpoint or is_midpoint_lt_even);
	    //   (the last part should have really been 
	    //   (is_inexact_lt_midpoint or is_midpoint_gt_even) from
	    //    the rounding of C2, but the position flags have been reversed)
	    // 10^33 = 0x0000314dc6448d93 0x38c15b0a00000000
	    if ((C1.w[1] < 0x0000314dc6448d93ull || (C1.w[1] == 0x0000314dc6448d93ull && C1.w[0] < 0x38c15b0a00000000ull)) || (C1.w[1] == 0x0000314dc6448d93ull && C1.w[0] == 0x38c15b0a00000000ull && (is_inexact_gt_midpoint || is_midpoint_lt_even))) {	// C1=10^33
	      x1 = x1 - 1;	// x1 >= 0
	      if (x1 >= 0) {
		// clear position flags and tmp_inexact
		is_midpoint_lt_even = 0;
		is_midpoint_gt_even = 0;
		is_inexact_lt_midpoint = 0;
		is_inexact_gt_midpoint = 0;
		tmp_inexact = 0;
		second_pass = 1;
		goto roundC2;	// else result has less than P34 digits
	      }
	    }
	    // if the coefficient of the result is 10^34 it means that this
	    // must be the second pass, and we are done 
	    if (C1.w[1] == 0x0001ed09bead87c0ull && C1.w[0] == 0x378d8e6400000000ull) {	// if  C1 = 10^34
	      C1.w[1] = 0x0000314dc6448d93ull;	// C1 = 10^33
	      C1.w[0] = 0x38c15b0a00000000ull;
	      y_exp = y_exp + ((BID_UINT64) 1 << 49);
	    }
	    x_sign = tmp_sign;
	    if (x1 >= 1)
	      y_exp = y_exp + ((BID_UINT64) x1 << 49);
	    // x1 = -1 is possible at the end of a second pass when the 
	    // first pass started with x1 = 1 
	  }
	  C1_hi = C1.w[1];
	  C1_lo = C1.w[0];
	  // general correction from RN to RA, RM, RP, RZ; result uses y_exp
	  if (rnd_mode != BID_ROUNDING_TO_NEAREST) {
	    if ((!x_sign
		 && ((rnd_mode == BID_ROUNDING_UP && is_inexact_lt_midpoint)
		     ||
		     ((rnd_mode == BID_ROUNDING_TIES_AWAY
		       || rnd_mode == BID_ROUNDING_UP)
		      && is_midpoint_gt_even))) || (x_sign
						    &&
						    ((rnd_mode ==
						      BID_ROUNDING_DOWN
						      &&
						      is_inexact_lt_midpoint)
						     ||
						     ((rnd_mode ==
						       BID_ROUNDING_TIES_AWAY
						       || rnd_mode ==
						       BID_ROUNDING_DOWN)
						      &&
						      is_midpoint_gt_even))))
	    {
	      // C1 = C1 + 1
	      C1_lo = C1_lo + 1;
	      if (C1_lo == 0) {	// rounding overflow in the low 64 bits
		C1_hi = C1_hi + 1;
	      }
	      if (C1_hi == 0x0001ed09bead87c0ull
		  && C1_lo == 0x378d8e6400000000ull) {
		// C1 = 10^34 => rounding overflow
		C1_hi = 0x0000314dc6448d93ull;
		C1_lo = 0x38c15b0a00000000ull;	// 10^33
		y_exp = y_exp + EXP_P1;
	      }
	    } else if ((is_midpoint_lt_even || is_inexact_gt_midpoint)
		       &&
		       ((x_sign
			 && (rnd_mode == BID_ROUNDING_UP
			     || rnd_mode == BID_ROUNDING_TO_ZERO))
			|| (!x_sign
			    && (rnd_mode == BID_ROUNDING_DOWN
				|| rnd_mode == BID_ROUNDING_TO_ZERO)))) {
	      // C1 = C1 - 1
	      C1_lo = C1_lo - 1;
	      if (C1_lo == 0xffffffffffffffffull)
		C1_hi--;
	      // check if we crossed into the lower decade
	      if (C1_hi == 0x0000314dc6448d93ull && C1_lo == 0x38c15b09ffffffffull) {	// 10^33 - 1
		C1_hi = 0x0001ed09bead87c0ull;	// 10^34 - 1
		C1_lo = 0x378d8e63ffffffffull;
		y_exp = y_exp - EXP_P1;
		// no underflow, because delta + q2 >= P34 + 1
	      }
	    } else {
	      ;	// exact, the result is already correct
	    }
	    // in all cases check for overflow (RN and RA solved already)
	    if (y_exp == EXP_MAX_P1) {	// overflow
	      if ((rnd_mode == BID_ROUNDING_DOWN && x_sign) ||	// RM and res < 0
		  (rnd_mode == BID_ROUNDING_UP && !x_sign)) {	// RP and res > 0
		C1_hi = 0x7800000000000000ull;	// +inf
		C1_lo = 0x0ull;
	      } else {	// RM and res > 0, RP and res < 0, or RZ
		C1_hi = 0x5fffed09bead87c0ull;
		C1_lo = 0x378d8e63ffffffffull;
	      }
	      y_exp = 0;	// x_sign is preserved
	      // set the inexact flag (in case the exact addition was exact)
	      *pfpsf |= BID_INEXACT_EXCEPTION;
	      // set the overflow flag
	      *pfpsf |= BID_OVERFLOW_EXCEPTION;
	    }
	  }
	  // assemble the result
	  res.w[1] = x_sign | y_exp | C1_hi;
	  res.w[0] = C1_lo;
	  if (tmp_inexact)
	    *pfpsf |= BID_INEXACT_EXCEPTION;
	}
      } else {	// if (-P34 + 1 <= delta <= -1) <=> 1 <= -delta <= P34 - 1
	// NOTE: the following, up to "} else { // if x_sign != y_sign 
	// the result is exact" is identical to "else if (delta == P34 - q2) {"
	// from above; also, the code is not symmetric: a+b and b+a may take
	// different paths (need to unify eventually!) 
	// calculate C' = C2 + C1 * 10^(e1-e2) directly; the result may be 
	// inexact if it requires P34 + 1 decimal digits; in either case the 
	// 'cutoff' point for addition is at the position of the lsb of C2
	// The coefficient of the result is C1 * 10^(e1-e2) + C2 and the
	// exponent is e2; either C1 or 10^(e1-e2) may not fit is 64 bits,
	// but their product fits with certainty in 128 bits (actually in 113)
	// Note that 0 <= e1 - e2 <= P34 - 2
	//   -P34 + 1 <= delta <= -1 <=> -P34 + 1 <= delta <= -1 <=>
	//   -P34 + 1 <= q1 + e1 - q2 - e2 <= -1 <=>
	//   q2 - q1 - P34 + 1 <= e1 - e2 <= q2 - q1 - 1 <=>
	//   1 - P34 - P34 + 1 <= e1-e2 <= P34 - 1 - 1 => 0 <= e1-e2 <= P34 - 2
	scale = delta - q1 + q2;	// scale = (int)(e1 >> 49) - (int)(e2 >> 49)
	if (scale >= 20) {	// 10^(e1-e2) does not fit in 64 bits, but C1 does
	  __mul_128x64_to_128 (C1, C1_lo, bid_ten2k128[scale - 20]);
	} else if (scale >= 1) {
	  // if 1 <= scale <= 19 then 10^(e1-e2) fits in 64 bits
	  if (q1 <= 19) {	// C1 fits in 64 bits
	    __mul_64x64_to_128MACH (C1, C1_lo, bid_ten2k64[scale]);
	  } else {	// q1 >= 20
	    C1.w[1] = C1_hi;
	    C1.w[0] = C1_lo;
	    __mul_128x64_to_128 (C1, bid_ten2k64[scale], C1);
	  }
	} else {	// if (scale == 0) C1 is unchanged
	  C1.w[1] = C1_hi;
	  C1.w[0] = C1_lo;	// only the low part is necessary
	}
	C1_hi = C1.w[1];
	C1_lo = C1.w[0];
	// now add C2
	if (x_sign == y_sign) {
	  // the result can overflow!
	  C1_lo = C1_lo + C2_lo;
	  C1_hi = C1_hi + C2_hi;
	  if (C1_lo < C1.w[0])
	    C1_hi++;
	  // test for overflow, possible only when C1 >= 10^34
	  if (C1_hi > 0x0001ed09bead87c0ull || (C1_hi == 0x0001ed09bead87c0ull && C1_lo >= 0x378d8e6400000000ull)) {	// C1 >= 10^34
	    // in this case q = P34 + 1 and x = q - P34 = 1, so multiply 
	    // C'' = C'+ 5 = C1 + 5 by k1 ~ 10^(-1) calculated for P34 + 1 
	    // decimal digits
	    // Calculate C'' = C' + 1/2 * 10^x
	    if (C1_lo >= 0xfffffffffffffffbull) {	// low half add has carry
	      C1_lo = C1_lo + 5;
	      C1_hi = C1_hi + 1;
	    } else {
	      C1_lo = C1_lo + 5;
	    }
	    // the approximation of 10^(-1) was rounded up to 118 bits
	    // 10^(-1) =~ 33333333333333333333333333333400 * 2^-129
	    // 10^(-1) =~ 19999999999999999999999999999a00 * 2^-128
	    C1.w[1] = C1_hi;
	    C1.w[0] = C1_lo;	// C''
	    ten2m1.w[1] = 0x1999999999999999ull;
	    ten2m1.w[0] = 0x9999999999999a00ull;
	    __mul_128x128_to_256 (P256, C1, ten2m1);	// P256 = C*, f*
	    // C* is actually floor(C*) in this case
	    // the top Ex = 128 bits of 10^(-1) are 
	    // T* = 0x00199999999999999999999999999999
	    // if (0 < f* < 10^(-x)) then
	    //   if floor(C*) is even then C = floor(C*) - logical right 
	    //       shift; C has p decimal digits, correct by Prop. 1)
	    //   else if floor(C*) is odd C = floor(C*) - 1 (logical right
	    //       shift; C has p decimal digits, correct by Pr. 1)
	    // else
	    //   C = floor(C*) (logical right shift; C has p decimal digits,
	    //       correct by Property 1)
	    // n = C * 10^(e2+x)
	    if ((P256.w[1] || P256.w[0])
		&& (P256.w[1] < 0x1999999999999999ull
		    || (P256.w[1] == 0x1999999999999999ull
			&& P256.w[0] <= 0x9999999999999999ull))) {
	      // the result is a midpoint
	      if (P256.w[2] & 0x01) {
		is_midpoint_gt_even = 1;
		// if floor(C*) is odd C = floor(C*) - 1; the result is not 0
		P256.w[2]--;
		if (P256.w[2] == 0xffffffffffffffffull)
		  P256.w[3]--;
	      } else {
		is_midpoint_lt_even = 1;
	      }
	    }
	    // n = Cstar * 10^(e2+1)
	    y_exp = y_exp + EXP_P1;
	    // C* != 10^P34 because C* has P34 digits
	    // check for overflow
	    if (y_exp == EXP_MAX_P1
		&& (rnd_mode == BID_ROUNDING_TO_NEAREST
		    || rnd_mode == BID_ROUNDING_TIES_AWAY)) {
	      // overflow for RN
	      res.w[1] = x_sign | 0x7800000000000000ull;	// +/-inf
	      res.w[0] = 0x0ull;
	      // set the inexact flag
	      *pfpsf |= BID_INEXACT_EXCEPTION;
	      // set the overflow flag
	      *pfpsf |= BID_OVERFLOW_EXCEPTION;
	      BID_SWAP128 (res);
	      BID_RETURN (res);
	    }
	    // if (0 < f* - 1/2 < 10^(-x)) then 
	    //   the result of the addition is exact 
	    // else 
	    //   the result of the addition is inexact
	    if (P256.w[1] > 0x8000000000000000ull || (P256.w[1] == 0x8000000000000000ull && P256.w[0] > 0x0ull)) {	// the result may be exact
	      tmp64 = P256.w[1] - 0x8000000000000000ull;	// f* - 1/2
	      if ((tmp64 > 0x1999999999999999ull
		   || (tmp64 == 0x1999999999999999ull
		       && P256.w[0] >= 0x9999999999999999ull))) {
		// set the inexact flag
		*pfpsf |= BID_INEXACT_EXCEPTION;
		is_inexact = 1;
	      }	// else the result is exact
	    } else {	// the result is inexact
	      // set the inexact flag
	      *pfpsf |= BID_INEXACT_EXCEPTION;
	      is_inexact = 1;
	    }
	    C1_hi = P256.w[3];
	    C1_lo = P256.w[2];
	    if (!is_midpoint_gt_even && !is_midpoint_lt_even) {
	      is_inexact_lt_midpoint = is_inexact
		&& (P256.w[1] & 0x8000000000000000ull);
	      is_inexact_gt_midpoint = is_inexact
		&& !(P256.w[1] & 0x8000000000000000ull);
	    }
	    // general correction from RN to RA, RM, RP, RZ; result uses y_exp
	    if (rnd_mode != BID_ROUNDING_TO_NEAREST) {
	      if ((!x_sign
		   && ((rnd_mode == BID_ROUNDING_UP
			&& is_inexact_lt_midpoint)
		       || ((rnd_mode == BID_ROUNDING_TIES_AWAY
			    || rnd_mode == BID_ROUNDING_UP)
			   && is_midpoint_gt_even))) || (x_sign
							 &&
							 ((rnd_mode ==
							   BID_ROUNDING_DOWN
							   &&
							   is_inexact_lt_midpoint)
							  ||
							  ((rnd_mode ==
							    BID_ROUNDING_TIES_AWAY
							    || rnd_mode
							    ==
							    BID_ROUNDING_DOWN)
							   &&
							   is_midpoint_gt_even))))
	      {
		// C1 = C1 + 1
		C1_lo = C1_lo + 1;
		if (C1_lo == 0) {	// rounding overflow in the low 64 bits
		  C1_hi = C1_hi + 1;
		}
		if (C1_hi == 0x0001ed09bead87c0ull
		    && C1_lo == 0x378d8e6400000000ull) {
		  // C1 = 10^34 => rounding overflow
		  C1_hi = 0x0000314dc6448d93ull;
		  C1_lo = 0x38c15b0a00000000ull;	// 10^33
		  y_exp = y_exp + EXP_P1;
		}
	      } else
		if ((is_midpoint_lt_even || is_inexact_gt_midpoint) &&
		    ((x_sign && (rnd_mode == BID_ROUNDING_UP ||
				 rnd_mode == BID_ROUNDING_TO_ZERO)) ||
		     (!x_sign && (rnd_mode == BID_ROUNDING_DOWN ||
				  rnd_mode == BID_ROUNDING_TO_ZERO)))) {
		// C1 = C1 - 1
		C1_lo = C1_lo - 1;
		if (C1_lo == 0xffffffffffffffffull)
		  C1_hi--;
		// check if we crossed into the lower decade
		if (C1_hi == 0x0000314dc6448d93ull && C1_lo == 0x38c15b09ffffffffull) {	// 10^33 - 1
		  C1_hi = 0x0001ed09bead87c0ull;	// 10^34 - 1
		  C1_lo = 0x378d8e63ffffffffull;
		  y_exp = y_exp - EXP_P1;
		  // no underflow, because delta + q2 >= P34 + 1
		}
	      } else {
		;	// exact, the result is already correct
	      }
	      // in all cases check for overflow (RN and RA solved already)
	      if (y_exp == EXP_MAX_P1) {	// overflow
		if ((rnd_mode == BID_ROUNDING_DOWN && x_sign) ||	// RM and res < 0
		    (rnd_mode == BID_ROUNDING_UP && !x_sign)) {	// RP and res > 0
		  C1_hi = 0x7800000000000000ull;	// +inf
		  C1_lo = 0x0ull;
		} else {	// RM and res > 0, RP and res < 0, or RZ
		  C1_hi = 0x5fffed09bead87c0ull;
		  C1_lo = 0x378d8e63ffffffffull;
		}
		y_exp = 0;	// x_sign is preserved
		// set the inexact flag (in case the exact addition was exact)
		*pfpsf |= BID_INEXACT_EXCEPTION;
		// set the overflow flag
		*pfpsf |= BID_OVERFLOW_EXCEPTION;
	      }
	    }
	  }	// else if (C1 < 10^34) then C1 is the coeff.; the result is exact
	  // assemble the result
	  res.w[1] = x_sign | y_exp | C1_hi;
	  res.w[0] = C1_lo;
	} else {	// if x_sign != y_sign the result is exact
	  C1_lo = C2_lo - C1_lo;
	  C1_hi = C2_hi - C1_hi;
	  if (C1_lo > C2_lo)
	    C1_hi--;
	  if (C1_hi >= 0x8000000000000000ull) {	// negative coefficient!
	    C1_lo = ~C1_lo;
	    C1_lo++;
	    C1_hi = ~C1_hi;
	    if (C1_lo == 0x0)
	      C1_hi++;
	    x_sign = y_sign;	// the result will have the sign of y
	  }
	  // the result can be zero, but it cannot overflow
	  if (C1_lo == 0 && C1_hi == 0) {
	    // assemble the result
	    if (x_exp < y_exp)
	      res.w[1] = x_exp;
	    else
	      res.w[1] = y_exp;
	    res.w[0] = 0;
	    if (rnd_mode == BID_ROUNDING_DOWN) {
	      res.w[1] |= 0x8000000000000000ull;
	    }
	    BID_SWAP128 (res);
	    BID_RETURN (res);
	  }
	  // assemble the result
	  res.w[1] = y_sign | y_exp | C1_hi;
	  res.w[0] = C1_lo;
	}
      }
    }
    BID_SWAP128 (res);
    BID_RETURN (res)
  }
}



// bid128_sub stands for bid128qq_sub

/*****************************************************************************
 *  BID128 sub
 ****************************************************************************/

#if DECIMAL_CALL_BY_REFERENCE
void
bid128_sub (BID_UINT128 * pres, BID_UINT128 * px, BID_UINT128 * py
	    _RND_MODE_PARAM _EXC_FLAGS_PARAM _EXC_MASKS_PARAM
	    _EXC_INFO_PARAM) {
  BID_UINT128 x = *px, y = *py;
#if !DECIMAL_GLOBAL_ROUNDING
  unsigned int rnd_mode = *prnd_mode;
#endif
#else
DFP_WRAPFN_DFP_DFP(128, bid128_sub, 128, 128)
BID_UINT128
bid128_sub (BID_UINT128 x, BID_UINT128 y
	    _RND_MODE_PARAM _EXC_FLAGS_PARAM _EXC_MASKS_PARAM
	    _EXC_INFO_PARAM) {
#endif

  BID_UINT128 res;
  BID_UINT64 y_sign;

  if ((y.w[BID_HIGH_128W] & MASK_NAN) != MASK_NAN) {	// y is not NAN
    // change its sign
    y_sign = y.w[BID_HIGH_128W] & MASK_SIGN;	// 0 for positive, MASK_SIGN for negative
    if (y_sign)
      y.w[BID_HIGH_128W] = y.w[BID_HIGH_128W] & 0x7fffffffffffffffull;
    else
      y.w[BID_HIGH_128W] = y.w[BID_HIGH_128W] | 0x8000000000000000ull;
  }
#if DECIMAL_CALL_BY_REFERENCE
  bid128_add (&res, &x, &y
	      _RND_MODE_ARG _EXC_FLAGS_ARG _EXC_MASKS_ARG
	      _EXC_INFO_ARG);
#else
  res = bid128_add (x, y
		    _RND_MODE_ARG _EXC_FLAGS_ARG _EXC_MASKS_ARG
		    _EXC_INFO_ARG);
#endif
  BID_RETURN (res);
}
