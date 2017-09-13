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

#define  DECNUMDIGITS 34	// work with up to 34 digits

#include "bid_internal.h"
#include "bid_b2d.h"

#if DECIMAL_CALL_BY_REFERENCE
void
bid_to_dpd32 (BID_UINT32 * pres, BID_UINT32 * pba) {
  BID_UINT32 ba = *pba;
#else
DFP_WRAPFN_DFP(32, bid_to_dpd32, 32)
BID_UINT32
bid_to_dpd32 (BID_UINT32 ba) {
#endif
  BID_UINT32 res;

  BID_UINT32 sign, comb, exp, trailing;
  BID_UINT32 b0, b1, b2;
  BID_UINT32 bcoeff, dcoeff;
  BID_UINT32 nanb = 0;

  sign = (ba & 0x80000000);
  comb = (ba & 0x7ff00000) >> 20;
  trailing = (ba & 0xfffff);

  // Detect infinity, and return canonical infinity
  if ((comb & 0x7c0) == 0x780) {
    res = sign | 0x78000000;
    BID_RETURN (res);
    // Detect NaN, and canonicalize trailing
  } else if ((comb & 0x7c0) == 0x7c0) {
    if (trailing > 999999)
      trailing = 0;
    nanb = ba & 0xfe000000;
    exp = 0;
    bcoeff = trailing;
  } else {	// Normal number
    if ((comb & 0x600) == 0x600) {	// G0..G1 = 11 -> exp is G2..G11
      exp = (comb >> 1) & 0xff;
      bcoeff = ((8 + (comb & 1)) << 20) | trailing;
    } else {
      exp = (comb >> 3) & 0xff;
      bcoeff = ((comb & 7) << 20) | trailing;
    }
    // Zero the coefficient if non-canonical (>= 10^7)
    if (bcoeff >= 10000000)
      bcoeff = 0;
  }

  b0 = bcoeff / 1000000;
  b1 = (bcoeff / 1000) % 1000;
  b2 = bcoeff % 1000;
  dcoeff = (bid_b2d[b1] << 10) | bid_b2d[b2];

  if (b0 >= 8)	// is b0 8 or 9?
    res =
      sign |
      ((0x600 | ((exp >> 6) << 7) | ((b0 & 1) << 6) | (exp & 0x3f)) <<
       20) | dcoeff;
  else	// else b0 is 0..7
    res =
      sign | ((((exp >> 6) << 9) | (b0 << 6) | (exp & 0x3f)) << 20) |
      dcoeff;

  res |= nanb;

  BID_RETURN (res);
}

#if DECIMAL_CALL_BY_REFERENCE
void
bid_to_dpd64 (BID_UINT64 * pres, BID_UINT64 * pba) {
  BID_UINT64 ba = *pba;
#else
DFP_WRAPFN_DFP(64, bid_to_dpd64, 64)
BID_UINT64
bid_to_dpd64 (BID_UINT64 ba) {
#endif
  BID_UINT64 res;

  BID_UINT64 sign, comb, exp;
  BID_UINT64 trailing;
  BID_UINT32 b0, b1, b2, b3, b4, b5;
  BID_UINT64 bcoeff;
  BID_UINT64 dcoeff;
  BID_UINT32 yhi, ylo;
  BID_UINT64 nanb = 0;

//printf("arg bid "BID_FMT_LLX16" \n", ba);
  sign = (ba & 0x8000000000000000ull);
  comb = (ba & 0x7ffc000000000000ull) >> 50;
  trailing = (ba & 0x0003ffffffffffffull);

  // Detect infinity, and return canonical infinity
  if ((comb & 0x1f00) == 0x1e00) {
    res = sign | 0x7800000000000000ull;
    BID_RETURN (res);
    // Detect NaN, and canonicalize trailing
  } else if ((comb & 0x1e00) == 0x1e00) {
    if (trailing > 999999999999999ull)
      trailing = 0;
    nanb = ba & 0xfe00000000000000ull;
    exp = 0;
    bcoeff = trailing;
  } else {	// Normal number
    if ((comb & 0x1800) == 0x1800) {	// G0..G1 = 11 -> exp is G2..G11
      exp = (comb >> 1) & 0x3ff;
      bcoeff = ((8 + (comb & 1)) << 50) | trailing;
    } else {
      exp = (comb >> 3) & 0x3ff;
      bcoeff = ((comb & 7) << 50) | trailing;
    }

    // Zero the coefficient if it is non-canonical (>= 10^16)
    if (bcoeff >= 10000000000000000ull)
      bcoeff = 0;
  }

// Floor(2^61 / 10^9)
#define D61 (2305843009ull)

// Multipy the binary coefficient by ceil(2^64 / 1000), and take the upper
// 64-bits in order to compute a division by 1000.

#if 1
  yhi =
    ((BID_UINT64) D61 *
     (BID_UINT64) (BID_UINT32) (bcoeff >> (BID_UINT64) 27)) >> (BID_UINT64) 34;
  ylo = bcoeff - 1000000000ull * yhi;
  if (ylo >= 1000000000) {
    ylo = ylo - 1000000000;
    yhi = yhi + 1;
  }
#else
  yhi = bcoeff / 1000000000ull;
  ylo = bcoeff % 1000000000ull;
#endif

  // yhi = ABBBCCC ylo = DDDEEEFFF
  b5 = ylo % 1000;	// b5 = FFF
  b3 = ylo / 1000000;	// b3 = DDD
  b4 = (ylo / 1000) - (1000 * b3);	// b4 = EEE
  b2 = yhi % 1000;	// b2 = CCC
  b0 = yhi / 1000000;	// b0 = A
  b1 = (yhi / 1000) - (1000 * b0);	// b1 = BBB

  dcoeff = bid_b2d[b5] | bid_b2d2[b4] | bid_b2d3[b3] | bid_b2d4[b2] | bid_b2d5[b1];

  if (b0 >= 8)	// is b0 8 or 9?
    res =
      sign |
      ((0x1800 | ((exp >> 8) << 9) | ((b0 & 1) << 8) | (exp & 0xff)) <<
       50) | dcoeff;
  else	// else b0 is 0..7
    res =
      sign | ((((exp >> 8) << 11) | (b0 << 8) | (exp & 0xff)) << 50) |
      dcoeff;

  res |= nanb;

  BID_RETURN (res);
}

#if DECIMAL_CALL_BY_REFERENCE
void
bid_dpd_to_bid32 (BID_UINT32 * pres, BID_UINT32 * pda) {
  BID_UINT32 da = *pda;
#else
DFP_WRAPFN_DFP(32, bid_dpd_to_bid32, 32)
BID_UINT32
bid_dpd_to_bid32 (BID_UINT32 da) {
#endif
  BID_UINT32 in = *(BID_UINT32 *) & da;
  BID_UINT32 res;

  BID_UINT32 sign, comb, exp;
  BID_UINT32 trailing;
  BID_UINT32 d0 = 0, d1, d2;
  BID_UINT64 bcoeff;
  BID_UINT32 nanb = 0;

  sign = (in & 0x80000000);
  comb = (in & 0x7ff00000) >> 20;
  trailing = (in & 0x000fffff);

  if ((comb & 0x7c0) == 0x780) {	// G0..G4 = 11110 -> Inf
    res = in & 0xf8000000;
    BID_RETURN (res);
  } else if ((comb & 0x7c0) == 0x7c0) {	// G0..G5 = 11111 -> NaN
    nanb = in & 0xfe000000;
    exp = 0;
  } else {	// Normal number
    if ((comb & 0x600) == 0x600) {	// G0..G1 = 11 -> d0 = 8 + G4
      d0 = ((comb >> 6) & 1) | 8;
      exp = ((comb & 0x180) >> 1) | (comb & 0x3f);
    } else {
      d0 = (comb >> 6) & 0x7;
      exp = ((comb & 0x600) >> 3) | (comb & 0x3f);
    }
  }
  d1 = bid_d2b2[(trailing >> 10) & 0x3ff];
  d2 = bid_d2b[(trailing) & 0x3ff];

  bcoeff = d2 + d1 + (1000000 * d0);
  if (bcoeff < 0x800000) {
    res = (exp << 23) | bcoeff | sign;
  } else {
    res = (exp << 21) | sign | 0x60000000 | (bcoeff & 0x1fffff);
  }

  res |= nanb;

  BID_RETURN (res);
}

#if DECIMAL_CALL_BY_REFERENCE
void
bid_dpd_to_bid64 (BID_UINT64 * pres, BID_UINT64 * pda) {
  BID_UINT64 da = *pda;
#else
DFP_WRAPFN_DFP(64, bid_dpd_to_bid64, 64)
BID_UINT64
bid_dpd_to_bid64 (BID_UINT64 da) {
#endif
  BID_UINT64 in = *(BID_UINT64 *) & da;
  BID_UINT64 res;

  BID_UINT64 sign, comb, exp;
  BID_UINT64 trailing;
  // BID_UINT64 d0, d1, d2, d3, d4, d5;

  BID_UINT64 d1, d2;
  BID_UINT32 d0, d3, d4, d5;
  BID_UINT64 bcoeff;
  BID_UINT64 nanb = 0;

//printf("arg dpd "BID_FMT_LLX16" \n", in);
  sign = (in & 0x8000000000000000ull);
  comb = (in & 0x7ffc000000000000ull) >> 50;
  trailing = (in & 0x0003ffffffffffffull);

  if ((comb & 0x1f00) == 0x1e00) {	// G0..G4 = 11110 -> Inf
    res = in & 0xf800000000000000ull;
    BID_RETURN (res);
  } else if ((comb & 0x1f00) == 0x1f00) {	// G0..G5 = 11111 -> NaN
    nanb = in & 0xfe00000000000000ull;
    exp = 0;
    d0 = 0;
  } else {	// Normal number
    if ((comb & 0x1800) == 0x1800) {	// G0..G1 = 11 -> d0 = 8 + G4
      d0 = ((comb >> 8) & 1) | 8;
      // d0 = (comb & 0x0100 ? 9 : 8);
      exp = (comb & 0x600) >> 1;
      // exp = (comb & 0x0400 ? 1 : 0) * 0x200 + (comb & 0x0200 ? 1 : 0) * 0x100; // exp leading bits are G2..G3
    } else {
      d0 = (comb >> 8) & 0x7;
      exp = (comb & 0x1800) >> 3;
      // exp = (comb & 0x1000 ? 1 : 0) * 0x200 + (comb & 0x0800 ? 1 : 0) * 0x100; // exp loading bits are G0..G1
    }
  }
  d1 = bid_d2b5[(trailing >> 40) & 0x3ff];
  d2 = bid_d2b4[(trailing >> 30) & 0x3ff];
  d3 = bid_d2b3[(trailing >> 20) & 0x3ff];
  d4 = bid_d2b2[(trailing >> 10) & 0x3ff];
  d5 = bid_d2b[(trailing) & 0x3ff];

  bcoeff = (d5 + d4 + d3) + d2 + d1 + (1000000000000000ull * d0);
  exp += (comb & 0xff);
  res = very_fast_get_BID64 (sign, exp, bcoeff);

  res |= nanb;

  BID_RETURN (res);
}

#if DECIMAL_CALL_BY_REFERENCE
void
bid_to_dpd128 (BID_UINT128 * pres, BID_UINT128 * pba) {
  BID_UINT128 ba = *pba;
#else
DFP_WRAPFN_DFP(128, bid_to_dpd128, 128)
BID_UINT128
bid_to_dpd128 (BID_UINT128 ba) {
#endif
  BID_UINT128 res;

  BID_UINT128 sign;
  BID_UINT32 comb, exp;
  BID_UINT128 trailing;
  BID_UINT128 d0, d1, d2, d3, d4, d5, d6, d7, d8, d9, d10, d11;
  BID_UINT128 bcoeff;
  BID_UINT128 dcoeff;
  BID_UINT64 nanb = 0;

  sign.w[1] = (ba.w[BID_HIGH_128W] & 0x8000000000000000ull);
  sign.w[0] = 0;
  comb = (ba.w[BID_HIGH_128W] & 0x7fffc00000000000ull) >> 46;
  trailing.w[1] = (ba.w[BID_HIGH_128W] & 0x00003fffffffffffull);
  trailing.w[0] = ba.w[BID_LOW_128W];
  exp = 0;

  if ((comb & 0x1f000) == 0x1e000) {	// G0..G4 = 11110 -> Inf
    res.w[BID_HIGH_128W] = ba.w[BID_HIGH_128W] & 0xf800000000000000ull;
    res.w[BID_LOW_128W] = 0;
    BID_RETURN (res);
    // Detect NaN, and canonicalize trailing
  } else if ((comb & 0x1f000) == 0x1f000) {
    if ((trailing.w[1] > 0x0000314dc6448d93ULL) ||	// significand is non-canonical
	((trailing.w[1] == 0x0000314dc6448d93ULL)
	 && (trailing.w[0] >= 0x38c15b0a00000000ULL))
	// significand is non-canonical
      ) {
      trailing.w[1] = trailing.w[0] = 0ull;
    }
    bcoeff.w[1] = trailing.w[1];
    bcoeff.w[0] = trailing.w[0];
    nanb = ba.w[BID_HIGH_128W] & 0xfe00000000000000ull;
    exp = 0;
  } else {	// Normal number
    if ((comb & 0x18000) == 0x18000) {	// G0..G1 = 11 -> exp is G2..G11
      exp = (comb >> 1) & 0x3fff;
      bcoeff.w[1] =
	((BID_UINT64) (8 + (comb & 1)) << (BID_UINT64) 46) | trailing.w[1];
      bcoeff.w[0] = trailing.w[0];
    } else {
      exp = (comb >> 3) & 0x3fff;
      bcoeff.w[1] =
	((BID_UINT64) (comb & 7) << (BID_UINT64) 46) | trailing.w[1];
      bcoeff.w[0] = trailing.w[0];
    }
    // Zero the coefficient if non-canonical (>= 10^34)
    if (bcoeff.w[1] > 0x1ed09bead87c0ull ||
	(bcoeff.w[1] == 0x1ed09bead87c0ull
	 && bcoeff.w[0] >= 0x378D8E6400000000ull)) {
      bcoeff.w[0] = bcoeff.w[1] = 0;
    }
  }
  // Constant 2^128 / 1000 + 1
  {
    BID_UINT128 t;
    BID_UINT64 t2;
    BID_UINT128 d1000;
    BID_UINT128 b11, b10, b9, b8, b7, b6, b5, b4, b3, b2, b1;
    d1000.w[1] = 0x4189374BC6A7EFull;
    d1000.w[0] = 0x9DB22D0E56041894ull;
    __mul_128x128_high (b11, bcoeff, d1000);
    __mul_128x128_high (b10, b11, d1000);
    __mul_128x128_high (b9, b10, d1000);
    __mul_128x128_high (b8, b9, d1000);
    __mul_128x128_high (b7, b8, d1000);
    __mul_128x128_high (b6, b7, d1000);
    __mul_128x128_high (b5, b6, d1000);
    __mul_128x128_high (b4, b5, d1000);
    __mul_128x128_high (b3, b4, d1000);
    __mul_128x128_high (b2, b3, d1000);
    __mul_128x128_high (b1, b2, d1000);


    __mul_64x128_full (t2, t, 1000ull, b11);
    __sub_128_128 (d11, bcoeff, t);
    __mul_64x128_full (t2, t, 1000ull, b10);
    __sub_128_128 (d10, b11, t);
    __mul_64x128_full (t2, t, 1000ull, b9);
    __sub_128_128 (d9, b10, t);
    __mul_64x128_full (t2, t, 1000ull, b8);
    __sub_128_128 (d8, b9, t);
    __mul_64x128_full (t2, t, 1000ull, b7);
    __sub_128_128 (d7, b8, t);
    __mul_64x128_full (t2, t, 1000ull, b6);
    __sub_128_128 (d6, b7, t);
    __mul_64x128_full (t2, t, 1000ull, b5);
    __sub_128_128 (d5, b6, t);
    __mul_64x128_full (t2, t, 1000ull, b4);
    __sub_128_128 (d4, b5, t);
    __mul_64x128_full (t2, t, 1000ull, b3);
    __sub_128_128 (d3, b4, t);
    __mul_64x128_full (t2, t, 1000ull, b2);
    __sub_128_128 (d2, b3, t);
    __mul_64x128_full (t2, t, 1000ull, b1);
    __sub_128_128 (d1, b2, t);
    d0 = b1;

  }

  dcoeff.w[0] = bid_b2d[d11.w[0]] | (bid_b2d[d10.w[0]] << 10) |
    (bid_b2d[d9.w[0]] << 20) | (bid_b2d[d8.w[0]] << 30) | (bid_b2d[d7.w[0]] << 40) |
    (bid_b2d[d6.w[0]] << 50) | (bid_b2d[d5.w[0]] << 60);
  dcoeff.w[1] =
    (bid_b2d[d5.w[0]] >> 4) | (bid_b2d[d4.w[0]] << 6) | (bid_b2d[d3.w[0]] << 16) |
    (bid_b2d[d2.w[0]] << 26) | (bid_b2d[d1.w[0]] << 36);

  res.w[0] = dcoeff.w[0];
  if (d0.w[0] >= 8) {
    res.w[1] =
      sign.
      w[1] |
      ((0x18000 | ((exp >> 12) << 13) | ((d0.w[0] & 1) << 12) |
	(exp & 0xfff)) << 46) | dcoeff.w[1];
  } else {
    res.w[1] =
      sign.
      w[1] | ((((exp >> 12) << 15) | (d0.w[0] << 12) | (exp & 0xfff))
	      << 46) | dcoeff.w[1];
  }

  res.w[1] |= nanb;

  BID_SWAP128 (res);
  BID_RETURN (res);
}

#if DECIMAL_CALL_BY_REFERENCE
void
bid_dpd_to_bid128 (BID_UINT128 * pres, BID_UINT128 * pda) {
  BID_UINT128 da = *pda;
#else
DFP_WRAPFN_DFP(128, bid_dpd_to_bid128, 128)
BID_UINT128
bid_dpd_to_bid128 (BID_UINT128 da) {
#endif
  BID_UINT128 in = *(BID_UINT128 *) & da;
  BID_UINT128 res;

  BID_UINT128 sign;
  BID_UINT64 exp, comb;
  BID_UINT128 trailing;
  BID_UINT64 d0, d1, d2, d3, d4, d5, d6, d7, d8, d9, d10, d11;
  BID_UINT128 bcoeff;
  BID_UINT64 tl, th;
  BID_UINT64 nanb = 0;

  sign.w[1] = (in.w[BID_HIGH_128W] & 0x8000000000000000ull);
  sign.w[0] = 0;
  comb = (in.w[BID_HIGH_128W] & 0x7fffc00000000000ull) >> 46;
  trailing.w[1] = (in.w[BID_HIGH_128W] & 0x00003fffffffffffull);
  trailing.w[0] = in.w[BID_LOW_128W];
  exp = 0;

  if ((comb & 0x1f000) == 0x1e000) {	// G0..G4 = 11110 -> Inf
    res.w[BID_HIGH_128W] = in.w[BID_HIGH_128W] & 0xf800000000000000ull;
    res.w[BID_LOW_128W] = 0ull;
    BID_RETURN (res);
  } else if ((comb & 0x1f000) == 0x1f000) {	// G0..G4 = 11111 -> NaN
    nanb = in.w[BID_HIGH_128W] & 0xfe00000000000000ull;
    exp = 0;
    d0 = 0;
  } else {	// Normal number
    if ((comb & 0x18000) == 0x18000) {	// G0..G1 = 11 -> d0 = 8 + G4
      d0 = 8 + (comb & 0x01000 ? 1 : 0);
      exp =
	(comb & 0x04000 ? 1 : 0) * 0x2000 +
	(comb & 0x02000 ? 1 : 0) * 0x1000;
      // exp leading bits are G2..G3
    } else {
      d0 =
	4 * (comb & 0x04000 ? 1 : 0) + 2 * (comb & 0x2000 ? 1 : 0) +
	(comb & 0x1000 ? 1 : 0);
      exp =
	(comb & 0x10000 ? 1 : 0) * 0x2000 +
	(comb & 0x08000 ? 1 : 0) * 0x1000;
      // exp loading bits are G0..G1
    }
  }

  d11 = bid_d2b[(trailing.w[0]) & 0x3ff];
  d10 = bid_d2b[(trailing.w[0] >> 10) & 0x3ff];
  d9 = bid_d2b[(trailing.w[0] >> 20) & 0x3ff];
  d8 = bid_d2b[(trailing.w[0] >> 30) & 0x3ff];
  d7 = bid_d2b[(trailing.w[0] >> 40) & 0x3ff];
  d6 = bid_d2b[(trailing.w[0] >> 50) & 0x3ff];
  d5 = bid_d2b[(trailing.w[0] >> 60) | ((trailing.w[1] & 0x3f) << 4)];
  d4 = bid_d2b[(trailing.w[1] >> 6) & 0x3ff];
  d3 = bid_d2b[(trailing.w[1] >> 16) & 0x3ff];
  d2 = bid_d2b[(trailing.w[1] >> 26) & 0x3ff];
  d1 = bid_d2b[(trailing.w[1] >> 36) & 0x3ff];

  tl =
    d11 + (d10 * 1000ull) + (d9 * 1000000ull) + (d8 * 1000000000ull) +
    (d7 * 1000000000000ull) + (d6 * 1000000000000000ull);
  th =
    d5 + (d4 * 1000ull) + (d3 * 1000000ull) + (d2 * 1000000000ull) +
    (d1 * 1000000000000ull) + (d0 * 1000000000000000ull);
  __mul_64x64_to_128 (bcoeff, th, 1000000000000000000ull);
  __add_128_64 (bcoeff, bcoeff, tl);

  if (!nanb)
    exp += (comb & 0xfff);

  res.w[0] = bcoeff.w[0];
  res.w[1] = (exp << 49) | sign.w[1] | bcoeff.w[1];

  res.w[1] |= nanb;

  BID_SWAP128 (res);
  BID_RETURN (res);
}

