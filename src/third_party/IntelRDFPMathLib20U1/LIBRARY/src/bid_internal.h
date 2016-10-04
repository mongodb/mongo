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

#ifndef __BIDECIMAL_H
#define __BIDECIMAL_H

#define _CRT_SECURE_NO_DEPRECATE
#if defined(_MSC_VER) && !defined(__INTEL_COMPILER)
#   pragma warning( disable: 4996 )
#endif

#include "bid_conf.h"
#include "bid_functions.h"

#define __BID_INLINE__ static __inline

#if defined (__INTEL_COMPILER)
#define FENCE __fence
#else
#define FENCE 
#endif

/*********************************************************************
 *
 *      Logical Shift Macros
 *
 *********************************************************************/

#define __shr_128(Q, A, k)                        \
{                                                 \
     (Q).w[0] = (A).w[0] >> k;                      \
	 (Q).w[0] |= (A).w[1] << (64-k);               \
	 (Q).w[1] = (A).w[1] >> k;                    \
}

#define __shr_128_long(Q, A, k)                   \
{                                                 \
	if((k)<64) {                                  \
     (Q).w[0] = (A).w[0] >> k;                    \
	 (Q).w[0] |= (A).w[1] << (64-k);              \
	 (Q).w[1] = (A).w[1] >> k;                    \
	}                                             \
	else {                                        \
	 (Q).w[0] = (A).w[1]>>((k)-64);               \
	 (Q).w[1] = 0;                                \
	}                                             \
}

#define __shl_128_long(Q, A, k)                   \
{                                                 \
	if((k)<64) {                                  \
     (Q).w[1] = (A).w[1] << k;                    \
	 (Q).w[1] |= (A).w[0] >> (64-k);              \
	 (Q).w[0] = (A).w[0] << k;                    \
	}                                             \
	else {                                        \
	 (Q).w[1] = (A).w[0]<<((k)-64);               \
	 (Q).w[0] = 0;                                \
	}                                             \
}

#define __low_64(Q)  (Q).w[0]
/*********************************************************************
 *
 *      String Macros
 *
 *********************************************************************/
#define tolower_macro(x) (((unsigned char)((x)-'A')<=('Z'-'A'))?((x)-'A'+'a'):(x))
/*********************************************************************
 *
 *      Compare Macros
 *
 *********************************************************************/
// greater than
//  return 0 if A<=B
//  non-zero if A>B
#define __unsigned_compare_gt_128(A, B)  \
    ((A.w[1]>B.w[1]) || ((A.w[1]==B.w[1]) && (A.w[0]>B.w[0])))
// greater-or-equal
#define __unsigned_compare_ge_128(A, B)  \
    ((A.w[1]>B.w[1]) || ((A.w[1]==B.w[1]) && (A.w[0]>=B.w[0])))
#define __test_equal_128(A, B)  (((A).w[1]==(B).w[1]) && ((A).w[0]==(B).w[0]))
// tighten exponent range
#define __tight_bin_range_128(bp, P, bin_expon)  \
{                                                \
BID_UINT64 M;                                        \
	M = 1;                                       \
	(bp) = (bin_expon);                          \
	if((bp)<63) {                                \
	  M <<= ((bp)+1);                            \
	  if((P).w[0] >= M) (bp)++; }                 \
	else if((bp)>64) {                           \
	  M <<= ((bp)+1-64);                         \
	  if(((P).w[1]>M) ||((P).w[1]==M && (P).w[0]))\
	      (bp)++; }                              \
	else if((P).w[1]) (bp)++;                    \
}
/*********************************************************************
 *
 *      Add/Subtract Macros
 *
 *********************************************************************/
// add 64-bit value to 128-bit 
#define __add_128_64(R128, A128, B64)    \
{                                        \
BID_UINT64 R64H;                             \
	R64H = (A128).w[1];                 \
	(R128).w[0] = (B64) + (A128).w[0];     \
	if((R128).w[0] < (B64))               \
	  R64H ++;                           \
    (R128).w[1] = R64H;                  \
}
// subtract 64-bit value from 128-bit 
#define __sub_128_64(R128, A128, B64)    \
{                                        \
BID_UINT64 R64H;                             \
	R64H = (A128).w[1];                  \
	if((A128).w[0] < (B64))               \
	  R64H --;                           \
    (R128).w[1] = R64H;                  \
	(R128).w[0] = (A128).w[0] - (B64);     \
}
// add 128-bit value to 128-bit 
// assume no carry-out
#define __add_128_128(R128, A128, B128)  \
{                                        \
BID_UINT128 Q128;                            \
	Q128.w[1] = (A128).w[1]+(B128).w[1]; \
	Q128.w[0] = (B128).w[0] + (A128).w[0];  \
	if(Q128.w[0] < (B128).w[0])            \
	  Q128.w[1] ++;                      \
    (R128).w[1] = Q128.w[1];             \
    (R128).w[0] = Q128.w[0];               \
}
#define __sub_128_128(R128, A128, B128)  \
{                                        \
BID_UINT128 Q128;                            \
	Q128.w[1] = (A128).w[1]-(B128).w[1]; \
	Q128.w[0] = (A128).w[0] - (B128).w[0];  \
	if((A128).w[0] < (B128).w[0])          \
	  Q128.w[1] --;                      \
    (R128).w[1] = Q128.w[1];             \
    (R128).w[0] = Q128.w[0];               \
}
#define __add_carry_out(S, CY, X, Y)    \
{                                      \
BID_UINT64 X1=X;                           \
	S = X + Y;                         \
	CY = (S<X1) ? 1 : 0;                \
}
#define __add_carry_in_out(S, CY, X, Y, CI)    \
{                                             \
BID_UINT64 X1;                                    \
	X1 = X + CI;                              \
	S = X1 + Y;                               \
	CY = ((S<X1) || (X1<CI)) ? 1 : 0;          \
}
#define __sub_borrow_out(S, CY, X, Y)    \
{                                      \
BID_UINT64 X1=X;                           \
	S = X - Y;                         \
	CY = (S>X1) ? 1 : 0;                \
}
#define __sub_borrow_in_out(S, CY, X, Y, CI)    \
{                                             \
BID_UINT64 X1, X0=X;                              \
	X1 = X - CI;                              \
	S = X1 - Y;                               \
	CY = ((S>X1) || (X1>X0)) ? 1 : 0;          \
}
// increment C128 and check for rounding overflow: 
// if (C_128) = 10^34 then (C_128) = 10^33 and increment the exponent
#define INCREMENT(C_128, exp)                                           \
{                                                                       \
  C_128.w[0]++;                                                         \
  if (C_128.w[0] == 0) C_128.w[1]++;                                    \
  if (C_128.w[1] == 0x0001ed09bead87c0ull &&                            \
      C_128.w[0] == 0x378d8e6400000000ull) {                            \
    exp++;                                                              \
    C_128.w[1] = 0x0000314dc6448d93ull;                                 \
    C_128.w[0] = 0x38c15b0a00000000ull;                                 \
  }                                                                     \
}
// decrement C128 and check for rounding underflow, but only at the
// boundary: if C_128 = 10^33 - 1 and exp > 0 then C_128 = 10^34 - 1 
// and decrement the exponent 
#define DECREMENT(C_128, exp)                                           \
{                                                                       \
  C_128.w[0]--;                                                         \
  if (C_128.w[0] == 0xffffffffffffffffull) C_128.w[1]--;                \
  if (C_128.w[1] == 0x0000314dc6448d93ull &&                            \
      C_128.w[0] == 0x38c15b09ffffffffull && exp > 0) {                 \
    exp--;                                                              \
    C_128.w[1] = 0x0001ed09bead87c0ull;                                 \
    C_128.w[0] = 0x378d8e63ffffffffull;                                 \
  }                                                                     \
}

 /*********************************************************************
 *
 *      Multiply Macros
 *
 *********************************************************************/
#define __mul_64x64_to_64(P64, CX, CY)  (P64) = (CX) * (CY)
/***************************************
 *  Signed, Full 64x64-bit Multiply
 ***************************************/
#define __imul_64x64_to_128(P, CX, CY)  \
{                                       \
BID_UINT64 SX, SY;                          \
   __mul_64x64_to_128(P, CX, CY);       \
                                        \
   SX = ((BID_SINT64)(CX))>>63;             \
   SY = ((BID_SINT64)(CY))>>63;             \
   SX &= CY;   SY &= CX;                \
                                        \
   (P).w[1] = (P).w[1] - SX - SY;       \
}
/***************************************
 *  Signed, Full 64x128-bit Multiply
 ***************************************/
#define __imul_64x128_full(Ph, Ql, A, B)          \
{                                                 \
BID_UINT128 ALBL, ALBH, QM2, QM;                      \
                                                  \
	__imul_64x64_to_128(ALBH, (A), (B).w[1]);     \
	__imul_64x64_to_128(ALBL, (A), (B).w[0]);      \
                                                  \
	(Ql).w[0] = ALBL.w[0];                          \
	QM.w[0] = ALBL.w[1];                           \
	QM.w[1] = ((BID_SINT64)ALBL.w[1])>>63;            \
    __add_128_128(QM2, ALBH, QM);                 \
	(Ql).w[1] = QM2.w[0];                          \
    Ph = QM2.w[1];                                \
}
/*****************************************************
 *      Unsigned Multiply Macros
 *****************************************************/
// get full 64x64bit product
//
#define __mul_64x64_to_128(P, CX, CY)   \
{                                       \
BID_UINT64 CXH, CXL, CYH,CYL,PL,PH,PM,PM2;\
	CXH = (CX) >> 32;                     \
	CXL = (BID_UINT32)(CX);                   \
	CYH = (CY) >> 32;                     \
	CYL = (BID_UINT32)(CY);                   \
	                                      \
    PM = CXH*CYL;                         \
	PH = CXH*CYH;                         \
	PL = CXL*CYL;                         \
	PM2 = CXL*CYH;                        \
	PH += (PM>>32);                       \
	PM = (BID_UINT64)((BID_UINT32)PM)+PM2+(PL>>32); \
                                          \
	(P).w[1] = PH + (PM>>32);             \
	(P).w[0] = (PM<<32)+(BID_UINT32)PL;       \
}
// get full 64x64bit product
// Note:
// This macro is used for CX < 2^61, CY < 2^61
//
#define __mul_64x64_to_128_fast(P, CX, CY)   \
{                                       \
BID_UINT64 CXH, CXL, CYH, CYL, PL, PH, PM;  \
	CXH = (CX) >> 32;                   \
	CXL = (BID_UINT32)(CX);                 \
	CYH = (CY) >> 32;                   \
	CYL = (BID_UINT32)(CY);                 \
	                                    \
    PM = CXH*CYL;                       \
	PL = CXL*CYL;                       \
	PH = CXH*CYH;                       \
	PM += CXL*CYH;                      \
	PM += (PL>>32);                     \
                                        \
	(P).w[1] = PH + (PM>>32);           \
	(P).w[0] = (PM<<32)+(BID_UINT32)PL;      \
}
// used for CX< 2^60
#define __sqr64_fast(P, CX)   \
{                                       \
BID_UINT64 CXH, CXL, PL, PH, PM;            \
	CXH = (CX) >> 32;                   \
	CXL = (BID_UINT32)(CX);                 \
	                                    \
    PM = CXH*CXL;                       \
	PL = CXL*CXL;                       \
	PH = CXH*CXH;                       \
	PM += PM;                           \
	PM += (PL>>32);                     \
                                        \
	(P).w[1] = PH + (PM>>32);           \
	(P).w[0] = (PM<<32)+(BID_UINT32)PL;     \
}
// get full 64x64bit product
// Note:
// This implementation is used for CX < 2^61, CY < 2^61
//
#define __mul_64x64_to_64_high_fast(P, CX, CY)   \
{                                       \
BID_UINT64 CXH, CXL, CYH, CYL, PL, PH, PM;  \
	CXH = (CX) >> 32;                   \
	CXL = (BID_UINT32)(CX);                 \
	CYH = (CY) >> 32;                   \
	CYL = (BID_UINT32)(CY);                 \
	                                    \
    PM = CXH*CYL;                       \
	PL = CXL*CYL;                       \
	PH = CXH*CYH;                       \
	PM += CXL*CYH;                      \
	PM += (PL>>32);                     \
                                        \
	(P) = PH + (PM>>32);                \
}
// get full 64x64bit product 
//
#define __mul_64x64_to_128_full(P, CX, CY)     \
{                                         \
BID_UINT64 CXH, CXL, CYH,CYL,PL,PH,PM,PM2;\
	CXH = (CX) >> 32;                     \
	CXL = (BID_UINT32)(CX);                   \
	CYH = (CY) >> 32;                     \
	CYL = (BID_UINT32)(CY);                   \
	                                      \
    PM = CXH*CYL;                         \
	PH = CXH*CYH;                         \
	PL = CXL*CYL;                         \
	PM2 = CXL*CYH;                        \
	PH += (PM>>32);                       \
	PM = (BID_UINT64)((BID_UINT32)PM)+PM2+(PL>>32); \
                                          \
	(P).w[1] = PH + (PM>>32);             \
	(P).w[0] = (PM<<32)+(BID_UINT32)PL;        \
}
#define __mul_128x128_high(Q, A, B)               \
{                                                 \
BID_UINT128 ALBL, ALBH, AHBL, AHBH, QM, QM2;          \
                                                  \
	__mul_64x64_to_128(ALBH, (A).w[0], (B).w[1]);  \
	__mul_64x64_to_128(AHBL, (B).w[0], (A).w[1]);  \
	__mul_64x64_to_128(ALBL, (A).w[0], (B).w[0]);   \
	__mul_64x64_to_128(AHBH, (A).w[1],(B).w[1]);  \
                                                  \
    __add_128_128(QM, ALBH, AHBL);                \
    __add_128_64(QM2, QM, ALBL.w[1]);             \
    __add_128_64((Q), AHBH, QM2.w[1]);            \
}
#define __mul_128x128_full(Qh, Ql, A, B)          \
{                                                 \
BID_UINT128 ALBL, ALBH, AHBL, AHBH, QM, QM2;          \
                                                  \
	__mul_64x64_to_128(ALBH, (A).w[0], (B).w[1]);  \
	__mul_64x64_to_128(AHBL, (B).w[0], (A).w[1]);  \
	__mul_64x64_to_128(ALBL, (A).w[0], (B).w[0]);   \
	__mul_64x64_to_128(AHBH, (A).w[1],(B).w[1]);  \
                                                  \
    __add_128_128(QM, ALBH, AHBL);                \
	(Ql).w[0] = ALBL.w[0];                          \
    __add_128_64(QM2, QM, ALBL.w[1]);             \
    __add_128_64((Qh), AHBH, QM2.w[1]);           \
	(Ql).w[1] = QM2.w[0];                          \
}
#define __mul_128x128_low(Ql, A, B)               \
{                                                 \
BID_UINT128 ALBL;                                     \
BID_UINT64 QM64;                                      \
                                                  \
	__mul_64x64_to_128(ALBL, (A).w[0], (B).w[0]);   \
	QM64 = (B).w[0]*(A).w[1] + (A).w[0]*(B).w[1];   \
                                                  \
	(Ql).w[0] = ALBL.w[0];                          \
	(Ql).w[1] = QM64 + ALBL.w[1];                 \
}
#define __mul_64x128_low(Ql, A, B)                \
{                                                 \
  BID_UINT128 ALBL, ALBH, QM2;                        \
  __mul_64x64_to_128(ALBH, (A), (B).w[1]);        \
  __mul_64x64_to_128(ALBL, (A), (B).w[0]);        \
  (Ql).w[0] = ALBL.w[0];                          \
  __add_128_64(QM2, ALBH, ALBL.w[1]);             \
  (Ql).w[1] = QM2.w[0];                           \
}
#define __mul_64x128_full(Ph, Ql, A, B)           \
{                                                 \
BID_UINT128 ALBL, ALBH, QM2;                          \
                                                  \
	__mul_64x64_to_128(ALBH, (A), (B).w[1]);      \
	__mul_64x64_to_128(ALBL, (A), (B).w[0]);       \
                                                  \
	(Ql).w[0] = ALBL.w[0];                          \
    __add_128_64(QM2, ALBH, ALBL.w[1]);           \
	(Ql).w[1] = QM2.w[0];                          \
    Ph = QM2.w[1];                                \
}
#define __mul_64x128_to_192(Q, A, B)              \
{                                                 \
BID_UINT128 ALBL, ALBH, QM2;                          \
                                                  \
	__mul_64x64_to_128(ALBH, (A), (B).w[1]);      \
	__mul_64x64_to_128(ALBL, (A), (B).w[0]);      \
                                                  \
	(Q).w[0] = ALBL.w[0];                         \
    __add_128_64(QM2, ALBH, ALBL.w[1]);           \
	(Q).w[1] = QM2.w[0];                          \
    (Q).w[2] = QM2.w[1];                          \
}
#define __mul_64x128_to192(Q, A, B)          \
{                                             \
BID_UINT128 ALBL, ALBH, QM2;                      \
                                              \
    __mul_64x64_to_128(ALBH, (A), (B).w[1]);  \
    __mul_64x64_to_128(ALBL, (A), (B).w[0]);  \
                                              \
    (Q).w[0] = ALBL.w[0];                    \
    __add_128_64(QM2, ALBH, ALBL.w[1]);       \
    (Q).w[1] = QM2.w[0];                     \
    (Q).w[2] = QM2.w[1];                     \
}
#define __mul_128x128_to_256(P256, A, B)                         \
{                                                                \
BID_UINT128 Qll, Qlh;                                                \
BID_UINT64 Phl, Phh, CY1, CY2;                                         \
                                                                 \
   __mul_64x128_full(Phl, Qll, A.w[0], B);                       \
   __mul_64x128_full(Phh, Qlh, A.w[1], B);                       \
  (P256).w[0] = Qll.w[0];                                        \
	   __add_carry_out((P256).w[1],CY1, Qlh.w[0], Qll.w[1]);      \
	   __add_carry_in_out((P256).w[2],CY2, Qlh.w[1], Phl, CY1);    \
	   (P256).w[3] = Phh + CY2;                                   \
}
//
// For better performance, will check A.w[1] against 0,
//                         but not B.w[1]
// Use this macro accordingly
#define __mul_128x128_to_256_check_A(P256, A, B)                   \
{                                                                  \
BID_UINT128 Qll, Qlh;                                                  \
BID_UINT64 Phl, Phh, CY1, CY2;                                           \
                                                                   \
   __mul_64x128_full(Phl, Qll, A.w[0], B);                          \
  (P256).w[0] = Qll.w[0];                                        \
   if(A.w[1])  {                                                   \
	   __mul_64x128_full(Phh, Qlh, A.w[1], B);                     \
	   __add_carry_out((P256).w[1],CY1, Qlh.w[0], Qll.w[1]);      \
	   __add_carry_in_out((P256).w[2],CY2, Qlh.w[1], Phl, CY1);   \
	   (P256).w[3] = Phh + CY2;   }                              \
   else  {                                                         \
	   (P256).w[1] = Qll.w[1];                                  \
	   (P256).w[2] = Phl;                                       \
	   (P256).w[3] = 0;  }                                      \
}
#define __mul_64x192_to_256(lP, lA, lB)                      \
{                                                         \
BID_UINT128 lP0,lP1,lP2;                                      \
BID_UINT64 lC;                                                 \
	__mul_64x64_to_128(lP0, lA, (lB).w[0]);              \
	__mul_64x64_to_128(lP1, lA, (lB).w[1]);              \
	__mul_64x64_to_128(lP2, lA, (lB).w[2]);              \
	(lP).w[0] = lP0.w[0];                                \
	__add_carry_out((lP).w[1],lC,lP1.w[0],lP0.w[1]);      \
	__add_carry_in_out((lP).w[2],lC,lP2.w[0],lP1.w[1],lC); \
	(lP).w[3] = lP2.w[1] + lC;                           \
}
#define __mul_64x256_to_320(P, A, B)                    \
{                                                       \
BID_UINT128 lP0,lP1,lP2,lP3;                                \
BID_UINT64 lC;                                               \
	__mul_64x64_to_128(lP0, A, (B).w[0]);             \
	__mul_64x64_to_128(lP1, A, (B).w[1]);             \
	__mul_64x64_to_128(lP2, A, (B).w[2]);             \
	__mul_64x64_to_128(lP3, A, (B).w[3]);             \
	(P).w[0] = lP0.w[0];                               \
	__add_carry_out((P).w[1],lC,lP1.w[0],lP0.w[1]);      \
	__add_carry_in_out((P).w[2],lC,lP2.w[0],lP1.w[1],lC); \
	__add_carry_in_out((P).w[3],lC,lP3.w[0],lP2.w[1],lC); \
	(P).w[4] = lP3.w[1] + lC;                          \
}
#define __mul_192x192_to_384(P, A, B)                          \
{                                                              \
BID_UINT256 P0,P1,P2;                                              \
BID_UINT64 CY;                                                      \
	__mul_64x192_to_256(P0, (A).w[0], B);                   \
	__mul_64x192_to_256(P1, (A).w[1], B);                   \
	__mul_64x192_to_256(P2, (A).w[2], B);                   \
	(P).w[0] = P0.w[0];                                  \
	__add_carry_out((P).w[1],CY,P1.w[0],P0.w[1]);      \
	__add_carry_in_out((P).w[2],CY,P1.w[1],P0.w[2],CY); \
	__add_carry_in_out((P).w[3],CY,P1.w[2],P0.w[3],CY); \
	(P).w[4] = P1.w[3] + CY;                              \
	__add_carry_out((P).w[2],CY,P2.w[0],(P).w[2]);     \
	__add_carry_in_out((P).w[3],CY,P2.w[1],(P).w[3],CY);   \
	__add_carry_in_out((P).w[4],CY,P2.w[2],(P).w[4],CY);   \
	(P).w[5] = P2.w[3] + CY;                              \
}
#define __mul_64x320_to_384(P, A, B)                    \
{                                                       \
BID_UINT128 lP0,lP1,lP2,lP3,lP4;                            \
BID_UINT64 lC;                                               \
	__mul_64x64_to_128(lP0, A, (B).w[0]);             \
	__mul_64x64_to_128(lP1, A, (B).w[1]);             \
	__mul_64x64_to_128(lP2, A, (B).w[2]);             \
	__mul_64x64_to_128(lP3, A, (B).w[3]);             \
	__mul_64x64_to_128(lP4, A, (B).w[4]);             \
	(P).w[0] = lP0.w[0];                               \
	__add_carry_out((P).w[1],lC,lP1.w[0],lP0.w[1]);      \
	__add_carry_in_out((P).w[2],lC,lP2.w[0],lP1.w[1],lC); \
	__add_carry_in_out((P).w[3],lC,lP3.w[0],lP2.w[1],lC); \
	__add_carry_in_out((P).w[4],lC,lP4.w[0],lP3.w[1],lC); \
	(P).w[5] = lP4.w[1] + lC;                          \
}
// A*A
// Full 128x128-bit product
#define __sqr128_to_256(P256, A)                                 \
{                                                                \
BID_UINT128 Qll, Qlh, Qhh;                                           \
BID_UINT64 TMP_C1, TMP_C2;                                 \
                                                                 \
   __mul_64x64_to_128(Qhh, A.w[1], A.w[1]);                      \
   __mul_64x64_to_128(Qlh, A.w[0], A.w[1]);                      \
   Qhh.w[1] += (Qlh.w[1]>>63);                                   \
   Qlh.w[1] = (Qlh.w[1]+Qlh.w[1])|(Qlh.w[0]>>63);                \
   Qlh.w[0] += Qlh.w[0];                                         \
   __mul_64x64_to_128(Qll, A.w[0], A.w[0]);                      \
                                                                 \
   __add_carry_out((P256).w[1],TMP_C1, Qlh.w[0], Qll.w[1]);      \
   (P256).w[0] = Qll.w[0];                                       \
   __add_carry_in_out((P256).w[2],TMP_C2, Qlh.w[1], Qhh.w[0], TMP_C1);    \
   (P256).w[3] = Qhh.w[1]+TMP_C2;                                         \
}
#define __mul_128x128_to_256_low_high(PQh, PQl, A, B)            \
{                                                                \
BID_UINT128 Qll, Qlh;                                                \
BID_UINT64 Phl, Phh, C1, C2;                                         \
                                                                 \
   __mul_64x128_full(Phl, Qll, A.w[0], B);                       \
   __mul_64x128_full(Phh, Qlh, A.w[1], B);                       \
  (PQl).w[0] = Qll.w[0];                                        \
	   __add_carry_out((PQl).w[1],C1, Qlh.w[0], Qll.w[1]);      \
	   __add_carry_in_out((PQh).w[0],C2, Qlh.w[1], Phl, C1);    \
	   (PQh).w[1] = Phh + C2;                                   \
}
#define __mul_256x256_to_512(P, A, B)                          \
{                                                              \
BID_UINT512 P0,P1,P2,P3;                                           \
BID_UINT64 CY;                                                      \
	__mul_64x256_to_320(P0, (A).w[0], B);                   \
	__mul_64x256_to_320(P1, (A).w[1], B);                   \
	__mul_64x256_to_320(P2, (A).w[2], B);                   \
	__mul_64x256_to_320(P3, (A).w[3], B);                   \
	(P).w[0] = P0.w[0];                                  \
	__add_carry_out((P).w[1],CY,P1.w[0],P0.w[1]);      \
	__add_carry_in_out((P).w[2],CY,P1.w[1],P0.w[2],CY); \
	__add_carry_in_out((P).w[3],CY,P1.w[2],P0.w[3],CY); \
	__add_carry_in_out((P).w[4],CY,P1.w[3],P0.w[4],CY); \
	(P).w[5] = P1.w[4] + CY;                              \
	__add_carry_out((P).w[2],CY,P2.w[0],(P).w[2]);     \
	__add_carry_in_out((P).w[3],CY,P2.w[1],(P).w[3],CY);   \
	__add_carry_in_out((P).w[4],CY,P2.w[2],(P).w[4],CY);   \
	__add_carry_in_out((P).w[5],CY,P2.w[3],(P).w[5],CY);   \
	(P).w[6] = P2.w[4] + CY;                              \
	__add_carry_out((P).w[3],CY,P3.w[0],(P).w[3]);     \
	__add_carry_in_out((P).w[4],CY,P3.w[1],(P).w[4],CY);   \
	__add_carry_in_out((P).w[5],CY,P3.w[2],(P).w[5],CY);   \
	__add_carry_in_out((P).w[6],CY,P3.w[3],(P).w[6],CY);   \
	(P).w[7] = P3.w[4] + CY;                              \
}
#define __mul_192x256_to_448(P, A, B)                          \
{                                                              \
BID_UINT512 P0,P1,P2;                                           \
BID_UINT64 CY;                                                      \
	__mul_64x256_to_320(P0, (A).w[0], B);                   \
	__mul_64x256_to_320(P1, (A).w[1], B);                   \
	__mul_64x256_to_320(P2, (A).w[2], B);                   \
	(P).w[0] = P0.w[0];                                  \
	__add_carry_out((P).w[1],CY,P1.w[0],P0.w[1]);      \
	__add_carry_in_out((P).w[2],CY,P1.w[1],P0.w[2],CY); \
	__add_carry_in_out((P).w[3],CY,P1.w[2],P0.w[3],CY); \
	__add_carry_in_out((P).w[4],CY,P1.w[3],P0.w[4],CY); \
	(P).w[5] = P1.w[4] + CY;                              \
	__add_carry_out((P).w[2],CY,P2.w[0],(P).w[2]);     \
	__add_carry_in_out((P).w[3],CY,P2.w[1],(P).w[3],CY);   \
	__add_carry_in_out((P).w[4],CY,P2.w[2],(P).w[4],CY);   \
	__add_carry_in_out((P).w[5],CY,P2.w[3],(P).w[5],CY);   \
	(P).w[6] = P2.w[4] + CY;                              \
}
#define __mul_320x320_to_640(P, A, B)                          \
{                                                              \
BID_UINT512 P0,P1,P2,P3;                                           \
BID_UINT64 CY;                                                     \
	__mul_256x256_to_512((P), (A), B);                   \
	__mul_64x256_to_320(P1, (A).w[4], B);                   \
	__mul_64x256_to_320(P2, (B).w[4], A);                   \
	__mul_64x64_to_128(P3, (A).w[4], (B).w[4]);               \
	__add_carry_out((P0).w[0],CY,P1.w[0],P2.w[0]);      \
	__add_carry_in_out((P0).w[1],CY,P1.w[1],P2.w[1],CY); \
	__add_carry_in_out((P0).w[2],CY,P1.w[2],P2.w[2],CY); \
	__add_carry_in_out((P0).w[3],CY,P1.w[3],P2.w[3],CY); \
	__add_carry_in_out((P0).w[4],CY,P1.w[4],P2.w[4],CY); \
	P3.w[1] += CY;                                       \
	__add_carry_out((P).w[4],CY,(P).w[4],P0.w[0]);      \
	__add_carry_in_out((P).w[5],CY,(P).w[5],P0.w[1],CY); \
	__add_carry_in_out((P).w[6],CY,(P).w[6],P0.w[2],CY); \
	__add_carry_in_out((P).w[7],CY,(P).w[7],P0.w[3],CY); \
	__add_carry_in_out((P).w[8],CY,P3.w[0],P0.w[4],CY); \
	(P).w[9] = P3.w[1] + CY;                             \
}
#define __mul_384x384_to_768(P, A, B)                          \
{                                                              \
BID_UINT512 P0,P1,P2,P3;                                           \
BID_UINT64 CY;                                                     \
	__mul_320x320_to_640((P), (A), B);                         \
	__mul_64x320_to_384(P1, (A).w[5], B);                   \
	__mul_64x320_to_384(P2, (B).w[5], A);                   \
	__mul_64x64_to_128(P3, (A).w[5], (B).w[5]);               \
	__add_carry_out((P0).w[0],CY,P1.w[0],P2.w[0]);      \
	__add_carry_in_out((P0).w[1],CY,P1.w[1],P2.w[1],CY); \
	__add_carry_in_out((P0).w[2],CY,P1.w[2],P2.w[2],CY); \
	__add_carry_in_out((P0).w[3],CY,P1.w[3],P2.w[3],CY); \
	__add_carry_in_out((P0).w[4],CY,P1.w[4],P2.w[4],CY); \
	__add_carry_in_out((P0).w[5],CY,P1.w[5],P2.w[5],CY); \
	P3.w[1] += CY;                                       \
	__add_carry_out((P).w[5],CY,(P).w[5],P0.w[0]);      \
	__add_carry_in_out((P).w[6],CY,(P).w[6],P0.w[1],CY); \
	__add_carry_in_out((P).w[7],CY,(P).w[7],P0.w[2],CY); \
	__add_carry_in_out((P).w[8],CY,(P).w[8],P0.w[3],CY); \
	__add_carry_in_out((P).w[9],CY,(P).w[9],P0.w[4],CY); \
	__add_carry_in_out((P).w[10],CY,P3.w[0],P0.w[5],CY); \
	(P).w[11] = P3.w[1] + CY;                             \
}
#define __mul_64x128_short(Ql, A, B)              \
{                                                 \
BID_UINT64 ALBH_L;                                    \
                                                  \
	__mul_64x64_to_64(ALBH_L, (A),(B).w[1]);      \
	__mul_64x64_to_128((Ql), (A), (B).w[0]);       \
                                                  \
	(Ql).w[1] += ALBH_L;                          \
}
#define __scale128_10(D,_TMP)                            \
{                                                        \
BID_UINT128 _TMP2,_TMP8;                                     \
	  _TMP2.w[1] = (_TMP.w[1]<<1)|(_TMP.w[0]>>63);       \
	  _TMP2.w[0] = _TMP.w[0]<<1;                         \
	  _TMP8.w[1] = (_TMP.w[1]<<3)|(_TMP.w[0]>>61);       \
	  _TMP8.w[0] = _TMP.w[0]<<3;                         \
	  __add_128_128(D, _TMP2, _TMP8);                    \
}
// 64x64-bit product
#define __mul_64x64_to_128MACH(P128, CX64, CY64)  \
{                                                  \
  BID_UINT64 CXH,CXL,CYH,CYL,PL,PH,PM,PM2;     \
  CXH = (CX64) >> 32;                              \
  CXL = (BID_UINT32)(CX64);                            \
  CYH = (CY64) >> 32;                              \
  CYL = (BID_UINT32)(CY64);                            \
  PM = CXH*CYL;                                    \
  PH = CXH*CYH;                                    \
  PL = CXL*CYL;                                    \
  PM2 = CXL*CYH;                                   \
  PH += (PM>>32);                                  \
  PM = (BID_UINT64)((BID_UINT32)PM)+PM2+(PL>>32);          \
  (P128).w[1] = PH + (PM>>32);                     \
  (P128).w[0] = (PM<<32)+(BID_UINT32)PL;                \
}
// 64x64-bit product
#define __mul_64x64_to_128HIGH(P64, CX64, CY64)  \
{                                                  \
  BID_UINT64 CXH,CXL,CYH,CYL,PL,PH,PM,PM2;     \
  CXH = (CX64) >> 32;                              \
  CXL = (BID_UINT32)(CX64);                            \
  CYH = (CY64) >> 32;                              \
  CYL = (BID_UINT32)(CY64);                            \
  PM = CXH*CYL;                                    \
  PH = CXH*CYH;                                    \
  PL = CXL*CYL;                                    \
  PM2 = CXL*CYH;                                   \
  PH += (PM>>32);                                  \
  PM = (BID_UINT64)((BID_UINT32)PM)+PM2+(PL>>32);          \
  P64 = PH + (PM>>32);                     \
}
#define __mul_128x64_to_128(Q128, A64, B128)        \
{                                                  \
  BID_UINT64 ALBH_L;                                   \
  ALBH_L = (A64) * (B128).w[1];                    \
  __mul_64x64_to_128MACH((Q128), (A64), (B128).w[0]);   \
  (Q128).w[1] += ALBH_L;                           \
}
// might simplify by calculating just QM2.w[0]
#define __mul_64x128_to_128(Ql, A, B)           \
{                                                 \
  BID_UINT128 ALBL, ALBH, QM2;                        \
  __mul_64x64_to_128(ALBH, (A), (B).w[1]);        \
  __mul_64x64_to_128(ALBL, (A), (B).w[0]);        \
  (Ql).w[0] = ALBL.w[0];                          \
  __add_128_64(QM2, ALBH, ALBL.w[1]);             \
  (Ql).w[1] = QM2.w[0];                           \
}
/*********************************************************************
 *
 *      BID Pack/Unpack Macros
 *
 *********************************************************************/
/////////////////////////////////////////
// BID64 definitions
////////////////////////////////////////
#define DECIMAL_MAX_EXPON_64  767
#define DECIMAL_EXPONENT_BIAS 398
#define MAX_FORMAT_DIGITS     16
/////////////////////////////////////////
// BID128 definitions
////////////////////////////////////////
#define DECIMAL_MAX_EXPON_128  12287
#define DECIMAL_EXPONENT_BIAS_128  6176
#define MAX_FORMAT_DIGITS_128      34
/////////////////////////////////////////
// BID32 definitions
////////////////////////////////////////
#define DECIMAL_MAX_EXPON_32  191
#define DECIMAL_EXPONENT_BIAS_32  101
#define MAX_FORMAT_DIGITS_32      7
////////////////////////////////////////
// Constant Definitions
///////////////////////////////////////
#define SPECIAL_ENCODING_MASK64 0x6000000000000000ull
#define INFINITY_MASK64         0x7800000000000000ull
#define SINFINITY_MASK64        0xf800000000000000ull
#define SSNAN_MASK64            0xfc00000000000000ull
#define NAN_MASK64              0x7c00000000000000ull
#define SNAN_MASK64             0x7e00000000000000ull
#define QUIET_MASK64            0xfdffffffffffffffull
#define LARGE_COEFF_MASK64      0x0007ffffffffffffull
#define LARGE_COEFF_HIGH_BIT64  0x0020000000000000ull
#define SMALL_COEFF_MASK64      0x001fffffffffffffull
#define EXPONENT_MASK64         0x3ff
#define EXPONENT_SHIFT_LARGE64  51
#define EXPONENT_SHIFT_SMALL64  53
#define LARGEST_BID64           0x77fb86f26fc0ffffull
#define SMALLEST_BID64          0xf7fb86f26fc0ffffull
#define SMALL_COEFF_MASK128     0x0001ffffffffffffull
#define LARGE_COEFF_MASK128     0x00007fffffffffffull
#define EXPONENT_MASK128        0x3fff
#define LARGEST_BID128_HIGH     0x5fffed09bead87c0ull
#define LARGEST_BID128_LOW      0x378d8e63ffffffffull
#define SPECIAL_ENCODING_MASK32 0x60000000ul
#define SINFINITY_MASK32        0xf8000000ul
#define INFINITY_MASK32         0x78000000ul
#define LARGE_COEFF_MASK32      0x007ffffful
#define LARGE_COEFF_HIGH_BIT32  0x00800000ul
#define SMALL_COEFF_MASK32      0x001ffffful
#define EXPONENT_MASK32         0xff
#define LARGEST_BID32           0x77f8967f
#define NAN_MASK32              0x7c000000
#define SNAN_MASK32             0x7e000000
#define SSNAN_MASK32            0xfc000000
#define QUIET_MASK32            0xfdffffff
#define MASK_BINARY_EXPONENT  0x7ff0000000000000ull
#define BINARY_EXPONENT_BIAS  0x3ff
#define UPPER_EXPON_LIMIT     51
// data needed for BID pack/unpack macros
BID_EXTERN_C BID_UINT64 bid_round_const_table[][19];
BID_EXTERN_C BID_UINT128 bid_reciprocals10_128[];
BID_EXTERN_C int bid_recip_scale[];
BID_EXTERN_C BID_UINT128 bid_power10_table_128[];
BID_EXTERN_C int bid_estimate_decimal_digits[];
BID_EXTERN_C int bid_estimate_bin_expon[];
BID_EXTERN_C BID_UINT64 bid_power10_index_binexp[];
BID_EXTERN_C int bid_short_recip_scale[];
BID_EXTERN_C int bid_bid_bid_recip_scale32[];
BID_EXTERN_C BID_UINT64 bid_reciprocals10_64[];
BID_EXTERN_C BID_UINT64 bid_bid_reciprocals10_32[];
BID_EXTERN_C BID_UINT128 bid_power10_index_binexp_128[];
BID_EXTERN_C BID_UINT128 bid_round_const_table_128[][36];


//////////////////////////////////////////////
//  Status Flag Handling
/////////////////////////////////////////////
#define __set_status_flags(fpsc, status)  *(fpsc) |= status
#define is_inexact(fpsc)  ((*(fpsc))&BID_INEXACT_EXCEPTION)

__BID_INLINE__ BID_UINT64
unpack_BID64 (BID_UINT64 * psign_x, int *pexponent_x,
	      BID_UINT64 * pcoefficient_x, BID_UINT64 x) {
  BID_UINT64 tmp, coeff;

  *psign_x = x & 0x8000000000000000ull;

  if ((x & SPECIAL_ENCODING_MASK64) == SPECIAL_ENCODING_MASK64) {
    // special encodings
    // coefficient
    coeff = (x & LARGE_COEFF_MASK64) | LARGE_COEFF_HIGH_BIT64;

    if ((x & INFINITY_MASK64) == INFINITY_MASK64) {
      *pexponent_x = 0;
      *pcoefficient_x = x & 0xfe03ffffffffffffull;
      if ((x & 0x0003ffffffffffffull) >= 1000000000000000ull)
	*pcoefficient_x = x & 0xfe00000000000000ull;
      if ((x & NAN_MASK64) == INFINITY_MASK64)
	*pcoefficient_x = x & SINFINITY_MASK64;
      return 0;	// NaN or Infinity
    }
    // check for non-canonical values
    if (coeff >= 10000000000000000ull)
      coeff = 0;
    *pcoefficient_x = coeff;
    // get exponent
    tmp = x >> EXPONENT_SHIFT_LARGE64;
    *pexponent_x = (int) (tmp & EXPONENT_MASK64);
    return coeff;
  }
  // exponent
  tmp = x >> EXPONENT_SHIFT_SMALL64;
  *pexponent_x = (int) (tmp & EXPONENT_MASK64);
  // coefficient
  *pcoefficient_x = (x & SMALL_COEFF_MASK64);

  return *pcoefficient_x;
}

//
//   BID64 pack macro (general form)
//
__BID_INLINE__ BID_UINT64
get_BID64 (BID_UINT64 sgn, int expon, BID_UINT64 coeff, int rmode,
	   unsigned *fpsc) {
  BID_UINT128 Stemp, Q_low;
  BID_UINT64 QH, r, mask, _C64, remainder_h, CY, carry;
  int extra_digits, amount, amount2;
  unsigned status;

  if (coeff > 9999999999999999ull) {
    expon++;
    coeff = 1000000000000000ull;
  }
  // check for possible underflow/overflow
  if (((unsigned) expon) >= 3 * 256) {
    if (expon < 0) {
      // underflow
      if (expon + MAX_FORMAT_DIGITS < 0) {
#ifdef BID_SET_STATUS_FLAGS
	__set_status_flags (fpsc,
			    BID_UNDERFLOW_EXCEPTION | BID_INEXACT_EXCEPTION);
#endif
#ifndef IEEE_ROUND_NEAREST_TIES_AWAY
#ifndef IEEE_ROUND_NEAREST
	if (rmode == BID_ROUNDING_DOWN && sgn)
	  return 0x8000000000000001ull;
	if (rmode == BID_ROUNDING_UP && !sgn)
	  return 1ull;
#endif
#endif
	// result is 0
	return sgn;
      }
#ifndef IEEE_ROUND_NEAREST_TIES_AWAY
#ifndef IEEE_ROUND_NEAREST
      if (sgn && (unsigned) (rmode - 1) < 2)
	rmode = 3 - rmode;
#endif
#endif
      // get digits to be shifted out
      extra_digits = -expon;
      coeff += bid_round_const_table[rmode][extra_digits];

      // get coeff*(2^M[extra_digits])/10^extra_digits
      __mul_64x128_full (QH, Q_low, coeff,
			 bid_reciprocals10_128[extra_digits]);

      // now get P/10^extra_digits: shift Q_high right by M[extra_digits]-128
      amount = bid_recip_scale[extra_digits];

      _C64 = QH >> amount;

#ifndef IEEE_ROUND_NEAREST_TIES_AWAY
#ifndef IEEE_ROUND_NEAREST
      if (rmode == 0)	//BID_ROUNDING_TO_NEAREST
#endif
	if (_C64 & 1) {
	  // check whether fractional part of initial_P/10^extra_digits is exactly .5

	  // get remainder
	  amount2 = 64 - amount;
	  remainder_h = 0;
	  remainder_h--;
	  remainder_h >>= amount2;
	  remainder_h = remainder_h & QH;

	  if (!remainder_h
	      && (Q_low.w[1] < bid_reciprocals10_128[extra_digits].w[1]
		  || (Q_low.w[1] == bid_reciprocals10_128[extra_digits].w[1]
		      && Q_low.w[0] <
		      bid_reciprocals10_128[extra_digits].w[0]))) {
	    _C64--;
	  }
	}
#endif

#ifdef BID_SET_STATUS_FLAGS

      if (is_inexact (fpsc))
	__set_status_flags (fpsc, BID_UNDERFLOW_EXCEPTION);
      else {
	status = BID_INEXACT_EXCEPTION;
	// get remainder
	remainder_h = QH << (64 - amount);

	switch (rmode) {
	case BID_ROUNDING_TO_NEAREST:
	case BID_ROUNDING_TIES_AWAY:
	  // test whether fractional part is 0
	  if (remainder_h == 0x8000000000000000ull
	      && (Q_low.w[1] < bid_reciprocals10_128[extra_digits].w[1]
		  || (Q_low.w[1] == bid_reciprocals10_128[extra_digits].w[1]
		      && Q_low.w[0] <
		      bid_reciprocals10_128[extra_digits].w[0])))
	    status = BID_EXACT_STATUS;
	  break;
	case BID_ROUNDING_DOWN:
	case BID_ROUNDING_TO_ZERO:
	  if (!remainder_h
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
	  if ((remainder_h >> (64 - amount)) + carry >=
	      (((BID_UINT64) 1) << amount))
	    status = BID_EXACT_STATUS;
	}

	if (status != BID_EXACT_STATUS)
	  __set_status_flags (fpsc, BID_UNDERFLOW_EXCEPTION | status);
      }

#endif

      return sgn | _C64;
    }
    if(!coeff) { if(expon > DECIMAL_MAX_EXPON_64) expon = DECIMAL_MAX_EXPON_64; }
    while (coeff < 1000000000000000ull && expon >= 3 * 256) {
      expon--;
      coeff = (coeff << 3) + (coeff << 1);
    }
    if (expon > DECIMAL_MAX_EXPON_64) {
#ifdef BID_SET_STATUS_FLAGS
      __set_status_flags (fpsc, BID_OVERFLOW_EXCEPTION | BID_INEXACT_EXCEPTION);
#endif
      // overflow
      r = sgn | INFINITY_MASK64;
      switch (rmode) {
      case BID_ROUNDING_DOWN:
	if (!sgn)
	  r = LARGEST_BID64;
	break;
      case BID_ROUNDING_TO_ZERO:
	r = sgn | LARGEST_BID64;
	break;
      case BID_ROUNDING_UP:
	// round up
	if (sgn)
	  r = SMALLEST_BID64;
      }
      return r;
    }
  }

  mask = 1;
  mask <<= EXPONENT_SHIFT_SMALL64;

  // check whether coefficient fits in 10*5+3 bits
  if (coeff < mask) {
    r = expon;
    r <<= EXPONENT_SHIFT_SMALL64;
    r |= (coeff | sgn);
    return r;
  }
  // special format

  // eliminate the case coeff==10^16 after rounding
  if (coeff == 10000000000000000ull) {
    r = expon + 1;
    r <<= EXPONENT_SHIFT_SMALL64;
    r |= (1000000000000000ull | sgn);
    return r;
  }

  r = expon;
  r <<= EXPONENT_SHIFT_LARGE64;
  r |= (sgn | SPECIAL_ENCODING_MASK64);
  // add coeff, without leading bits
  mask = (mask >> 2) - 1;
  coeff &= mask;
  r |= coeff;

  return r;
}




//
//   No overflow/underflow checking 
//
__BID_INLINE__ BID_UINT64
fast_get_BID64 (BID_UINT64 sgn, int expon, BID_UINT64 coeff) {
  BID_UINT64 r, mask;

  mask = 1;
  mask <<= EXPONENT_SHIFT_SMALL64;

  // check whether coefficient fits in 10*5+3 bits
  if (coeff < mask) {
    r = expon;
    r <<= EXPONENT_SHIFT_SMALL64;
    r |= (coeff | sgn);
    return r;
  }
  // special format

  // eliminate the case coeff==10^16 after rounding
  if (coeff == 10000000000000000ull) {
    r = expon + 1;
    r <<= EXPONENT_SHIFT_SMALL64;
    r |= (1000000000000000ull | sgn);
    return r;
  }

  r = expon;
  r <<= EXPONENT_SHIFT_LARGE64;
  r |= (sgn | SPECIAL_ENCODING_MASK64);
  // add coeff, without leading bits
  mask = (mask >> 2) - 1;
  coeff &= mask;
  r |= coeff;

  return r;
}


//
//   no underflow checking
//
__BID_INLINE__ BID_UINT64
fast_get_BID64_check_OF (BID_UINT64 sgn, int expon, BID_UINT64 coeff, int rmode,
			 unsigned *fpsc) {
  BID_UINT64 r, mask;

  if (((unsigned) expon) >= 3 * 256 - 1) {
    if ((expon == 3 * 256 - 1) && coeff == 10000000000000000ull) {
      expon = 3 * 256;
      coeff = 1000000000000000ull;
    }

    if (((unsigned) expon) >= 3 * 256) {
      while (coeff < 1000000000000000ull && expon >= 3 * 256) {
	expon--;
	coeff = (coeff << 3) + (coeff << 1);
      }
      if (expon > DECIMAL_MAX_EXPON_64) {
#ifdef BID_SET_STATUS_FLAGS
	__set_status_flags (fpsc,
			    BID_OVERFLOW_EXCEPTION | BID_INEXACT_EXCEPTION);
#endif
	// overflow
	r = sgn | INFINITY_MASK64;
	switch (rmode) {
	case BID_ROUNDING_DOWN:
	  if (!sgn)
	    r = LARGEST_BID64;
	  break;
	case BID_ROUNDING_TO_ZERO:
	  r = sgn | LARGEST_BID64;
	  break;
	case BID_ROUNDING_UP:
	  // round up
	  if (sgn)
	    r = SMALLEST_BID64;
	}
	return r;
      }
    }
  }

  mask = 1;
  mask <<= EXPONENT_SHIFT_SMALL64;

  // check whether coefficient fits in 10*5+3 bits
  if (coeff < mask) {
    r = expon;
    r <<= EXPONENT_SHIFT_SMALL64;
    r |= (coeff | sgn);
    return r;
  }
  // special format

  // eliminate the case coeff==10^16 after rounding
  if (coeff == 10000000000000000ull) {
    r = expon + 1;
    r <<= EXPONENT_SHIFT_SMALL64;
    r |= (1000000000000000ull | sgn);
    return r;
  }

  r = expon;
  r <<= EXPONENT_SHIFT_LARGE64;
  r |= (sgn | SPECIAL_ENCODING_MASK64);
  // add coeff, without leading bits
  mask = (mask >> 2) - 1;
  coeff &= mask;
  r |= coeff;

  return r;
}


//
//   No overflow/underflow checking 
//   or checking for coefficients equal to 10^16 (after rounding)
//
__BID_INLINE__ BID_UINT64
very_fast_get_BID64 (BID_UINT64 sgn, int expon, BID_UINT64 coeff) {
  BID_UINT64 r, mask;

  mask = 1;
  mask <<= EXPONENT_SHIFT_SMALL64;

  // check whether coefficient fits in 10*5+3 bits
  if (coeff < mask) {
    r = expon;
    r <<= EXPONENT_SHIFT_SMALL64;
    r |= (coeff | sgn);
    return r;
  }
  // special format
  r = expon;
  r <<= EXPONENT_SHIFT_LARGE64;
  r |= (sgn | SPECIAL_ENCODING_MASK64);
  // add coeff, without leading bits
  mask = (mask >> 2) - 1;
  coeff &= mask;
  r |= coeff;

  return r;
}

//
//   No overflow/underflow checking or checking for coefficients above 2^53
//
__BID_INLINE__ BID_UINT64
very_fast_get_BID64_small_mantissa (BID_UINT64 sgn, int expon, BID_UINT64 coeff) {
  // no UF/OF
  BID_UINT64 r;

  r = expon;
  r <<= EXPONENT_SHIFT_SMALL64;
  r |= (coeff | sgn);
  return r;
}


//
// This pack macro is used when underflow is known to occur
//
__BID_INLINE__ BID_UINT64
get_BID64_UF (BID_UINT64 sgn, int expon, BID_UINT64 coeff, BID_UINT64 R, int rmode,
	      unsigned *fpsc) {
  BID_UINT128 C128, Q_low, Stemp;
  BID_UINT64 _C64, remainder_h, QH, carry, CY;
  int extra_digits, amount, amount2;
  unsigned status;

  // underflow
  if (expon + MAX_FORMAT_DIGITS < 0) {
#ifdef BID_SET_STATUS_FLAGS
    __set_status_flags (fpsc, BID_UNDERFLOW_EXCEPTION | BID_INEXACT_EXCEPTION);
#endif
#ifndef IEEE_ROUND_NEAREST_TIES_AWAY
#ifndef IEEE_ROUND_NEAREST
    if (rmode == BID_ROUNDING_DOWN && sgn)
      return 0x8000000000000001ull;
    if (rmode == BID_ROUNDING_UP && !sgn)
      return 1ull;
#endif
#endif
    // result is 0
    return sgn;
  }
  // 10*coeff
  coeff = (coeff << 3) + (coeff << 1);
#ifndef IEEE_ROUND_NEAREST_TIES_AWAY
#ifndef IEEE_ROUND_NEAREST
  if (sgn && (unsigned) (rmode - 1) < 2)
    rmode = 3 - rmode;
#endif
#endif
  if (R)
    coeff |= 1;
  // get digits to be shifted out
  extra_digits = 1 - expon;
  C128.w[0] = coeff + bid_round_const_table[rmode][extra_digits];

  // get coeff*(2^M[extra_digits])/10^extra_digits
  __mul_64x128_full (QH, Q_low, C128.w[0],
		     bid_reciprocals10_128[extra_digits]);

  // now get P/10^extra_digits: shift Q_high right by M[extra_digits]-128
  amount = bid_recip_scale[extra_digits];

  _C64 = QH >> amount;
  //__shr_128(C128, Q_high, amount); 

#ifndef IEEE_ROUND_NEAREST_TIES_AWAY
#ifndef IEEE_ROUND_NEAREST
  if (rmode == 0)	//BID_ROUNDING_TO_NEAREST
#endif
    if (_C64 & 1) {
      // check whether fractional part of initial_P/10^extra_digits is exactly .5

      // get remainder
      amount2 = 64 - amount;
      remainder_h = 0;
      remainder_h--;
      remainder_h >>= amount2;
      remainder_h = remainder_h & QH;

      if (!remainder_h
	  && (Q_low.w[1] < bid_reciprocals10_128[extra_digits].w[1]
	      || (Q_low.w[1] == bid_reciprocals10_128[extra_digits].w[1]
		  && Q_low.w[0] <
		  bid_reciprocals10_128[extra_digits].w[0]))) {
	_C64--;
      }
    }
#endif

#ifdef BID_SET_STATUS_FLAGS

  if (is_inexact (fpsc))
    __set_status_flags (fpsc, BID_UNDERFLOW_EXCEPTION);
  else {
    status = BID_INEXACT_EXCEPTION;
    // get remainder
    remainder_h = QH << (64 - amount);

    switch (rmode) {
    case BID_ROUNDING_TO_NEAREST:
    case BID_ROUNDING_TIES_AWAY:
      // test whether fractional part is 0
      if (remainder_h == 0x8000000000000000ull
	  && (Q_low.w[1] < bid_reciprocals10_128[extra_digits].w[1]
	      || (Q_low.w[1] == bid_reciprocals10_128[extra_digits].w[1]
		  && Q_low.w[0] <
		  bid_reciprocals10_128[extra_digits].w[0])))
	status = BID_EXACT_STATUS;
      break;
    case BID_ROUNDING_DOWN:
    case BID_ROUNDING_TO_ZERO:
      if (!remainder_h
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
      if ((remainder_h >> (64 - amount)) + carry >=
	  (((BID_UINT64) 1) << amount))
	status = BID_EXACT_STATUS;
    }

    if (status != BID_EXACT_STATUS)
      __set_status_flags (fpsc, BID_UNDERFLOW_EXCEPTION | status);
  }

#endif

  return sgn | _C64;

}



//
//   This pack macro doesnot check for coefficients above 2^53 
//
__BID_INLINE__ BID_UINT64
get_BID64_small_mantissa (BID_UINT64 sgn, int expon, BID_UINT64 coeff,
			  int rmode, unsigned *fpsc) {
  BID_UINT128 C128, Q_low, Stemp;
  BID_UINT64 r, mask, _C64, remainder_h, QH, carry, CY;
  int extra_digits, amount, amount2;
  unsigned status;

  // check for possible underflow/overflow
  if (((unsigned) expon) >= 3 * 256) {
    if (expon < 0) {
      // underflow
      if (expon + MAX_FORMAT_DIGITS < 0) {
#ifdef BID_SET_STATUS_FLAGS
	__set_status_flags (fpsc,
			    BID_UNDERFLOW_EXCEPTION | BID_INEXACT_EXCEPTION);
#endif
#ifndef IEEE_ROUND_NEAREST_TIES_AWAY
#ifndef IEEE_ROUND_NEAREST
	if (rmode == BID_ROUNDING_DOWN && sgn)
	  return 0x8000000000000001ull;
	if (rmode == BID_ROUNDING_UP && !sgn)
	  return 1ull;
#endif
#endif
	// result is 0
	return sgn;
      }
#ifndef IEEE_ROUND_NEAREST_TIES_AWAY
#ifndef IEEE_ROUND_NEAREST
      if (sgn && (unsigned) (rmode - 1) < 2)
	rmode = 3 - rmode;
#endif
#endif
      // get digits to be shifted out
      extra_digits = -expon;
      C128.w[0] = coeff + bid_round_const_table[rmode][extra_digits];

      // get coeff*(2^M[extra_digits])/10^extra_digits
      __mul_64x128_full (QH, Q_low, C128.w[0],
			 bid_reciprocals10_128[extra_digits]);

      // now get P/10^extra_digits: shift Q_high right by M[extra_digits]-128
      amount = bid_recip_scale[extra_digits];

      _C64 = QH >> amount;

#ifndef IEEE_ROUND_NEAREST_TIES_AWAY
#ifndef IEEE_ROUND_NEAREST
      if (rmode == 0)	//BID_ROUNDING_TO_NEAREST
#endif
	if (_C64 & 1) {
	  // check whether fractional part of initial_P/10^extra_digits is exactly .5

	  // get remainder
	  amount2 = 64 - amount;
	  remainder_h = 0;
	  remainder_h--;
	  remainder_h >>= amount2;
	  remainder_h = remainder_h & QH;

	  if (!remainder_h
	      && (Q_low.w[1] < bid_reciprocals10_128[extra_digits].w[1]
		  || (Q_low.w[1] == bid_reciprocals10_128[extra_digits].w[1]
		      && Q_low.w[0] <
		      bid_reciprocals10_128[extra_digits].w[0]))) {
	    _C64--;
	  }
	}
#endif

#ifdef BID_SET_STATUS_FLAGS

      if (is_inexact (fpsc))
	__set_status_flags (fpsc, BID_UNDERFLOW_EXCEPTION);
      else {
	status = BID_INEXACT_EXCEPTION;
	// get remainder
	remainder_h = QH << (64 - amount);

	switch (rmode) {
	case BID_ROUNDING_TO_NEAREST:
	case BID_ROUNDING_TIES_AWAY:
	  // test whether fractional part is 0
	  if (remainder_h == 0x8000000000000000ull
	      && (Q_low.w[1] < bid_reciprocals10_128[extra_digits].w[1]
		  || (Q_low.w[1] == bid_reciprocals10_128[extra_digits].w[1]
		      && Q_low.w[0] <
		      bid_reciprocals10_128[extra_digits].w[0])))
	    status = BID_EXACT_STATUS;
	  break;
	case BID_ROUNDING_DOWN:
	case BID_ROUNDING_TO_ZERO:
	  if (!remainder_h
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
	  if ((remainder_h >> (64 - amount)) + carry >=
	      (((BID_UINT64) 1) << amount))
	    status = BID_EXACT_STATUS;
	}

	if (status != BID_EXACT_STATUS)
	  __set_status_flags (fpsc, BID_UNDERFLOW_EXCEPTION | status);
      }

#endif

      return sgn | _C64;
    }

    while (coeff < 1000000000000000ull && expon >= 3 * 256) {
      expon--;
      coeff = (coeff << 3) + (coeff << 1);
    }
    if (expon > DECIMAL_MAX_EXPON_64) {
#ifdef BID_SET_STATUS_FLAGS
      __set_status_flags (fpsc, BID_OVERFLOW_EXCEPTION | BID_INEXACT_EXCEPTION);
#endif
      // overflow
      r = sgn | INFINITY_MASK64;
      switch (rmode) {
      case BID_ROUNDING_DOWN:
	if (!sgn)
	  r = LARGEST_BID64;
	break;
      case BID_ROUNDING_TO_ZERO:
	r = sgn | LARGEST_BID64;
	break;
      case BID_ROUNDING_UP:
	// round up
	if (sgn)
	  r = SMALLEST_BID64;
      }
      return r;
    } else {
      mask = 1;
      mask <<= EXPONENT_SHIFT_SMALL64;
      if (coeff >= mask) {
	r = expon;
	r <<= EXPONENT_SHIFT_LARGE64;
	r |= (sgn | SPECIAL_ENCODING_MASK64);
	// add coeff, without leading bits
	mask = (mask >> 2) - 1;
	coeff &= mask;
	r |= coeff;
	return r;
      }
    }
  }

  r = expon;
  r <<= EXPONENT_SHIFT_SMALL64;
  r |= (coeff | sgn);

  return r;
}


/*****************************************************************************
*
*    BID128 pack/unpack macros
*
*****************************************************************************/

//
//   Macro for handling BID128 underflow
//         sticky bit given as additional argument
//
__BID_INLINE__ BID_UINT128 *
bid_handle_UF_128_rem (BID_UINT128 * pres, BID_UINT64 sgn, int expon, BID_UINT128 CQ,
		   BID_UINT64 R, unsigned *prounding_mode, unsigned *fpsc) {
  BID_UINT128 T128, TP128, Qh, Ql, Qh1, Stemp, Tmp, Tmp1, CQ2, CQ8;
  BID_UINT64 carry, CY;
  int ed2, amount;
  unsigned rmode, status;

  // UF occurs
  if (expon + MAX_FORMAT_DIGITS_128 < 0) {
#ifdef BID_SET_STATUS_FLAGS
    __set_status_flags (fpsc, BID_UNDERFLOW_EXCEPTION | BID_INEXACT_EXCEPTION);
#endif
    pres->w[1] = sgn;
    pres->w[0] = 0;
#ifndef IEEE_ROUND_NEAREST_TIES_AWAY
#ifndef IEEE_ROUND_NEAREST
    if ((sgn && *prounding_mode == BID_ROUNDING_DOWN)
	|| (!sgn && *prounding_mode == BID_ROUNDING_UP))
      pres->w[0] = 1ull;
#endif
#endif
    return pres;
  }
  // CQ *= 10
  CQ2.w[1] = (CQ.w[1] << 1) | (CQ.w[0] >> 63);
  CQ2.w[0] = CQ.w[0] << 1;
  CQ8.w[1] = (CQ.w[1] << 3) | (CQ.w[0] >> 61);
  CQ8.w[0] = CQ.w[0] << 3;
  __add_128_128 (CQ, CQ2, CQ8);

  // add remainder
  if (R)
    CQ.w[0] |= 1;

  ed2 = 1 - expon;
  // add rounding constant to CQ
#ifndef IEEE_ROUND_NEAREST_TIES_AWAY
#ifndef IEEE_ROUND_NEAREST
  rmode = *prounding_mode;
  if (sgn && (unsigned) (rmode - 1) < 2)
    rmode = 3 - rmode;
#else
  rmode = 0;
#endif
#else
  rmode = 0;
#endif
  T128 = bid_round_const_table_128[rmode][ed2];
  __add_carry_out (CQ.w[0], carry, T128.w[0], CQ.w[0]);
  CQ.w[1] = CQ.w[1] + T128.w[1] + carry;

  TP128 = bid_reciprocals10_128[ed2];
  __mul_128x128_full (Qh, Ql, CQ, TP128);
  amount = bid_recip_scale[ed2];

  if (amount >= 64) {
    CQ.w[0] = Qh.w[1] >> (amount - 64);
    CQ.w[1] = 0;
  } else {
    __shr_128 (CQ, Qh, amount);
  }

  expon = 0;

#ifndef IEEE_ROUND_NEAREST_TIES_AWAY
#ifndef IEEE_ROUND_NEAREST
  if (!(*prounding_mode))
#endif
    if (CQ.w[0] & 1) {
      // check whether fractional part of initial_P/10^ed1 is exactly .5

      // get remainder
      __shl_128_long (Qh1, Qh, (128 - amount));

      if (!Qh1.w[1] && !Qh1.w[0]
	  && (Ql.w[1] < bid_reciprocals10_128[ed2].w[1]
	      || (Ql.w[1] == bid_reciprocals10_128[ed2].w[1]
		  && Ql.w[0] < bid_reciprocals10_128[ed2].w[0]))) {
	CQ.w[0]--;
      }
    }
#endif

#ifdef BID_SET_STATUS_FLAGS

  if (is_inexact (fpsc))
    __set_status_flags (fpsc, BID_UNDERFLOW_EXCEPTION);
  else {
    status = BID_INEXACT_EXCEPTION;
    // get remainder
    __shl_128_long (Qh1, Qh, (128 - amount));

    switch (rmode) {
    case BID_ROUNDING_TO_NEAREST:
    case BID_ROUNDING_TIES_AWAY:
      // test whether fractional part is 0
      if (Qh1.w[1] == 0x8000000000000000ull && (!Qh1.w[0])
	  && (Ql.w[1] < bid_reciprocals10_128[ed2].w[1]
	      || (Ql.w[1] == bid_reciprocals10_128[ed2].w[1]
		  && Ql.w[0] < bid_reciprocals10_128[ed2].w[0])))
	status = BID_EXACT_STATUS;
      break;
    case BID_ROUNDING_DOWN:
    case BID_ROUNDING_TO_ZERO:
      if ((!Qh1.w[1]) && (!Qh1.w[0])
	  && (Ql.w[1] < bid_reciprocals10_128[ed2].w[1]
	      || (Ql.w[1] == bid_reciprocals10_128[ed2].w[1]
		  && Ql.w[0] < bid_reciprocals10_128[ed2].w[0])))
	status = BID_EXACT_STATUS;
      break;
    default:
      // round up
      __add_carry_out (Stemp.w[0], CY, Ql.w[0],
		       bid_reciprocals10_128[ed2].w[0]);
      __add_carry_in_out (Stemp.w[1], carry, Ql.w[1],
			  bid_reciprocals10_128[ed2].w[1], CY);
      __shr_128_long (Qh, Qh1, (128 - amount));
      Tmp.w[0] = 1;
      Tmp.w[1] = 0;
      __shl_128_long (Tmp1, Tmp, amount);
      Qh.w[0] += carry;
      if (Qh.w[0] < carry)
	Qh.w[1]++;
      if (__unsigned_compare_ge_128 (Qh, Tmp1))
	status = BID_EXACT_STATUS;
    }

    if (status != BID_EXACT_STATUS)
      __set_status_flags (fpsc, BID_UNDERFLOW_EXCEPTION | status);
  }

#endif

  pres->w[1] = sgn | CQ.w[1];
  pres->w[0] = CQ.w[0];

  return pres;

}


//
//   Macro for handling BID128 underflow
//
__BID_INLINE__ BID_UINT128 *
handle_UF_128 (BID_UINT128 * pres, BID_UINT64 sgn, int expon, BID_UINT128 CQ,
	       unsigned *prounding_mode, unsigned *fpsc) {
  BID_UINT128 T128, TP128, Qh, Ql, Qh1, Stemp, Tmp, Tmp1;
  BID_UINT64 carry, CY;
  int ed2, amount;
  unsigned rmode, status;

  // UF occurs
  if (expon + MAX_FORMAT_DIGITS_128 < 0) {
#ifdef BID_SET_STATUS_FLAGS
    __set_status_flags (fpsc, BID_UNDERFLOW_EXCEPTION | BID_INEXACT_EXCEPTION);
#endif
    pres->w[1] = sgn;
    pres->w[0] = 0;
#ifndef IEEE_ROUND_NEAREST_TIES_AWAY
#ifndef IEEE_ROUND_NEAREST
    if ((sgn && *prounding_mode == BID_ROUNDING_DOWN)
	|| (!sgn && *prounding_mode == BID_ROUNDING_UP))
      pres->w[0] = 1ull;
#endif
#endif
    return pres;
  }

  ed2 = 0 - expon;
  // add rounding constant to CQ
#ifndef IEEE_ROUND_NEAREST_TIES_AWAY
#ifndef IEEE_ROUND_NEAREST
  rmode = *prounding_mode;
  if (sgn && (unsigned) (rmode - 1) < 2)
    rmode = 3 - rmode;
#else
  rmode = 0;
#endif
#else
  rmode = 0;
#endif

  T128 = bid_round_const_table_128[rmode][ed2];
  __add_carry_out (CQ.w[0], carry, T128.w[0], CQ.w[0]);
  CQ.w[1] = CQ.w[1] + T128.w[1] + carry;

  TP128 = bid_reciprocals10_128[ed2];
  __mul_128x128_full (Qh, Ql, CQ, TP128);
  amount = bid_recip_scale[ed2];

  if (amount >= 64) {
    CQ.w[0] = Qh.w[1] >> (amount - 64);
    CQ.w[1] = 0;
  } else {
    __shr_128 (CQ, Qh, amount);
  }

  expon = 0;

#ifndef IEEE_ROUND_NEAREST_TIES_AWAY
#ifndef IEEE_ROUND_NEAREST
  if (!(*prounding_mode))
#endif
    if (CQ.w[0] & 1) {
      // check whether fractional part of initial_P/10^ed1 is exactly .5

      // get remainder
      __shl_128_long (Qh1, Qh, (128 - amount));

      if (!Qh1.w[1] && !Qh1.w[0]
	  && (Ql.w[1] < bid_reciprocals10_128[ed2].w[1]
	      || (Ql.w[1] == bid_reciprocals10_128[ed2].w[1]
		  && Ql.w[0] < bid_reciprocals10_128[ed2].w[0]))) {
	CQ.w[0]--;
      }
    }
#endif

#ifdef BID_SET_STATUS_FLAGS

  if (is_inexact (fpsc))
    __set_status_flags (fpsc, BID_UNDERFLOW_EXCEPTION);
  else {
    status = BID_INEXACT_EXCEPTION;
    // get remainder
    __shl_128_long (Qh1, Qh, (128 - amount));

    switch (rmode) {
    case BID_ROUNDING_TO_NEAREST:
    case BID_ROUNDING_TIES_AWAY:
      // test whether fractional part is 0
      if (Qh1.w[1] == 0x8000000000000000ull && (!Qh1.w[0])
	  && (Ql.w[1] < bid_reciprocals10_128[ed2].w[1]
	      || (Ql.w[1] == bid_reciprocals10_128[ed2].w[1]
		  && Ql.w[0] < bid_reciprocals10_128[ed2].w[0])))
	status = BID_EXACT_STATUS;
      break;
    case BID_ROUNDING_DOWN:
    case BID_ROUNDING_TO_ZERO:
      if ((!Qh1.w[1]) && (!Qh1.w[0])
	  && (Ql.w[1] < bid_reciprocals10_128[ed2].w[1]
	      || (Ql.w[1] == bid_reciprocals10_128[ed2].w[1]
		  && Ql.w[0] < bid_reciprocals10_128[ed2].w[0])))
	status = BID_EXACT_STATUS;
      break;
    default:
      // round up
      __add_carry_out (Stemp.w[0], CY, Ql.w[0],
		       bid_reciprocals10_128[ed2].w[0]);
      __add_carry_in_out (Stemp.w[1], carry, Ql.w[1],
			  bid_reciprocals10_128[ed2].w[1], CY);
      __shr_128_long (Qh, Qh1, (128 - amount));
      Tmp.w[0] = 1;
      Tmp.w[1] = 0;
      __shl_128_long (Tmp1, Tmp, amount);
      Qh.w[0] += carry;
      if (Qh.w[0] < carry)
	Qh.w[1]++;
      if (__unsigned_compare_ge_128 (Qh, Tmp1))
	status = BID_EXACT_STATUS;
    }

    if (status != BID_EXACT_STATUS)
      __set_status_flags (fpsc, BID_UNDERFLOW_EXCEPTION | status);
  }

#endif

  pres->w[1] = sgn | CQ.w[1];
  pres->w[0] = CQ.w[0];

  return pres;

}



//
//  BID128 unpack, input passed by value  (used in transcendental functions only)
//
__BID_INLINE__ BID_UINT64
unpack_BID128_value_BLE (BID_UINT64 * psign_x, int *pexponent_x,
		     BID_UINT128 * pcoefficient_x, BID_UINT128 x) {
  BID_UINT128 coeff, T33, T34;
  BID_UINT64 ex;

  *psign_x = (x.w[BID_HIGH_128W]) & 0x8000000000000000ull;

  // special encodings
  if ((x.w[BID_HIGH_128W] & INFINITY_MASK64) >= SPECIAL_ENCODING_MASK64) {
    if ((x.w[BID_HIGH_128W] & INFINITY_MASK64) < INFINITY_MASK64) {
      // non-canonical input
      pcoefficient_x->w[BID_LOW_128W] = 0;
      pcoefficient_x->w[BID_HIGH_128W] = 0;
      ex = (x.w[BID_HIGH_128W]) >> 47;
      *pexponent_x = ((int) ex) & EXPONENT_MASK128;
      return 0;
    }
    // 10^33
    T33 = bid_power10_table_128[33];

    pcoefficient_x->w[BID_LOW_128W] = x.w[BID_LOW_128W];
    pcoefficient_x->w[BID_HIGH_128W] = (x.w[BID_HIGH_128W]) & 0x00003fffffffffffull;
	if ((pcoefficient_x->w[BID_HIGH_128W]>T33.w[1]) || ((pcoefficient_x->w[BID_HIGH_128W]==T33.w[1]) && (pcoefficient_x->w[BID_LOW_128W]>=T33.w[0]))) // non-canonical
    {
      pcoefficient_x->w[BID_HIGH_128W] = (x.w[BID_HIGH_128W]) & 0xfe00000000000000ull;
      pcoefficient_x->w[BID_LOW_128W] = 0;
    } else
      pcoefficient_x->w[BID_HIGH_128W] = (x.w[BID_HIGH_128W]) & 0xfe003fffffffffffull;
    if ((x.w[BID_HIGH_128W] & NAN_MASK64) == INFINITY_MASK64) {
      pcoefficient_x->w[BID_LOW_128W] = 0;
      pcoefficient_x->w[BID_HIGH_128W] = x.w[BID_HIGH_128W] & SINFINITY_MASK64;
    }
    *pexponent_x = 0;
    return 0;	// NaN or Infinity 
  }

  coeff.w[BID_LOW_128W] = x.w[BID_LOW_128W];
  coeff.w[BID_HIGH_128W] = (x.w[BID_HIGH_128W]) & SMALL_COEFF_MASK128;

  // 10^34
  T34 = bid_power10_table_128[34];
  // check for non-canonical values
  if ((coeff.w[BID_HIGH_128W]>T34.w[1]) || ((coeff.w[BID_HIGH_128W]==T34.w[1]) && (coeff.w[BID_LOW_128W]>=T34.w[0]))) {
	  coeff.w[BID_LOW_128W] = coeff.w[BID_HIGH_128W] = 0;}

  pcoefficient_x->w[BID_LOW_128W] = coeff.w[BID_LOW_128W];
  pcoefficient_x->w[BID_HIGH_128W] = coeff.w[BID_HIGH_128W];

  ex = (x.w[BID_HIGH_128W]) >> 49;
  *pexponent_x = ((int) ex) & EXPONENT_MASK128;

  return coeff.w[BID_LOW_128W] | coeff.w[BID_HIGH_128W];
}



//
//  BID128 unpack, input passed by value
//
__BID_INLINE__ BID_UINT64
unpack_BID128_value (BID_UINT64 * psign_x, int *pexponent_x,
		     BID_UINT128 * pcoefficient_x, BID_UINT128 x) {
  BID_UINT128 coeff, T33, T34;
  BID_UINT64 ex;

  *psign_x = (x.w[1]) & 0x8000000000000000ull;

  // special encodings
  if ((x.w[1] & INFINITY_MASK64) >= SPECIAL_ENCODING_MASK64) {
    if ((x.w[1] & INFINITY_MASK64) < INFINITY_MASK64) {
      // non-canonical input
      pcoefficient_x->w[0] = 0;
      pcoefficient_x->w[1] = 0;
      ex = (x.w[1]) >> 47;
      *pexponent_x = ((int) ex) & EXPONENT_MASK128;
      return 0;
    }
    // 10^33
    T33 = bid_power10_table_128[33];
    /*coeff.w[0] = x.w[0];
       coeff.w[1] = (x.w[1]) & LARGE_COEFF_MASK128;
       pcoefficient_x->w[0] = x.w[0];
       pcoefficient_x->w[1] = x.w[1];
       if (__unsigned_compare_ge_128 (coeff, T33)) // non-canonical
       pcoefficient_x->w[1] &= (~LARGE_COEFF_MASK128); */

    pcoefficient_x->w[0] = x.w[0];
    pcoefficient_x->w[1] = (x.w[1]) & 0x00003fffffffffffull;
    if (__unsigned_compare_ge_128 ((*pcoefficient_x), T33))	// non-canonical
    {
      pcoefficient_x->w[1] = (x.w[1]) & 0xfe00000000000000ull;
      pcoefficient_x->w[0] = 0;
    } else
      pcoefficient_x->w[1] = (x.w[1]) & 0xfe003fffffffffffull;
    if ((x.w[1] & NAN_MASK64) == INFINITY_MASK64) {
      pcoefficient_x->w[0] = 0;
      pcoefficient_x->w[1] = x.w[1] & SINFINITY_MASK64;
    }
    *pexponent_x = 0;
    return 0;	// NaN or Infinity 
  }

  coeff.w[0] = x.w[0];
  coeff.w[1] = (x.w[1]) & SMALL_COEFF_MASK128;

  // 10^34
  T34 = bid_power10_table_128[34];
  // check for non-canonical values
  if (__unsigned_compare_ge_128 (coeff, T34))
    coeff.w[0] = coeff.w[1] = 0;

  pcoefficient_x->w[0] = coeff.w[0];
  pcoefficient_x->w[1] = coeff.w[1];

  ex = (x.w[1]) >> 49;
  *pexponent_x = ((int) ex) & EXPONENT_MASK128;

  return coeff.w[0] | coeff.w[1];
}

//
//  BID128 unpack, input passed by value
//
__BID_INLINE__ void
quick_unpack_BID128_em (int *pexponent_x,
		     BID_UINT128 * pcoefficient_x, BID_UINT128 x) {
  BID_UINT64 ex;

  pcoefficient_x->w[0] = x.w[0];
  pcoefficient_x->w[1] = (x.w[1]) & SMALL_COEFF_MASK128;

  ex = (x.w[1]) >> 49;
  *pexponent_x = ((int) ex) & EXPONENT_MASK128;

}


//
//  BID128 unpack, input pased by reference
//
__BID_INLINE__ BID_UINT64
unpack_BID128 (BID_UINT64 * psign_x, int *pexponent_x,
	       BID_UINT128 * pcoefficient_x, BID_UINT128 * px) {
  BID_UINT128 coeff, T33, T34;
  BID_UINT64 ex;

  *psign_x = (px->w[1]) & 0x8000000000000000ull;

  // special encodings
  if ((px->w[1] & INFINITY_MASK64) >= SPECIAL_ENCODING_MASK64) {
    if ((px->w[1] & INFINITY_MASK64) < INFINITY_MASK64) {
      // non-canonical input
      pcoefficient_x->w[0] = 0;
      pcoefficient_x->w[1] = 0;
      ex = (px->w[1]) >> 47;
      *pexponent_x = ((int) ex) & EXPONENT_MASK128;
      return 0;
    }
    // 10^33
    T33 = bid_power10_table_128[33];
    coeff.w[0] = px->w[0];
    coeff.w[1] = (px->w[1]) & LARGE_COEFF_MASK128;
    pcoefficient_x->w[0] = px->w[0];
    pcoefficient_x->w[1] = px->w[1];
    if (__unsigned_compare_ge_128 (coeff, T33)) {	// non-canonical
      pcoefficient_x->w[1] &= (~LARGE_COEFF_MASK128);
      pcoefficient_x->w[0] = 0;
    }
    *pexponent_x = 0;
    return 0;	// NaN or Infinity 
  }

  coeff.w[0] = px->w[0];
  coeff.w[1] = (px->w[1]) & SMALL_COEFF_MASK128;

  // 10^34
  T34 = bid_power10_table_128[34];
  // check for non-canonical values
  if (__unsigned_compare_ge_128 (coeff, T34))
    coeff.w[0] = coeff.w[1] = 0;

  pcoefficient_x->w[0] = coeff.w[0];
  pcoefficient_x->w[1] = coeff.w[1];

  ex = (px->w[1]) >> 49;
  *pexponent_x = ((int) ex) & EXPONENT_MASK128;

  return coeff.w[0] | coeff.w[1];
}

//
//   Pack macro checks for overflow, but not underflow
//
__BID_INLINE__ BID_UINT128 *
bid_get_BID128_very_fast_OF (BID_UINT128 * pres, BID_UINT64 sgn, int expon,
			 BID_UINT128 coeff, unsigned *prounding_mode,
			 unsigned *fpsc) {
  BID_UINT128 T;
  BID_UINT64 tmp, tmp2;

  if ((unsigned) expon > DECIMAL_MAX_EXPON_128) {

    if (expon - MAX_FORMAT_DIGITS_128 <= DECIMAL_MAX_EXPON_128) {
      T = bid_power10_table_128[MAX_FORMAT_DIGITS_128 - 1];
      while (__unsigned_compare_gt_128 (T, coeff)
	     && expon > DECIMAL_MAX_EXPON_128) {
	coeff.w[1] =
	  (coeff.w[1] << 3) + (coeff.w[1] << 1) + (coeff.w[0] >> 61) +
	  (coeff.w[0] >> 63);
	tmp2 = coeff.w[0] << 3;
	coeff.w[0] = (coeff.w[0] << 1) + tmp2;
	if (coeff.w[0] < tmp2)
	  coeff.w[1]++;

	expon--;
      }
    }
    if ((unsigned) expon > DECIMAL_MAX_EXPON_128) {
      // OF
#ifdef BID_SET_STATUS_FLAGS
      __set_status_flags (fpsc, BID_OVERFLOW_EXCEPTION | BID_INEXACT_EXCEPTION);
#endif
#ifndef IEEE_ROUND_NEAREST_TIES_AWAY
#ifndef IEEE_ROUND_NEAREST
      if (*prounding_mode == BID_ROUNDING_TO_ZERO
	  || (sgn && *prounding_mode == BID_ROUNDING_UP) || (!sgn
							 &&
							 *prounding_mode
							 ==
							 BID_ROUNDING_DOWN))
      {
	pres->w[1] = sgn | LARGEST_BID128_HIGH;
	pres->w[0] = LARGEST_BID128_LOW;
      } else
#endif
#endif
      {
	pres->w[1] = sgn | INFINITY_MASK64;
	pres->w[0] = 0;
      }
      return pres;
    }
  }

  pres->w[0] = coeff.w[0];
  tmp = expon;
  tmp <<= 49;
  pres->w[1] = sgn | tmp | coeff.w[1];

  return pres;
}


//
//   No overflow/underflow checks
//   No checking for coefficient == 10^34 (rounding artifact)
//
__BID_INLINE__ BID_UINT128 *
bid_get_BID128_very_fast (BID_UINT128 * pres, BID_UINT64 sgn, int expon,
		      BID_UINT128 coeff) {
  BID_UINT64 tmp;

  pres->w[0] = coeff.w[0];
  tmp = expon;
  tmp <<= 49;
  pres->w[1] = sgn | tmp | coeff.w[1];

  return pres;
}

// same as above, but for either endian mode
__BID_INLINE__ BID_UINT128 *
bid_get_BID128_very_fast_BLE (BID_UINT128 * pres, BID_UINT64 sgn, int expon,
		      BID_UINT128 coeff) {
  BID_UINT64 tmp;

  pres->w[BID_LOW_128W] = coeff.w[BID_LOW_128W];
  tmp = expon;
  tmp <<= 49;
  pres->w[BID_HIGH_128W] = sgn | tmp | coeff.w[BID_HIGH_128W];

  return pres;
}

//
//   No overflow/underflow checks
//
__BID_INLINE__ BID_UINT128 *
bid_get_BID128_fast (BID_UINT128 * pres, BID_UINT64 sgn, int expon, BID_UINT128 coeff) {
  BID_UINT64 tmp;

  // coeff==10^34?
  if (coeff.w[1] == 0x0001ed09bead87c0ull
      && coeff.w[0] == 0x378d8e6400000000ull) {
    expon++;
    // set coefficient to 10^33
    coeff.w[1] = 0x0000314dc6448d93ull;
    coeff.w[0] = 0x38c15b0a00000000ull;
  }

  pres->w[0] = coeff.w[0];
  tmp = expon;
  tmp <<= 49;
  pres->w[1] = sgn | tmp | coeff.w[1];

  return pres;
}

//
//   General BID128 pack macro
//
__BID_INLINE__ BID_UINT128 *
bid_get_BID128 (BID_UINT128 * pres, BID_UINT64 sgn, int expon, BID_UINT128 coeff,
	    unsigned *prounding_mode, unsigned *fpsc) {
  BID_UINT128 T;
  BID_UINT64 tmp, tmp2;

  // coeff==10^34?
  if (coeff.w[1] == 0x0001ed09bead87c0ull
      && coeff.w[0] == 0x378d8e6400000000ull) {
    expon++;
    // set coefficient to 10^33
    coeff.w[1] = 0x0000314dc6448d93ull;
    coeff.w[0] = 0x38c15b0a00000000ull;
  }
  // check OF, UF
  if (expon < 0 || expon > DECIMAL_MAX_EXPON_128) {
    // check UF
    if (expon < 0) {
      return handle_UF_128 (pres, sgn, expon, coeff, prounding_mode,
			    fpsc);
    }

    if (expon - MAX_FORMAT_DIGITS_128 <= DECIMAL_MAX_EXPON_128) {
      T = bid_power10_table_128[MAX_FORMAT_DIGITS_128 - 1];
      while (__unsigned_compare_gt_128 (T, coeff)
	     && expon > DECIMAL_MAX_EXPON_128) {
	coeff.w[1] =
	  (coeff.w[1] << 3) + (coeff.w[1] << 1) + (coeff.w[0] >> 61) +
	  (coeff.w[0] >> 63);
	tmp2 = coeff.w[0] << 3;
	coeff.w[0] = (coeff.w[0] << 1) + tmp2;
	if (coeff.w[0] < tmp2)
	  coeff.w[1]++;

	expon--;
      }
    }
    if (expon > DECIMAL_MAX_EXPON_128) {
      if (!(coeff.w[1] | coeff.w[0])) {
	pres->w[1] = sgn | (((BID_UINT64) DECIMAL_MAX_EXPON_128) << 49);
	pres->w[0] = 0;
	return pres;
      }
      // OF
#ifdef BID_SET_STATUS_FLAGS
      __set_status_flags (fpsc, BID_OVERFLOW_EXCEPTION | BID_INEXACT_EXCEPTION);
#endif
#ifndef IEEE_ROUND_NEAREST_TIES_AWAY
#ifndef IEEE_ROUND_NEAREST
      if (*prounding_mode == BID_ROUNDING_TO_ZERO
	  || (sgn && *prounding_mode == BID_ROUNDING_UP) || (!sgn
							 &&
							 *prounding_mode
							 ==
							 BID_ROUNDING_DOWN))
      {
	pres->w[1] = sgn | LARGEST_BID128_HIGH;
	pres->w[0] = LARGEST_BID128_LOW;
      } else
#endif
#endif
      {
	pres->w[1] = sgn | INFINITY_MASK64;
	pres->w[0] = 0;
      }
      return pres;
    }
  }

  pres->w[0] = coeff.w[0];
  tmp = expon;
  tmp <<= 49;
  pres->w[1] = sgn | tmp | coeff.w[1];

  return pres;
}


//
//  Macro used for conversions from string 
//        (no additional arguments given for rounding mode, status flags) 
//
__BID_INLINE__ BID_UINT128 *
bid_get_BID128_string (BID_UINT128 * pres, BID_UINT64 sgn, int expon, BID_UINT128 coeff) {
  BID_UINT128 D2, D8;
  BID_UINT64 tmp;
  unsigned rmode = 0, status;

  // coeff==10^34?
  if (coeff.w[1] == 0x0001ed09bead87c0ull
      && coeff.w[0] == 0x378d8e6400000000ull) {
    expon++;
    // set coefficient to 10^33
    coeff.w[1] = 0x0000314dc6448d93ull;
    coeff.w[0] = 0x38c15b0a00000000ull;
  }
  // check OF, UF
  if ((unsigned) expon > DECIMAL_MAX_EXPON_128) {
    // check UF
    if (expon < 0)
      return handle_UF_128 (pres, sgn, expon, coeff, &rmode, &status);

    // OF

    if (expon < DECIMAL_MAX_EXPON_128 + 34) {
      while (expon > DECIMAL_MAX_EXPON_128 &&
	     (coeff.w[1] < bid_power10_table_128[33].w[1] ||
	      (coeff.w[1] == bid_power10_table_128[33].w[1]
	       && coeff.w[0] < bid_power10_table_128[33].w[0]))) {
	D2.w[1] = (coeff.w[1] << 1) | (coeff.w[0] >> 63);
	D2.w[0] = coeff.w[0] << 1;
	D8.w[1] = (coeff.w[1] << 3) | (coeff.w[0] >> 61);
	D8.w[0] = coeff.w[0] << 3;

	__add_128_128 (coeff, D2, D8);
	expon--;
      }
    } else if (!(coeff.w[0] | coeff.w[1]))
      expon = DECIMAL_MAX_EXPON_128;

    if (expon > DECIMAL_MAX_EXPON_128) {
      pres->w[1] = sgn | INFINITY_MASK64;
      pres->w[0] = 0;
      switch (rmode) {
      case BID_ROUNDING_DOWN:
	if (!sgn) {
	  pres->w[1] = LARGEST_BID128_HIGH;
	  pres->w[0] = LARGEST_BID128_LOW;
	}
	break;
      case BID_ROUNDING_TO_ZERO:
	pres->w[1] = sgn | LARGEST_BID128_HIGH;
	pres->w[0] = LARGEST_BID128_LOW;
	break;
      case BID_ROUNDING_UP:
	// round up
	if (sgn) {
	  pres->w[1] = sgn | LARGEST_BID128_HIGH;
	  pres->w[0] = LARGEST_BID128_LOW;
	}
	break;
      }

      return pres;
    }
  }

  pres->w[0] = coeff.w[0];
  tmp = expon;
  tmp <<= 49;
  pres->w[1] = sgn | tmp | coeff.w[1];

  return pres;
}



/*****************************************************************************
*
*    BID32 pack/unpack macros
*
*****************************************************************************/


__BID_INLINE__ BID_UINT32
unpack_BID32 (BID_UINT32 * psign_x, int *pexponent_x,
	      BID_UINT32 * pcoefficient_x, BID_UINT32 x) {
  BID_UINT32 tmp;

  *psign_x = x & 0x80000000;

  if ((x & SPECIAL_ENCODING_MASK32) == SPECIAL_ENCODING_MASK32) {
    // special encodings
    if ((x & INFINITY_MASK32) == INFINITY_MASK32) {
      *pcoefficient_x = x & 0xfe0fffff;
      if ((x & 0x000fffff) >= 1000000)
	*pcoefficient_x = x & 0xfe000000;
      if ((x & NAN_MASK32) == INFINITY_MASK32)
	*pcoefficient_x = x & 0xf8000000;
      *pexponent_x = 0;
      return 0;	// NaN or Infinity
    }
    // coefficient
    *pcoefficient_x = (x & SMALL_COEFF_MASK32) | LARGE_COEFF_HIGH_BIT32;
    // check for non-canonical value
    if (*pcoefficient_x >= 10000000)
      *pcoefficient_x = 0;
    // get exponent
    tmp = x >> 21;
    *pexponent_x = tmp & EXPONENT_MASK32;
    return *pcoefficient_x;
  }
  // exponent
  tmp = x >> 23;
  *pexponent_x = tmp & EXPONENT_MASK32;
  // coefficient
  *pcoefficient_x = (x & LARGE_COEFF_MASK32);

  return *pcoefficient_x;
}

//
//   General pack macro for BID32 
//
__BID_INLINE__ BID_UINT32
get_BID32 (BID_UINT32 sgn, int expon, BID_UINT64 coeff, int rmode,
	   unsigned *fpsc) {
  BID_UINT128 Q;
  BID_UINT64 _C64, remainder_h, carry, Stemp;
  BID_UINT32 r, mask;
  int extra_digits, amount, amount2;
  unsigned status;

  if (coeff > 9999999ull) {
    expon++;
    coeff = 1000000ull;
  }
  // check for possible underflow/overflow
  if (((unsigned) expon) > DECIMAL_MAX_EXPON_32) {
    if (expon < 0) {
      // underflow
      if (expon + MAX_FORMAT_DIGITS_32 < 0) {
#ifdef BID_SET_STATUS_FLAGS
	__set_status_flags (fpsc,
			    BID_UNDERFLOW_EXCEPTION | BID_INEXACT_EXCEPTION);
#endif
#ifndef IEEE_ROUND_NEAREST_TIES_AWAY
#ifndef IEEE_ROUND_NEAREST
	if (rmode == BID_ROUNDING_DOWN && sgn)
	  return 0x80000001;
	if (rmode == BID_ROUNDING_UP && !sgn)
	  return 1;
#endif
#endif
	// result is 0
	return sgn;
      }
      // get digits to be shifted out
#ifdef IEEE_ROUND_NEAREST_TIES_AWAY
      rmode = 0;
#endif
#ifdef IEEE_ROUND_NEAREST
      rmode = 0;
#endif
#ifndef IEEE_ROUND_NEAREST_TIES_AWAY
#ifndef IEEE_ROUND_NEAREST
      if (sgn && (unsigned) (rmode - 1) < 2)
	rmode = 3 - rmode;
#endif
#endif

      extra_digits = -expon;
      coeff += bid_round_const_table[rmode][extra_digits];

      // get coeff*(2^M[extra_digits])/10^extra_digits
      __mul_64x64_to_128 (Q, coeff, bid_reciprocals10_64[extra_digits]);

      // now get P/10^extra_digits: shift Q_high right by M[extra_digits]-128
      amount = bid_short_recip_scale[extra_digits];

      _C64 = Q.w[1] >> amount;

#ifndef IEEE_ROUND_NEAREST_TIES_AWAY
#ifndef IEEE_ROUND_NEAREST
      if (rmode == 0)	//BID_ROUNDING_TO_NEAREST
#endif
	if (_C64 & 1) {
	  // check whether fractional part of initial_P/10^extra_digits is exactly .5

	  // get remainder
	  amount2 = 64 - amount;
	  remainder_h = 0;
	  remainder_h--;
	  remainder_h >>= amount2;
	  remainder_h = remainder_h & Q.w[1];

	  if (!remainder_h && (Q.w[0] < bid_reciprocals10_64[extra_digits])) {
	    _C64--;
	  }
	}
#endif

#ifdef BID_SET_STATUS_FLAGS

      if (is_inexact (fpsc))
	__set_status_flags (fpsc, BID_UNDERFLOW_EXCEPTION);
      else {
	status = BID_INEXACT_EXCEPTION;
	// get remainder
	remainder_h = Q.w[1] << (64 - amount);

	switch (rmode) {
	case BID_ROUNDING_TO_NEAREST:
	case BID_ROUNDING_TIES_AWAY:
	  // test whether fractional part is 0
	  if (remainder_h == 0x8000000000000000ull
	      && (Q.w[0] < bid_reciprocals10_64[extra_digits]))
	    status = BID_EXACT_STATUS;
	  break;
	case BID_ROUNDING_DOWN:
	case BID_ROUNDING_TO_ZERO:
	  if (!remainder_h && (Q.w[0] < bid_reciprocals10_64[extra_digits]))
	    status = BID_EXACT_STATUS;
	  break;
	default:
	  // round up
	  __add_carry_out (Stemp, carry, Q.w[0],
			   bid_reciprocals10_64[extra_digits]);
	  if ((remainder_h >> (64 - amount)) + carry >=
	      (((BID_UINT64) 1) << amount))
	    status = BID_EXACT_STATUS;
	}

	if (status != BID_EXACT_STATUS)
	  __set_status_flags (fpsc, BID_UNDERFLOW_EXCEPTION | status);
      }

#endif

      return sgn | (BID_UINT32) _C64;
    }

    if(!coeff)  { if(expon > DECIMAL_MAX_EXPON_32) expon = DECIMAL_MAX_EXPON_32; }
    while (coeff < 1000000 && expon > DECIMAL_MAX_EXPON_32) {
      coeff = (coeff << 3) + (coeff << 1);
      expon--;
    }
    if (((unsigned) expon) > DECIMAL_MAX_EXPON_32) {
      __set_status_flags (fpsc, BID_OVERFLOW_EXCEPTION | BID_INEXACT_EXCEPTION);
      // overflow
      r = sgn | INFINITY_MASK32;
      switch (rmode) {
      case BID_ROUNDING_DOWN:
	if (!sgn)
	  r = LARGEST_BID32;
	break;
      case BID_ROUNDING_TO_ZERO:
	r = sgn | LARGEST_BID32;
	break;
      case BID_ROUNDING_UP:
	// round up
	if (sgn)
	  r = sgn | LARGEST_BID32;
      }
      return r;
    }
  }

  mask = 1 << 23;

  // check whether coefficient fits in DECIMAL_COEFF_FIT bits
  if (coeff < mask) {
    r = expon;
    r <<= 23;
    r |= ((BID_UINT32) coeff | sgn);
    return r;
  }
  // special format

  r = expon;
  r <<= 21;
  r |= (sgn | SPECIAL_ENCODING_MASK32);
  // add coeff, without leading bits
  mask = (1 << 21) - 1;
  r |= (((BID_UINT32) coeff) & mask);

  return r;
}





//
//   General pack macro for BID32 
//
__BID_INLINE__ BID_UINT32
get_BID32_UF (BID_UINT32 sgn, int expon, BID_UINT64 coeff, BID_UINT32 R, int rmode,
	   unsigned *fpsc) {
  BID_UINT128 Q;
  BID_UINT64 _C64, remainder_h, carry, Stemp;
  BID_UINT32 r, mask;
  int extra_digits, amount, amount2;
  unsigned status;

  if (coeff > 9999999ull) {
    expon++;
    coeff = 1000000ull;
  }
  // check for possible underflow/overflow
  if (((unsigned) expon) > DECIMAL_MAX_EXPON_32) {
   if (expon < 0) {
      // underflow
      if (expon + MAX_FORMAT_DIGITS_32 < 0) {
#ifdef BID_SET_STATUS_FLAGS
	__set_status_flags (fpsc,
			    BID_UNDERFLOW_EXCEPTION | BID_INEXACT_EXCEPTION);
#endif
#ifndef IEEE_ROUND_NEAREST_TIES_AWAY
#ifndef IEEE_ROUND_NEAREST
	if (rmode == BID_ROUNDING_DOWN && sgn)
	  return 0x80000001;
	if (rmode == BID_ROUNDING_UP && !sgn)
	  return 1;
#endif
#endif
	// result is 0
	return sgn;
      }
      // get digits to be shifted out
#ifdef IEEE_ROUND_NEAREST_TIES_AWAY
      rmode = 0;
#endif
#ifdef IEEE_ROUND_NEAREST
      rmode = 0;
#endif
#ifndef IEEE_ROUND_NEAREST_TIES_AWAY
#ifndef IEEE_ROUND_NEAREST
      if (sgn && (unsigned) (rmode - 1) < 2)
	rmode = 3 - rmode;
#endif
#endif

  // 10*coeff
  coeff = (coeff << 3) + (coeff << 1);
  if (R)
    coeff |= 1;

      extra_digits = 1-expon;
      coeff += bid_round_const_table[rmode][extra_digits];

      // get coeff*(2^M[extra_digits])/10^extra_digits
      __mul_64x64_to_128 (Q, coeff, bid_reciprocals10_64[extra_digits]);

      // now get P/10^extra_digits: shift Q_high right by M[extra_digits]-128
      amount = bid_short_recip_scale[extra_digits];

      _C64 = Q.w[1] >> amount;

#ifndef IEEE_ROUND_NEAREST_TIES_AWAY
#ifndef IEEE_ROUND_NEAREST
      if (rmode == 0)	//BID_ROUNDING_TO_NEAREST
#endif
	if (_C64 & 1) {
	  // check whether fractional part of initial_P/10^extra_digits is exactly .5

	  // get remainder
	  amount2 = 64 - amount;
	  remainder_h = 0;
	  remainder_h--;
	  remainder_h >>= amount2;
	  remainder_h = remainder_h & Q.w[1];

	  if (!remainder_h && (Q.w[0] < bid_reciprocals10_64[extra_digits])) {
	    _C64--;
	  }
	}
#endif

#ifdef BID_SET_STATUS_FLAGS
	  if (is_inexact (fpsc)){
			  __set_status_flags (fpsc, BID_UNDERFLOW_EXCEPTION);}
      else {
	status = BID_INEXACT_EXCEPTION;
	// get remainder
	remainder_h = Q.w[1] << (64 - amount);

	switch (rmode) {
	case BID_ROUNDING_TO_NEAREST:
	case BID_ROUNDING_TIES_AWAY:
	  // test whether fractional part is 0
	  if (remainder_h == 0x8000000000000000ull
	      && (Q.w[0] < bid_reciprocals10_64[extra_digits]))
	    status = BID_EXACT_STATUS;
	  break;
	case BID_ROUNDING_DOWN:
	case BID_ROUNDING_TO_ZERO:
	  if (!remainder_h && (Q.w[0] < bid_reciprocals10_64[extra_digits]))
	    status = BID_EXACT_STATUS;
	  break;
	default:
	  // round up
	  __add_carry_out (Stemp, carry, Q.w[0],
			   bid_reciprocals10_64[extra_digits]);
	  if ((remainder_h >> (64 - amount)) + carry >=
	      (((BID_UINT64) 1) << amount))
	    status = BID_EXACT_STATUS;
	}

	if (status != BID_EXACT_STATUS) {
	  __set_status_flags (fpsc,  BID_UNDERFLOW_EXCEPTION|status);
	}
 }
#endif

      return sgn | (BID_UINT32) _C64;
    }

    while (coeff < 1000000 && expon > DECIMAL_MAX_EXPON_32) {
      coeff = (coeff << 3) + (coeff << 1);
      expon--;
    }
    if (((unsigned) expon) > DECIMAL_MAX_EXPON_32) {
      __set_status_flags (fpsc, BID_OVERFLOW_EXCEPTION | BID_INEXACT_EXCEPTION);
      // overflow
      r = sgn | INFINITY_MASK32;
      switch (rmode) {
      case BID_ROUNDING_DOWN:
	if (!sgn)
	  r = LARGEST_BID32;
	break;
      case BID_ROUNDING_TO_ZERO:
	r = sgn | LARGEST_BID32;
	break;
      case BID_ROUNDING_UP:
	// round up
	if (sgn)
	  r = sgn | LARGEST_BID32;
      }
      return r;
    }
  }

  mask = 1 << 23;

  // check whether coefficient fits in DECIMAL_COEFF_FIT bits
  if (coeff < mask) {
    r = expon;
    r <<= 23;
    r |= ((BID_UINT32) coeff | sgn);
    return r;
  }
  // special format

  r = expon;
  r <<= 21;
  r |= (sgn | SPECIAL_ENCODING_MASK32);
  // add coeff, without leading bits
  mask = (1 << 21) - 1;
  r |= (((BID_UINT32) coeff) & mask);

  return r;
}






//
//   no overflow/underflow checks
//
__BID_INLINE__ BID_UINT32
very_fast_get_BID32 (BID_UINT32 sgn, int expon, BID_UINT32 coeff) {
  BID_UINT32 r, mask;

  mask = 1 << 23;

  // check whether coefficient fits in 10*2+3 bits
  if (coeff < mask) {
    r = expon;
    r <<= 23;
    r |= (coeff | sgn);
    return r;
  }
  // special format
  r = expon;
  r <<= 21;
  r |= (sgn | SPECIAL_ENCODING_MASK32);
  // add coeff, without leading bits
  mask = (1 << 21) - 1;
  coeff &= mask;
  r |= coeff;

  return r;
}

__BID_INLINE__ BID_UINT32
fast_get_BID32 (BID_UINT32 sgn, int expon, BID_UINT32 coeff) {
  BID_UINT32 r, mask;

  mask = 1 << 23;

  if (coeff > 9999999ul) {
    expon++;
    coeff = 1000000ul;
  }
  // check whether coefficient fits in 10*2+3 bits
  if (coeff < mask) {
    r = expon;
    r <<= 23;
    r |= (coeff | sgn);
    return r;
  }
  // special format
  r = expon;
  r <<= 21;
  r |= (sgn | SPECIAL_ENCODING_MASK32);
  // add coeff, without leading bits
  mask = (1 << 21) - 1;
  coeff &= mask;
  r |= coeff;

  return r;
}



/*************************************************************
 *
 *************************************************************/
typedef struct BID_ALIGN (16)
     {
       BID_UINT64 w[6];
     } BID_UINT384;
     typedef struct BID_ALIGN (16)
     {
       BID_UINT64 w[8];
     } BID_UINT512;

// #define P                               34
#define MASK_STEERING_BITS              0x6000000000000000ull
#define MASK_BINARY_EXPONENT1           0x7fe0000000000000ull
#define MASK_BINARY_SIG1                0x001fffffffffffffull
#define MASK_BINARY_EXPONENT2           0x1ff8000000000000ull
    //used to take G[2:w+3] (sec 3.3)
#define MASK_BINARY_SIG2                0x0007ffffffffffffull
    //used to mask out G4:T0 (sec 3.3)
#define MASK_BINARY_OR2                 0x0020000000000000ull
    //used to prefix 8+G4 to T (sec 3.3)
#define UPPER_EXPON_LIMIT               51
#define MASK_EXP                        0x7ffe000000000000ull
#define MASK_EXP2                       0x1fff800000000000ull
#define MASK_SPECIAL                    0x7800000000000000ull
#define MASK_NAN                        0x7c00000000000000ull
#define MASK_SNAN                       0x7e00000000000000ull
#define MASK_ANY_INF                    0x7c00000000000000ull
#define MASK_INF                        0x7800000000000000ull
#define MASK_SIGN                       0x8000000000000000ull
#define MASK_COEFF                      0x0001ffffffffffffull
#define BIN_EXP_BIAS                    (0x1820ull << 49)

#define EXP_MIN32                       0x00000000
#define EXP_MAX32                       0x5f800000
#define EXP_MIN                         0x0000000000000000ull
   // EXP_MIN = (-6176 + 6176) << 49
#define EXP_MAX                         0x5ffe000000000000ull
  // EXP_MAX = (6111 + 6176) << 49
#define EXP_MAX_P1                      0x6000000000000000ull
  // EXP_MAX + 1 = (6111 + 6176 + 1) << 49
#define EXP_P1                          0x0002000000000000ull
  // EXP_ P1= 1 << 49
#define expmin                            -6176
  // min unbiased exponent
#define expmax                            6111
  // max unbiased exponent
#define expmin16                          -398
  // min unbiased exponent
#define expmax16                          369
  // max unbiased exponent
#define expmin7                           -101
  // min unbiased exponent
#define expmax7                           90
  // max unbiased exponent

#define MASK_INF32                        0x78000000
#define MASK_ANY_INF32                    0x7c000000
#define MASK_SIGN32                       0x80000000
#define MASK_NAN32                        0x7c000000
#define MASK_SNAN32                       0x7e000000
#define SIGNMASK32                        0x80000000
#define BID32_SIG_MAX                     0x0098967f
#define BID64_SIG_MAX                     0x002386F26FC0ffffull
#define SIGNMASK64                        0x8000000000000000ull
#define MASK_STEERING_BITS32              0x60000000
#define MASK_BINARY_EXPONENT1_32          0x7f800000
#define MASK_BINARY_SIG1_32               0x007fffff
#define MASK_BINARY_EXPONENT2_32          0x1fe00000
    //used to take G[2:w+3] (sec 3.3)
#define MASK_BINARY_SIG2_32               0x001fffff
    //used to mask out G4:T0 (sec 3.3)
#define MASK_BINARY_OR2_32                0x00800000
#define MASK_SPECIAL32                    0x78000000

// typedef unsigned int BID_FPSC; // floating-point status and control
	// bit31:
	// bit30:
	// bit29:
	// bit28:
	// bit27:
	// bit26:
	// bit25:
	// bit24:
	// bit23:
	// bit22:
	// bit21:
	// bit20:
	// bit19:
	// bit18:
	// bit17:
	// bit16:
	// bit15:
	// bit14: RC:2
	// bit13: RC:1
	// bit12: RC:0
	// bit11: PM
	// bit10: UM
	// bit9:  OM
	// bit8:  ZM
	// bit7:  DM
	// bit6:  IM
	// bit5:  PE
	// bit4:  UE
	// bit3:  OE
	// bit2:  ZE
	// bit1:  DE
	// bit0:  IE

#define ROUNDING_BID_MODE_MASK	0x00007000

     typedef struct _DEC_DIGITS {
       unsigned int digits;
       BID_UINT64 threshold_hi;
       BID_UINT64 threshold_lo;
       unsigned int digits1;
     } DEC_DIGITS;

     BID_EXTERN_C DEC_DIGITS bid_nr_digits[];
     BID_EXTERN_C BID_UINT64 bid_midpoint64[];
     BID_EXTERN_C BID_UINT128 bid_midpoint128[];
     BID_EXTERN_C BID_UINT192 bid_midpoint192[];
     BID_EXTERN_C BID_UINT256 bid_midpoint256[];
     BID_EXTERN_C BID_UINT64 bid_ten2k64[];
     BID_EXTERN_C BID_UINT128 bid_ten2k128[];
     BID_EXTERN_C BID_UINT256 bid_ten2k256[];
     BID_EXTERN_C BID_UINT128 bid_ten2mk128[];
     BID_EXTERN_C BID_UINT64 bid_ten2mk64[];
     BID_EXTERN_C BID_UINT128 bid_ten2mk128trunc[];
     BID_EXTERN_C int bid_shiftright128[];
     BID_EXTERN_C BID_UINT64 bid_maskhigh128[];
     BID_EXTERN_C BID_UINT64 bid_maskhigh128M[];
     BID_EXTERN_C BID_UINT64 bid_maskhigh192M[];
     BID_EXTERN_C BID_UINT64 bid_maskhigh256M[];
     BID_EXTERN_C BID_UINT64 bid_onehalf128[];
     BID_EXTERN_C BID_UINT64 bid_onehalf128M[];
     BID_EXTERN_C BID_UINT64 bid_onehalf192M[];
     BID_EXTERN_C BID_UINT64 bid_onehalf256M[];
     BID_EXTERN_C BID_UINT128 bid_ten2mk128M[];
     BID_EXTERN_C BID_UINT128 bid_ten2mk128truncM[];
     BID_EXTERN_C BID_UINT192 bid_ten2mk192truncM[];
     BID_EXTERN_C BID_UINT256 bid_ten2mk256truncM[];
     BID_EXTERN_C int bid_shiftright128M[];
     BID_EXTERN_C int bid_shiftright192M[];
     BID_EXTERN_C int bid_shiftright256M[];
     BID_EXTERN_C BID_UINT192 bid_ten2mk192M[];
     BID_EXTERN_C BID_UINT256 bid_ten2mk256M[];
     BID_EXTERN_C unsigned char bid_char_table2[];
     BID_EXTERN_C unsigned char bid_char_table3[];

     BID_EXTERN_C BID_UINT64 bid_ten2m3k64[];
     BID_EXTERN_C unsigned int bid_shift_ten2m3k64[];
     BID_EXTERN_C BID_UINT128 bid_ten2m3k128[];
     BID_EXTERN_C unsigned int bid_shift_ten2m3k128[];



/***************************************************************************
 *************** TABLES FOR GENERAL ROUNDING FUNCTIONS *********************
 ***************************************************************************/

     BID_EXTERN_C BID_UINT64 bid_Kx64[];
     BID_EXTERN_C unsigned int bid_Ex64m64[];
     BID_EXTERN_C BID_UINT64 bid_half64[];
     BID_EXTERN_C BID_UINT64 bid_mask64[];
     BID_EXTERN_C BID_UINT64 bid_ten2mxtrunc64[];

     BID_EXTERN_C BID_UINT128 bid_Kx128[];
     BID_EXTERN_C unsigned int bid_Ex128m128[];
     BID_EXTERN_C BID_UINT64 bid_half128[];
     BID_EXTERN_C BID_UINT64 bid_mask128[];
     BID_EXTERN_C BID_UINT128 bid_ten2mxtrunc128[];

     BID_EXTERN_C BID_UINT192 bid_Kx192[];
     BID_EXTERN_C unsigned int bid_Ex192m192[];
     BID_EXTERN_C BID_UINT64 bid_half192[];
     BID_EXTERN_C BID_UINT64 bid_mask192[];
     BID_EXTERN_C BID_UINT192 bid_ten2mxtrunc192[];

     BID_EXTERN_C BID_UINT256 bid_Kx256[];
     BID_EXTERN_C unsigned int bid_Ex256m256[];
     BID_EXTERN_C BID_UINT64 bid_half256[];
     BID_EXTERN_C BID_UINT64 bid_mask256[];
     BID_EXTERN_C BID_UINT256 bid_ten2mxtrunc256[];

     typedef union BID_ALIGN (16) __bid64_128 {
       BID_UINT64 b64;
       BID_UINT128 b128;
     } BID64_128;

     BID64_128 bid_fma (unsigned int P0,
			BID64_128 x1, unsigned int P1,
			BID64_128 y1, unsigned int P2,
			BID64_128 z1, unsigned int P3,
			unsigned int rnd_mode, BID_FPSC * fpsc);

#define         P7      7
#define         P16     16
#define         P34     34

     union __int_double {
       BID_UINT64 i;
       double d;
     };
     typedef union __int_double int_double;


     union __int_float {
       BID_UINT32 i;
       float d;
     };
     typedef union __int_float int_float;

#define SWAP(A,B,T) {\
        T = A; \
        A = B; \
        B = T; \
}

// this macro will find coefficient_x to be in [2^A, 2^(A+1) )
// ie it knows that it is A bits long
#define NUMBITS(A, coefficient_x, tempx){\
      temp_x.d=(float)coefficient_x;\
      A=((tempx.i >>23) & EXPONENT_MASK32) - 0x7f;\
}

enum class_types {
  signalingNaN,
  quietNaN,
  negativeInfinity,
  negativeNormal,
  negativeSubnormal,
  negativeZero,
  positiveZero,
  positiveSubnormal,
  positiveNormal,
  positiveInfinity
};

typedef union { 
  BID_UINT32 ui32; 
  float f; 
} BID_UI32FLOAT; 

typedef union {
  BID_UINT64 ui64;
  double d;
} BID_UI64DOUBLE;


//////////////////////////////////////////////////
// Macros for raising exceptions / setting flags
/////////////////////////////////////////////////

#define bid_raise_except(x)  __set_status_flags (pfpsf, (x))
#define get_bid_sw()         (*pfpsf)
#define set_bid_sw(x)        (*pfpsf) = (x)

#endif
