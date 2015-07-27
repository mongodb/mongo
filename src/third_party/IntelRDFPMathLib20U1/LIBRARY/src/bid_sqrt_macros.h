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

#ifndef _SQRT_MACROS_H_
#define _SQRT_MACROS_H_

#include "bid_internal.h"

#if DOUBLE_EXTENDED_ON

BID_EXTERN_C BINARY80 SQRT80 (BINARY80);


__BID_INLINE__ BID_UINT64
short_sqrt128 (BID_UINT128 A10) {
  BINARY80 lx, ly, l64;
  int_float f64;

  // 2^64
  f64.i = 0x5f800000;
  l64 = (BINARY80) f64.d;
  lx = (BINARY80) A10.w[1] * l64 + (BINARY80) A10.w[0];
  ly = SQRT80 (lx);
  return (BID_UINT64) ly;
}


typedef union BID_ALIGN (16)
     {
       BID_UINT64 w[2];
	   long double d; 
     } int_dext;


__BID_INLINE__ void
bid_long_sqrt128 (BID_UINT128 * pCS, BID_UINT256 C256) {
  BID_UINT128 CS;
  BID_UINT64 X;
  BID_SINT64 SE;
  BINARY80 l64, lm64, l128, lxL, lx, lS, lSH, lSL, lE, l3, l2,
    l1, l0, lp, lCl;
  int_float f64, fm64;
  int_dext tmp_dext;

  // 2^64
  f64.i = 0x5f800000;
  l64 = (BINARY80) f64.d;

  l128 = l64 * l64;
  lx = l3 = (BINARY80) C256.w[3] * l64 * l128;
  l2 = (BINARY80) C256.w[2] * l128;
  lx = FENCE (lx + l2);
  l1 = (BINARY80) C256.w[1] * l64;
  lx = FENCE (lx + l1);
  l0 = (BINARY80) C256.w[0];
  lx = FENCE (lx + l0);
  // sqrt(C256)
  lS = SQRT80 (lx);

  // get coefficient
  // 2^(-64)
  fm64.i = 0x1f800000;
  lm64 = (BINARY80) fm64.d;
  CS.w[1] = (BID_UINT64) (lS * lm64);
  CS.w[0] = (BID_UINT64) (lS - (BINARY80) CS.w[1] * l64);

  //printf("C256=%016I64x %016I64x %016I64x %016I64x, CS=%016I64x %016I64x \n",C256.w[3],C256.w[2],C256.w[1],C256.w[0],CS.w[1],CS.w[0]);

  ///////////////////////////////////////
  //  CAUTION!
  //  little endian code only
  //  add solution for big endian
  //////////////////////////////////////
    tmp_dext.d = lS;

#if BID_BIG_ENDIAN
	tmp_dext.w[0] &= 0xffffffffffff0000ull;
	tmp_dext.w[1] = 0;
#else
  //lSH = lS;
  //*((BID_UINT64 *) & lSH) &= 0xffffffff00000000ull;
	tmp_dext.w[0] &= 0xffffffff00000000ull;
#endif
	lSH = tmp_dext.d;

  // correction for C256 rounding
  lCl = FENCE (l3 - lx);
  lCl = FENCE (lCl + l2);
  lCl = FENCE (lCl + l1);
  lCl = FENCE (lCl + l0);

  lSL = lS - lSH;

  //////////////////////////////////////////
  //   Watch for compiler re-ordering
  //
  /////////////////////////////////////////
  // C256-S^2
  lxL = FENCE (lx - lSH * lSH);
  lp = lSH * lSL;
  lp += lp;
  lxL = FENCE (lxL - lp);
  lSL *= lSL;
  lxL = FENCE (lxL - lSL);
  lCl += lxL;

  // correction term
  lE = lCl / (lS + lS);

  // get low part of coefficient
  X = CS.w[0];
  if (lCl >= 0) {
    SE = (BID_SINT64) (lE);
    CS.w[0] += SE;
    if (CS.w[0] < X)
      CS.w[1]++;
  } else {
    SE = (BID_SINT64) (-lE);
    CS.w[0] -= SE;
    if (CS.w[0] > X)
      CS.w[1]--;
  }

  pCS->w[0] = CS.w[0];
  pCS->w[1] = CS.w[1];
}

#else

BID_EXTERN_C double sqrt (double);

__BID_INLINE__ BID_UINT64
short_sqrt128 (BID_UINT128 A10) {
  BID_UINT256 ARS, ARS0, AE0, AE, S;

  BID_UINT64 MY, ES, CY;
  double lx, l64;
  int_double f64, ly;
  int ey, k;

  // 2^64
  f64.i = 0x43f0000000000000ull;
  l64 = f64.d;
  lx = (double) A10.w[1] * l64 + (double) A10.w[0];
  ly.d = 1.0 / sqrt (lx);

  MY = (ly.i & 0x000fffffffffffffull) | 0x0010000000000000ull;
  ey = 0x3ff - (ly.i >> 52);

  // A10*RS^2
  __mul_64x128_to_192 (ARS0, MY, A10);
  __mul_64x192_to_256 (ARS, MY, ARS0);

  // shr by 2*ey+40, to get a 64-bit value
  k = (ey << 1) + 104 - 64;
  if (k >= 128) {
    if (k > 128)
      ES = (ARS.w[2] >> (k - 128)) | (ARS.w[3] << (192 - k));
    else
      ES = ARS.w[2];
  } else {
    if (k >= 64) {
      ARS.w[0] = ARS.w[1];
      ARS.w[1] = ARS.w[2];
      k -= 64;
    }
    if (k) {
      __shr_128 (ARS, ARS, k);
    }
    ES = ARS.w[0];
  }

  ES = ((BID_SINT64) ES) >> 1;

  if (((BID_SINT64) ES) < 0) {
    ES = -ES;

    // A*RS*eps (scaled by 2^64)
    __mul_64x192_to_256 (AE0, ES, ARS0);

    AE.w[0] = AE0.w[1];
    AE.w[1] = AE0.w[2];
    AE.w[2] = AE0.w[3];

    __add_carry_out (S.w[0], CY, ARS0.w[0], AE.w[0]);
    __add_carry_in_out (S.w[1], CY, ARS0.w[1], AE.w[1], CY);
    S.w[2] = ARS0.w[2] + AE.w[2] + CY;
  } else {
    // A*RS*eps (scaled by 2^64)
    __mul_64x192_to_256 (AE0, ES, ARS0);

    AE.w[0] = AE0.w[1];
    AE.w[1] = AE0.w[2];
    AE.w[2] = AE0.w[3];

    __sub_borrow_out (S.w[0], CY, ARS0.w[0], AE.w[0]);
    __sub_borrow_in_out (S.w[1], CY, ARS0.w[1], AE.w[1], CY);
    S.w[2] = ARS0.w[2] - AE.w[2] - CY;
  }

  k = ey + 51;

  if (k >= 64) {
    if (k >= 128) {
      S.w[0] = S.w[2];
      S.w[1] = 0;
      k -= 128;
    } else {
      S.w[0] = S.w[1];
      S.w[1] = S.w[2];
    }
    k -= 64;
  }
  if (k) {
    __shr_128 (S, S, k);
  }


  return (BID_UINT64) ((S.w[0] + 1) >> 1);

}



__BID_INLINE__ void
bid_long_sqrt128 (BID_UINT128 * pCS, BID_UINT256 C256) {
  BID_UINT512 ARS0, ARS;
  BID_UINT256 ARS00, AE, AE2, S;
  BID_UINT128 ES, ES2, ARS1;
  BID_UINT64 ES32, CY, MY;
  double l64, l128, lx, l2, l1, l0;
  int_double f64, ly;
  int ey, k, k2;

  // 2^64
  f64.i = 0x43f0000000000000ull;
  l64 = f64.d;

  l128 = l64 * l64;
  lx = (double) C256.w[3] * l64 * l128;
  l2 = (double) C256.w[2] * l128;
  lx = FENCE (lx + l2);
  l1 = (double) C256.w[1] * l64;
  lx = FENCE (lx + l1);
  l0 = (double) C256.w[0];
  lx = FENCE (lx + l0);
  // sqrt(C256)
  ly.d = 1.0 / sqrt (lx);

  MY = (ly.i & 0x000fffffffffffffull) | 0x0010000000000000ull;
  ey = 0x3ff - (ly.i >> 52);

  // A10*RS^2, scaled by 2^(2*ey+104)
  __mul_64x256_to_320 (ARS0, MY, C256);
  __mul_64x320_to_384 (ARS, MY, ARS0);

  // shr by k=(2*ey+104)-128
  // expect k is in the range (192, 256) if result in [10^33, 10^34)
  // apply an additional signed shift by 1 at the same time (to get eps=eps0/2)
  k = (ey << 1) + 104 - 128 - 192;
  k2 = 64 - k;
  ES.w[0] = (ARS.w[3] >> (k + 1)) | (ARS.w[4] << (k2 - 1));
  ES.w[1] = (ARS.w[4] >> k) | (ARS.w[5] << k2);
  ES.w[1] = ((BID_SINT64) ES.w[1]) >> 1;

  // A*RS >> 192 (for error term computation)
  ARS1.w[0] = ARS0.w[3];
  ARS1.w[1] = ARS0.w[4];

  // A*RS>>64
  ARS00.w[0] = ARS0.w[1];
  ARS00.w[1] = ARS0.w[2];
  ARS00.w[2] = ARS0.w[3];
  ARS00.w[3] = ARS0.w[4];

  if (((BID_SINT64) ES.w[1]) < 0) {
    ES.w[0] = -ES.w[0];
    ES.w[1] = -ES.w[1];
    if (ES.w[0])
      ES.w[1]--;

    // A*RS*eps 
    __mul_128x128_to_256 (AE, ES, ARS1);

    __add_carry_out (S.w[0], CY, ARS00.w[0], AE.w[0]);
    __add_carry_in_out (S.w[1], CY, ARS00.w[1], AE.w[1], CY);
    __add_carry_in_out (S.w[2], CY, ARS00.w[2], AE.w[2], CY);
    S.w[3] = ARS00.w[3] + AE.w[3] + CY;
  } else {
    // A*RS*eps 
    __mul_128x128_to_256 (AE, ES, ARS1);

    __sub_borrow_out (S.w[0], CY, ARS00.w[0], AE.w[0]);
    __sub_borrow_in_out (S.w[1], CY, ARS00.w[1], AE.w[1], CY);
    __sub_borrow_in_out (S.w[2], CY, ARS00.w[2], AE.w[2], CY);
    S.w[3] = ARS00.w[3] - AE.w[3] - CY;
  }

  // 3/2*eps^2, scaled by 2^128
  ES32 = ES.w[1] + (ES.w[1] >> 1);
  __mul_64x64_to_128 (ES2, ES32, ES.w[1]);
  // A*RS*3/2*eps^2
  __mul_128x128_to_256 (AE2, ES2, ARS1);

  // result, scaled by 2^(ey+52-64)
  __add_carry_out (S.w[0], CY, S.w[0], AE2.w[0]);
  __add_carry_in_out (S.w[1], CY, S.w[1], AE2.w[1], CY);
  __add_carry_in_out (S.w[2], CY, S.w[2], AE2.w[2], CY);
  S.w[3] = S.w[3] + AE2.w[3] + CY;

  // k in (0, 64)
  k = ey + 51 - 128;
  k2 = 64 - k;
  S.w[0] = (S.w[1] >> k) | (S.w[2] << k2);
  S.w[1] = (S.w[2] >> k) | (S.w[3] << k2);

  // round to nearest
  S.w[0]++;
  if (!S.w[0])
    S.w[1]++;

  pCS->w[0] = (S.w[1] << 63) | (S.w[0] >> 1);
  pCS->w[1] = S.w[1] >> 1;

}

#endif
#endif
