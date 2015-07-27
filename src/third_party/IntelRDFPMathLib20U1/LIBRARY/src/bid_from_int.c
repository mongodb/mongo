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
 *  BID64_round_integral_exact
 ****************************************************************************/

#if DECIMAL_CALL_BY_REFERENCE
void
bid64_from_int32 (BID_UINT64 * pres,
		  int *px _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
  int x = *px;
#else
DFP_WRAPFN_OTHERTYPE(64, bid64_from_int32, int)
BID_UINT64
bid64_from_int32 (int x _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
#endif
  BID_UINT64 res;

  // if integer is negative, put the absolute value
  // in the lowest 32bits of the result
  if ((x & SIGNMASK32) == SIGNMASK32) {
    // negative int32
    x = ~x + 1;	// 2's complement of x
    res = (unsigned int) x | 0xb1c0000000000000ull;
    // (exp << 53)) = biased exp. is 0
  } else {	// positive int32
    res = x | 0x31c0000000000000ull;	// (exp << 53)) = biased exp. is 0
  }
  BID_RETURN (res);
}

#if DECIMAL_CALL_BY_REFERENCE
void
bid64_from_uint32 (BID_UINT64 * pres, unsigned int *px
		   _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
  unsigned int x = *px;
#else
DFP_WRAPFN_OTHERTYPE(64, bid64_from_uint32, unsigned int)
BID_UINT64
bid64_from_uint32 (unsigned int x _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
#endif
  BID_UINT64 res;

  res = x | 0x31c0000000000000ull;	// (exp << 53)) = biased exp. is 0
  BID_RETURN (res);
}

#if DECIMAL_CALL_BY_REFERENCE
void
bid64_from_int64 (BID_UINT64 * pres, BID_SINT64 * px
		  _RND_MODE_PARAM _EXC_FLAGS_PARAM _EXC_MASKS_PARAM
		  _EXC_INFO_PARAM) {
  BID_SINT64 x = *px;
#if !DECIMAL_GLOBAL_ROUNDING
  unsigned int rnd_mode = *prnd_mode;
#endif
#else
DFP_WRAPFN_OTHERTYPE(64, bid64_from_int64, BID_SINT64)
BID_UINT64
bid64_from_int64 (BID_SINT64 x
		  _RND_MODE_PARAM _EXC_FLAGS_PARAM _EXC_MASKS_PARAM
		  _EXC_INFO_PARAM) {
#endif

  BID_UINT64 res;
  BID_UINT64 x_sign, C;
  unsigned int q, ind;
  int incr_exp = 0;
  int is_midpoint_lt_even = 0, is_midpoint_gt_even = 0;
  int is_inexact_lt_midpoint = 0, is_inexact_gt_midpoint = 0;

  x_sign = x & 0x8000000000000000ull;
  // if the integer is negative, use the absolute value
  if (x_sign)
    C = ~((BID_UINT64) x) + 1;
  else
    C = x;
  if (C <= BID64_SIG_MAX) {	// |C| <= 10^16-1 and the result is exact
    if (C < 0x0020000000000000ull) {	// C < 2^53
      res = x_sign | 0x31c0000000000000ull | C;
    } else {	// C >= 2^53
      res =
	x_sign | 0x6c70000000000000ull | (C & 0x0007ffffffffffffull);
    }
  } else {	// |C| >= 10^16 and the result may be inexact 
    // the smallest |C| is 10^16 which has 17 decimal digits
    // the largest |C| is 0x8000000000000000 = 9223372036854775808 w/ 19 digits
    if (C < 0x16345785d8a0000ull) {	// x < 10^17 
      q = 17;
      ind = 1;	// number of digits to remove for q = 17
    } else if (C < 0xde0b6b3a7640000ull) {	// C < 10^18
      q = 18;
      ind = 2;	// number of digits to remove for q = 18 
    } else {	// C < 10^19
      q = 19;
      ind = 3;	// number of digits to remove for q = 19
    }
    // overflow and underflow are not possible
    // Note: performance can be improved by inlining this call
    bid_round64_2_18 (	// will work for 19 digits too if C fits in 64 bits
		   q, ind, C, &res, &incr_exp,
		   &is_midpoint_lt_even, &is_midpoint_gt_even,
		   &is_inexact_lt_midpoint, &is_inexact_gt_midpoint);
    if (incr_exp)
      ind++;
    // set the inexact flag
    if (is_inexact_lt_midpoint || is_inexact_gt_midpoint ||
	is_midpoint_lt_even || is_midpoint_gt_even)
      *pfpsf |= BID_INEXACT_EXCEPTION;
    // general correction from RN to RA, RM, RP, RZ; result uses ind for exp
    if (rnd_mode != BID_ROUNDING_TO_NEAREST) {
      if ((!x_sign
	   && ((rnd_mode == BID_ROUNDING_UP && is_inexact_lt_midpoint)
	       ||
	       ((rnd_mode == BID_ROUNDING_TIES_AWAY
		 || rnd_mode == BID_ROUNDING_UP) && is_midpoint_gt_even)))
	  || (x_sign
	      && ((rnd_mode == BID_ROUNDING_DOWN && is_inexact_lt_midpoint)
		  ||
		  ((rnd_mode == BID_ROUNDING_TIES_AWAY
		    || rnd_mode == BID_ROUNDING_DOWN)
		   && is_midpoint_gt_even)))) {
	res = res + 1;
	if (res == 0x002386f26fc10000ull) {	// res = 10^16 => rounding overflow
	  res = 0x00038d7ea4c68000ull;	// 10^15
	  ind = ind + 1;
	}
      } else if ((is_midpoint_lt_even || is_inexact_gt_midpoint) &&
		 ((x_sign && (rnd_mode == BID_ROUNDING_UP ||
			      rnd_mode == BID_ROUNDING_TO_ZERO)) ||
		  (!x_sign && (rnd_mode == BID_ROUNDING_DOWN ||
			       rnd_mode == BID_ROUNDING_TO_ZERO)))) {
	res = res - 1;
	// check if we crossed into the lower decade
	if (res == 0x00038d7ea4c67fffull) {	// 10^15 - 1
	  res = 0x002386f26fc0ffffull;	// 10^16 - 1
	  ind = ind - 1;
	}
      } else {
	;	// exact, the result is already correct
      }
    }
    if (res < 0x0020000000000000ull) {	// res < 2^53
      res = x_sign | (((BID_UINT64) ind + 398) << 53) | res;
    } else {	// res >= 2^53 
      res =
	x_sign | 0x6000000000000000ull | (((BID_UINT64) ind + 398) << 51) |
	(res & 0x0007ffffffffffffull);
    }
  }
  BID_RETURN (res);
}

#if DECIMAL_CALL_BY_REFERENCE
void
bid64_from_uint64 (BID_UINT64 * pres, BID_UINT64 * px
		   _RND_MODE_PARAM _EXC_FLAGS_PARAM _EXC_MASKS_PARAM
		   _EXC_INFO_PARAM) {
  BID_UINT64 x = *px;
#if !DECIMAL_GLOBAL_ROUNDING
  unsigned int rnd_mode = *prnd_mode;
#endif
#else
DFP_WRAPFN_OTHERTYPE(64, bid64_from_uint64, BID_UINT64)
BID_UINT64
bid64_from_uint64 (BID_UINT64 x
		   _RND_MODE_PARAM _EXC_FLAGS_PARAM _EXC_MASKS_PARAM
		   _EXC_INFO_PARAM) {
#endif

  BID_UINT64 res;
  BID_UINT128 x128, res128;
  unsigned int q, ind;
  int incr_exp = 0;
  int is_midpoint_lt_even = 0, is_midpoint_gt_even = 0;
  int is_inexact_lt_midpoint = 0, is_inexact_gt_midpoint = 0;

  if (x <= BID64_SIG_MAX) {	// x <= 10^16-1 and the result is exact
    if (x < 0x0020000000000000ull) {	// x < 2^53
      res = 0x31c0000000000000ull | x;
    } else {	// x >= 2^53
      res = 0x6c70000000000000ull | (x & 0x0007ffffffffffffull);
    }
  } else {	// x >= 10^16 and the result may be inexact 
    // the smallest x is 10^16 which has 17 decimal digits
    // the largest x is 0xffffffffffffffff = 18446744073709551615 w/ 20 digits
    if (x < 0x16345785d8a0000ull) {	// x < 10^17 
      q = 17;
      ind = 1;	// number of digits to remove for q = 17
    } else if (x < 0xde0b6b3a7640000ull) {	// x < 10^18
      q = 18;
      ind = 2;	// number of digits to remove for q = 18 
    } else if (x < 0x8ac7230489e80000ull) {	// x < 10^19
      q = 19;
      ind = 3;	// number of digits to remove for q = 19
    } else {	// x < 10^20
      q = 20;
      ind = 4;	// number of digits to remove for q = 20
    }
    // overflow and underflow are not possible
    // Note: performance can be improved by inlining this call
    if (q <= 19) {
      bid_round64_2_18 (	// will work for 20 digits too if x fits in 64 bits
		     q, ind, x, &res, &incr_exp,
		     &is_midpoint_lt_even, &is_midpoint_gt_even,
		     &is_inexact_lt_midpoint, &is_inexact_gt_midpoint);
    } else {	// q = 20
      x128.w[1] = 0x0;
      x128.w[0] = x;
      bid_round128_19_38 (q, ind, x128, &res128, &incr_exp,
		      &is_midpoint_lt_even, &is_midpoint_gt_even,
		      &is_inexact_lt_midpoint, &is_inexact_gt_midpoint);
      res = res128.w[0];	// res.w[1] is 0
    }
    if (incr_exp)
      ind++;
    // set the inexact flag
    if (is_inexact_lt_midpoint || is_inexact_gt_midpoint ||
	is_midpoint_lt_even || is_midpoint_gt_even)
      *pfpsf |= BID_INEXACT_EXCEPTION;
    // general correction from RN to RA, RM, RP, RZ; result uses ind for exp
    if (rnd_mode != BID_ROUNDING_TO_NEAREST) {
      if ((rnd_mode == BID_ROUNDING_UP && is_inexact_lt_midpoint) ||
	  ((rnd_mode == BID_ROUNDING_TIES_AWAY || rnd_mode == BID_ROUNDING_UP)
	   && is_midpoint_gt_even)) {
	res = res + 1;
	if (res == 0x002386f26fc10000ull) {	// res = 10^16 => rounding overflow
	  res = 0x00038d7ea4c68000ull;	// 10^15
	  ind = ind + 1;
	}
      } else if ((is_midpoint_lt_even || is_inexact_gt_midpoint) &&
		 (rnd_mode == BID_ROUNDING_DOWN ||
		  rnd_mode == BID_ROUNDING_TO_ZERO)) {
	res = res - 1;
	// check if we crossed into the lower decade
	if (res == 0x00038d7ea4c67fffull) {	// 10^15 - 1
	  res = 0x002386f26fc0ffffull;	// 10^16 - 1
	  ind = ind - 1;
	}
      } else {
	;	// exact, the result is already correct
      }
    }
    if (res < 0x0020000000000000ull) {	// res < 2^53
      res = (((BID_UINT64) ind + 398) << 53) | res;
    } else {	// res >= 2^53 
      res = 0x6000000000000000ull | (((BID_UINT64) ind + 398) << 51) |
	(res & 0x0007ffffffffffffull);
    }
  }
  BID_RETURN (res);
}

#if DECIMAL_CALL_BY_REFERENCE
void
bid128_from_int32 (BID_UINT128 * pres,
		   int *px _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
  int x = *px;
#else
DFP_WRAPFN_OTHERTYPE(128, bid128_from_int32, int)
BID_UINT128
bid128_from_int32 (int x _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
#endif
  BID_UINT128 res;

  // if integer is negative, use the absolute value
  if ((x & SIGNMASK32) == SIGNMASK32) {
    res.w[BID_HIGH_128W] = 0xb040000000000000ull;
    res.w[BID_LOW_128W] = ~((unsigned int) x) + 1;	// 2's complement of x
  } else {
    res.w[BID_HIGH_128W] = 0x3040000000000000ull;
    res.w[BID_LOW_128W] = (unsigned int) x;
  }
  BID_RETURN (res);
}

#if DECIMAL_CALL_BY_REFERENCE
void
bid128_from_uint32 (BID_UINT128 * pres, unsigned int *px
		    _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
  unsigned int x = *px;
#else
DFP_WRAPFN_OTHERTYPE(128, bid128_from_uint32, unsigned int)
BID_UINT128
bid128_from_uint32 (unsigned int x _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
#endif
  BID_UINT128 res;

  res.w[BID_HIGH_128W] = 0x3040000000000000ull;
  res.w[BID_LOW_128W] = x;
  BID_RETURN (res);
}

#if DECIMAL_CALL_BY_REFERENCE
void
bid128_from_int64 (BID_UINT128 * pres, BID_SINT64 * px
		   _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
  BID_SINT64 x = *px;
#else
DFP_WRAPFN_OTHERTYPE(128, bid128_from_int64, BID_SINT64)
BID_UINT128
bid128_from_int64 (BID_SINT64 x _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
#endif

  BID_UINT128 res;

  // if integer is negative, use the absolute value
  if ((x & SIGNMASK64) == SIGNMASK64) {
    res.w[BID_HIGH_128W] = 0xb040000000000000ull;
    res.w[BID_LOW_128W] = ~x + 1;	// 2's complement of x
  } else {
    res.w[BID_HIGH_128W] = 0x3040000000000000ull;
    res.w[BID_LOW_128W] = x;
  }
  BID_RETURN (res);
}

#if DECIMAL_CALL_BY_REFERENCE
void
bid128_from_uint64 (BID_UINT128 * pres, BID_UINT64 * px
		    _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
  BID_UINT64 x = *px;
#else
DFP_WRAPFN_OTHERTYPE(128, bid128_from_uint64, BID_UINT64)
BID_UINT128
bid128_from_uint64 (BID_UINT64 x _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
#endif

  BID_UINT128 res;

  res.w[BID_HIGH_128W] = 0x3040000000000000ull;
  res.w[BID_LOW_128W] = x;
  BID_RETURN (res);
}


#if DECIMAL_CALL_BY_REFERENCE
void
bid32_from_int32 (BID_UINT32 * pres, BID_SINT32 * px
    _RND_MODE_PARAM _EXC_FLAGS_PARAM _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
  BID_SINT32 x = *px;
#if !DECIMAL_GLOBAL_ROUNDING
  unsigned int rnd_mode = *prnd_mode;
#endif
#else
DFP_WRAPFN_OTHERTYPE(32, bid32_from_int32, BID_SINT32)
BID_UINT32
bid32_from_int32 (BID_SINT32 x
    _RND_MODE_PARAM _EXC_FLAGS_PARAM _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
#endif

  BID_UINT32 res;
  BID_UINT64 res64;
  BID_UINT32 x_sign;
  BID_UINT32 C;
  unsigned int q, ind;
  int incr_exp = 0;
  int is_midpoint_lt_even = 0, is_midpoint_gt_even = 0;
  int is_inexact_lt_midpoint = 0, is_inexact_gt_midpoint = 0;

  x_sign = x & MASK_SIGN32;
  // if the integer is negative, use the absolute value
  if (x_sign)
    C = ~((BID_UINT32) x) + 1;
  else
    C = x;
  if (C <= BID32_SIG_MAX) { // |C| <= 10^7-1 and the result is exact
    if (C < 0x00800000) { // C < 2^23
      res = x_sign | 0x32800000 | C;
    } else { // C >= 2^23
      res = x_sign | 0x6ca00000 | (C & 0x001fffff);
    }
  } else { // |C| >= 10^7 and the result may be inexact 
    // the smallest |C| is 10^7 which has 8 decimal digits
    // the largest |C| is 0x80000000 = 2147483648 w/ 10 digits
    if (C < 0x05f5e100) { // x < 10^8 
      q = 8;
      ind = 1;	// number of digits to remove for q = 8
    } else if (C < 0x3b9aca00) { // C < 10^9
      q = 9;
      ind = 2;	// number of digits to remove for q = 9
    } else { // C < 10^10
      q = 10;
      ind = 3;	// number of digits to remove for q = 10
    }
    // overflow and underflow are not possible
    // Note: performance can be improved by inlining this call
    bid_round64_2_18 (q, ind, C, &res64, &incr_exp,
		   &is_midpoint_lt_even, &is_midpoint_gt_even,
		   &is_inexact_lt_midpoint, &is_inexact_gt_midpoint);
    res = (BID_UINT32)res64;
    if (incr_exp)
      ind++;
    // set the inexact flag
    if (is_inexact_lt_midpoint || is_inexact_gt_midpoint ||
	is_midpoint_lt_even || is_midpoint_gt_even)
      *pfpsf |= BID_INEXACT_EXCEPTION;
    // general correction from RN to RA, RM, RP, RZ; result uses ind for exp
    if (rnd_mode != BID_ROUNDING_TO_NEAREST) {
      if ((!x_sign
	   && ((rnd_mode == BID_ROUNDING_UP && is_inexact_lt_midpoint)
	       ||
	       ((rnd_mode == BID_ROUNDING_TIES_AWAY
		 || rnd_mode == BID_ROUNDING_UP) && is_midpoint_gt_even)))
	  || (x_sign
	      && ((rnd_mode == BID_ROUNDING_DOWN && is_inexact_lt_midpoint)
		  ||
		  ((rnd_mode == BID_ROUNDING_TIES_AWAY
		    || rnd_mode == BID_ROUNDING_DOWN)
		   && is_midpoint_gt_even)))) {
	res = res + 1;
	if (res == 10000000) { // res = 10^7 => rounding overflow
	  res = 1000000; // 10^6
	  ind = ind + 1;
	}
      } else if ((is_midpoint_lt_even || is_inexact_gt_midpoint) &&
		 ((x_sign && (rnd_mode == BID_ROUNDING_UP ||
			      rnd_mode == BID_ROUNDING_TO_ZERO)) ||
		  (!x_sign && (rnd_mode == BID_ROUNDING_DOWN ||
			       rnd_mode == BID_ROUNDING_TO_ZERO)))) {
	res = res - 1;
	// check if we crossed into the lower decade
	if (res == 999999) { // 10^6 - 1
	  res = 9999999; // 10^7 - 1
	  ind = ind - 1;
	}
      } else {
	; // exact, the result is already correct
      }
    }
    if (res < 0x00800000) { // res < 2^23
      res = x_sign | ((ind + 101) << 23) | res;
    } else { // res >= 2^23 
      res = x_sign | 0x60000000 | ((ind + 101) << 21) | (res & 0x001fffff);
    }
  }
  BID_RETURN (res);
}


#if DECIMAL_CALL_BY_REFERENCE
void
bid32_from_uint32 (BID_UINT32 * pres, BID_UINT32 * px
    _RND_MODE_PARAM _EXC_FLAGS_PARAM _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
  BID_UINT32 x = *px;
#if !DECIMAL_GLOBAL_ROUNDING
  unsigned int rnd_mode = *prnd_mode;
#endif
#else
DFP_WRAPFN_OTHERTYPE(32, bid32_from_uint32, BID_UINT32)
BID_UINT32
bid32_from_uint32 (BID_UINT32 x
    _RND_MODE_PARAM _EXC_FLAGS_PARAM _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
#endif

  BID_UINT32 res;
  BID_UINT64 res64;
  unsigned int q, ind;
  int incr_exp = 0;
  int is_midpoint_lt_even = 0, is_midpoint_gt_even = 0;
  int is_inexact_lt_midpoint = 0, is_inexact_gt_midpoint = 0;

  if (x <= BID32_SIG_MAX) { // x <= 10^7-1 and the result is exact
    if (x < 0x00800000) { // x < 2^23
      res = 0x32800000 | x;
    } else { // x >= 2^23
      res = 0x6ca00000 | (x & 0x001fffff);
    }
  } else { // x >= 10^7 and the result may be inexact 
    // the smallest x is 10^7 which has 8 decimal digits
    // the largest x is 0xffffffff = 4294967295 w/ 10 digits
    if (x < 0x05f5e100) { // x < 10^8
      q = 8;
      ind = 1;  // number of digits to remove for q = 8
    } else if (x < 0x3b9aca00) { // x < 10^9
      q = 9;
      ind = 2;  // number of digits to remove for q = 9
    } else { // x < 10^10
      q = 10;
      ind = 3; // number of digits to remove for q = 10
    }
    // overflow and underflow are not possible
    // Note: performance can be improved by inlining this call
    bid_round64_2_18 ( // would work for 20 digits too if x fits in 64 bits
       q, ind, x, &res64, &incr_exp,
       &is_midpoint_lt_even, &is_midpoint_gt_even,
       &is_inexact_lt_midpoint, &is_inexact_gt_midpoint);
    res = (BID_UINT32)res64;
    if (incr_exp)
      ind++;
    // set the inexact flag
    if (is_inexact_lt_midpoint || is_inexact_gt_midpoint ||
	is_midpoint_lt_even || is_midpoint_gt_even)
      *pfpsf |= BID_INEXACT_EXCEPTION;
    // general correction from RN to RA, RM, RP, RZ; result uses ind for exp
    if (rnd_mode != BID_ROUNDING_TO_NEAREST) {
      if ((rnd_mode == BID_ROUNDING_UP && is_inexact_lt_midpoint) ||
	  ((rnd_mode == BID_ROUNDING_TIES_AWAY || rnd_mode == BID_ROUNDING_UP)
	   && is_midpoint_gt_even)) {
	res = res + 1;
	if (res == 10000000) { // res = 10^7 => rounding overflow
	  res = 1000000; // 10^6
	  ind = ind + 1;
	}
      } else if ((is_midpoint_lt_even || is_inexact_gt_midpoint) &&
		 (rnd_mode == BID_ROUNDING_DOWN ||
		  rnd_mode == BID_ROUNDING_TO_ZERO)) {
	res = res - 1;
	// check if we crossed into the lower decade
	if (res == 999999) { // 10^6 - 1
	  res = 9999999; // 10^7 - 1
	  ind = ind - 1;
	}
      } else {
	; // exact, the result is already correct
      }
    }
    if (res < 0x00800000) { // res < 2^23
      res = ((ind + 101) << 23) | res;
    } else { // res >= 2^23 
      res = 0x60000000 | ((ind + 101) << 21) | (res & 0x001fffff);
    }
  }
  BID_RETURN (res);
}


#if DECIMAL_CALL_BY_REFERENCE
void
bid32_from_int64 (BID_UINT32 * pres, BID_SINT64 * px
    _RND_MODE_PARAM _EXC_FLAGS_PARAM _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
  BID_SINT64 x = *px;
#if !DECIMAL_GLOBAL_ROUNDING
  unsigned int rnd_mode = *prnd_mode;
#endif
#else
DFP_WRAPFN_OTHERTYPE(32, bid32_from_int64, BID_SINT64)
BID_UINT32
bid32_from_int64 (BID_SINT64 x
    _RND_MODE_PARAM _EXC_FLAGS_PARAM _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
#endif

  BID_UINT32 res;
  BID_UINT32 x_sign32;
  BID_UINT64 x_sign;
  BID_UINT64 C, res64;
  unsigned int q, ind;
  int incr_exp = 0;
  int is_midpoint_lt_even = 0, is_midpoint_gt_even = 0;
  int is_inexact_lt_midpoint = 0, is_inexact_gt_midpoint = 0;

  x_sign = x & 0x8000000000000000ull;
  x_sign32 = (x_sign ? 0x80000000 : 0x00000000);
  // if the integer is negative, use the absolute value
  if (x_sign)
    C = ~((BID_UINT64) x) + 1;
  else
    C = x;
  if (C <= (BID_UINT64)BID32_SIG_MAX) { // |C| <= 10^7-1 and the result is exact
    if (C < (BID_UINT64)0x00800000) { // C < 2^23
      res = x_sign32 | 0x32800000 | (BID_UINT32)(C & 0x007fffff);
    } else { // C >= 2^23
      res = x_sign32 | 0x6ca00000 | (BID_UINT32)(C & 0x001fffff);
    }
  } else { // |C| >= 10^7 and the result may be inexact 
    // the smallest |C| is 10^7 which has 8 decimal digits
    // the largest |C| is 0x8000000000000000 = 9223372036854775808 w/ 19 digits
    if (C < (BID_UINT64)0x05f5e100) { // x < 10^8 
      q = 8;
      ind = 1;	// number of digits to remove for q = 8
    } else if (C < (BID_UINT64)0x3b9aca00) { // C < 10^9
      q = 9;
      ind = 2;	// number of digits to remove for q = 9
    } else if (C < 10000000000ull) { // C < 10^10
      q = 10;
      ind = 3;  // number of digits to remove for q = 10
    } else if (C < 100000000000ull) { // C < 10^11
      q = 11;
      ind = 4;  // number of digits to remove for q = 11
    } else if (C < 1000000000000ull) { // C < 10^12
      q = 12;
      ind = 5;  // number of digits to remove for q = 12
    } else if (C < 10000000000000ull) { // C < 10^13
      q = 13;
      ind = 6;  // number of digits to remove for q = 13
    } else if (C < 100000000000000ull) { // C < 10^14
      q = 14;
      ind = 7;  // number of digits to remove for q = 14
    } else if (C < 1000000000000000ull) { // C < 10^15
      q = 15;
      ind = 8;  // number of digits to remove for q = 15
    } else if (C < 10000000000000000ull) { // C < 10^16
      q = 16;
      ind = 9;  // number of digits to remove for q = 16
    } else if (C < 100000000000000000ull) { // C < 10^17
      q = 17;
      ind = 10;  // number of digits to remove for q = 17
    } else if (C < 1000000000000000000ull) { // C < 10^18
      q = 18;
      ind = 11;  // number of digits to remove for q = 18
    } else { // C < 10^19
      q = 19;
      ind = 12;	// number of digits to remove for q = 19
    }
    // overflow and underflow are not possible
    // Note: performance can be improved by inlining this call
    bid_round64_2_18 ( // will work for 19 digits too if C fits in 64 bits
		   q, ind, C, &res64, &incr_exp,
		   &is_midpoint_lt_even, &is_midpoint_gt_even,
		   &is_inexact_lt_midpoint, &is_inexact_gt_midpoint);
    res = (BID_UINT32)res64;
    if (incr_exp)
      ind++;
    // set the inexact flag
    if (is_inexact_lt_midpoint || is_inexact_gt_midpoint ||
	is_midpoint_lt_even || is_midpoint_gt_even)
      *pfpsf |= BID_INEXACT_EXCEPTION;
    // general correction from RN to RA, RM, RP, RZ; result uses ind for exp
    if (rnd_mode != BID_ROUNDING_TO_NEAREST) {
      if ((!x_sign
	   && ((rnd_mode == BID_ROUNDING_UP && is_inexact_lt_midpoint)
	       ||
	       ((rnd_mode == BID_ROUNDING_TIES_AWAY
		 || rnd_mode == BID_ROUNDING_UP) && is_midpoint_gt_even)))
	  || (x_sign
	      && ((rnd_mode == BID_ROUNDING_DOWN && is_inexact_lt_midpoint)
		  ||
		  ((rnd_mode == BID_ROUNDING_TIES_AWAY
		    || rnd_mode == BID_ROUNDING_DOWN)
		   && is_midpoint_gt_even)))) {
	res = res + 1;
	if (res == 10000000) { // res = 10^7 => rounding overflow
	  res = 1000000; // 10^6
	  ind = ind + 1;
	}
      } else if ((is_midpoint_lt_even || is_inexact_gt_midpoint) &&
		 ((x_sign && (rnd_mode == BID_ROUNDING_UP ||
			      rnd_mode == BID_ROUNDING_TO_ZERO)) ||
		  (!x_sign && (rnd_mode == BID_ROUNDING_DOWN ||
			       rnd_mode == BID_ROUNDING_TO_ZERO)))) {
	res = res - 1;
	// check if we crossed into the lower decade
	if (res == 999999) { // 10^6 - 1
	  res = 9999999; // 10^7 - 1
	  ind = ind - 1;
	}
      } else {
	; // exact, the result is already correct
      }
    }
    if (res < 0x00800000) { // res < 2^23
      res = x_sign32 | ((ind + 101) << 23) | res;
    } else { // res >= 2^23 
      res = x_sign32 | 0x60000000 | ((ind + 101) << 21) | (res & 0x001fffff);
    }
  }
  BID_RETURN (res);
}


#if DECIMAL_CALL_BY_REFERENCE
void
bid32_from_uint64 (BID_UINT32 * pres, BID_UINT64 * px
    _RND_MODE_PARAM _EXC_FLAGS_PARAM _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
  BID_UINT64 x = *px;
#if !DECIMAL_GLOBAL_ROUNDING
  unsigned int rnd_mode = *prnd_mode;
#endif
#else
DFP_WRAPFN_OTHERTYPE(32, bid32_from_uint64, BID_UINT64)
BID_UINT32
bid32_from_uint64 (BID_UINT64 x
    _RND_MODE_PARAM _EXC_FLAGS_PARAM _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
#endif

  BID_UINT32 res;
  BID_UINT64 res64;
  BID_UINT128 x128, res128;
  unsigned int q, ind;
  int incr_exp = 0;
  int is_midpoint_lt_even = 0, is_midpoint_gt_even = 0;
  int is_inexact_lt_midpoint = 0, is_inexact_gt_midpoint = 0;

  if (x <= (BID_UINT64)BID32_SIG_MAX) { // x <= 10^7-1 and the result is exact
    if (x < (BID_UINT64)0x00800000) { // x < 2^23
      res = 0x32800000 | (BID_UINT32)(x & 0x007fffff);
    } else { // x >= 2^23
      res = 0x6ca00000 | (BID_UINT32)(x & 0x001fffff);
    }
  } else { // x >= 10^7 and the result may be inexact 
    // the smallest x is 10^7 which has 8 decimal digits
    // the largest x is 0xffffffffffffffff = 18446744073709551615 w/ 20 digits
    if (x < (BID_UINT64)0x05f5e100) { // x < 10^8
      q = 8;
      ind = 1;  // number of digits to remove for q = 8
    } else if (x < (BID_UINT64)0x3b9aca00) { // x < 10^9
      q = 9;
      ind = 2;  // number of digits to remove for q = 9
    } else if (x < 10000000000ull) { // x < 10^10
      q = 10;
      ind = 3;  // number of digits to remove for q = 10
    } else if (x < 100000000000ull) { // x < 10^11
      q = 11;
      ind = 4;  // number of digits to remove for q = 11
    } else if (x < 1000000000000ull) { // x < 10^12
      q = 12;
      ind = 5;  // number of digits to remove for q = 12
    } else if (x < 10000000000000ull) { // x < 10^13
      q = 13;
      ind = 6;  // number of digits to remove for q = 13
    } else if (x < 100000000000000ull) { // x < 10^14
      q = 14;
      ind = 7;  // number of digits to remove for q = 14
    } else if (x < 1000000000000000ull) { // x < 10^15
      q = 15;
      ind = 8;  // number of digits to remove for q = 15
    } else if (x < 10000000000000000ull) { // x < 10^16
      q = 16;
      ind = 9;  // number of digits to remove for q = 16
    } else if (x < 100000000000000000ull) { // x < 10^17
      q = 17;
      ind = 10;  // number of digits to remove for q = 17
    } else if (x < 1000000000000000000ull) { // x < 10^18
      q = 18;
      ind = 11;  // number of digits to remove for q = 18
    } else if (x < 10000000000000000000ull) { // x < 10^19
      q = 19;
      ind = 12;  // number of digits to remove for q = 19
    } else { // x < 10^20
      q = 20;
      ind = 13; // number of digits to remove for q = 20
    }
    // overflow and underflow are not possible
    // Note: performance can be improved by inlining this call
    if (q <= 19) {
      bid_round64_2_18 ( // would work for 20 digits too if x fits in 64 bits
		     q, ind, x, &res64, &incr_exp,
		     &is_midpoint_lt_even, &is_midpoint_gt_even,
		     &is_inexact_lt_midpoint, &is_inexact_gt_midpoint);
      res = (BID_UINT32)res64;
    } else { // q = 20
      x128.w[1] = 0x0;
      x128.w[0] = x;
      bid_round128_19_38 (q, ind, x128, &res128, &incr_exp,
		      &is_midpoint_lt_even, &is_midpoint_gt_even,
		      &is_inexact_lt_midpoint, &is_inexact_gt_midpoint);
      res = (BID_UINT32)res128.w[0]; // res.w[1] is 0
    }
    if (incr_exp)
      ind++;
    // set the inexact flag
    if (is_inexact_lt_midpoint || is_inexact_gt_midpoint ||
	is_midpoint_lt_even || is_midpoint_gt_even)
      *pfpsf |= BID_INEXACT_EXCEPTION;
    // general correction from RN to RA, RM, RP, RZ; result uses ind for exp
    if (rnd_mode != BID_ROUNDING_TO_NEAREST) {
      if ((rnd_mode == BID_ROUNDING_UP && is_inexact_lt_midpoint) ||
	  ((rnd_mode == BID_ROUNDING_TIES_AWAY || rnd_mode == BID_ROUNDING_UP)
	   && is_midpoint_gt_even)) {
	res= res+ 1;
	if (res== 10000000) { // res = 10^7 => rounding overflow
	  res = 1000000; // 10^6
	  ind = ind + 1;
	}
      } else if ((is_midpoint_lt_even || is_inexact_gt_midpoint) &&
		 (rnd_mode == BID_ROUNDING_DOWN ||
		  rnd_mode == BID_ROUNDING_TO_ZERO)) {
	res = res - 1;
	// check if we crossed into the lower decade
	if (res == 999999) { // 10^6 - 1
	  res = 9999999; // 10^7 - 1
	  ind = ind - 1;
	}
      } else {
	; // exact, the result is already correct
      }
    }
    if (res < 0x00800000) { // res < 2^23
      res = ((ind + 101) << 23) | res;
    } else { // res >= 2^23 
      res = 0x60000000 | ((ind + 101) << 21) | (res & 0x001fffff);
    }
  }
  BID_RETURN (res);
}

