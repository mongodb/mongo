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

#ifndef _DIV_MACROS_H_
#define _DIV_MACROS_H_

#include "bid_internal.h"

//#define DOUBLE_EXTENDED_ON

#if DOUBLE_EXTENDED_ON


__BID_INLINE__ void
bid___div_128_by_128 (BID_UINT128 * pCQ, BID_UINT128 * pCR, BID_UINT128 CX, BID_UINT128 CY) {
  BID_UINT128 CB2, CB4, CB8, CQB, CA;
  int_double d64, dm64, ds;
  int_float t64;
  double dx, dq, dqh;
  BINARY80 lq, lx, ly;
  BID_UINT64 Rh, Ph, Ql, Ql2, carry, Qh;

  if (!CY.w[1]) {
    pCR->w[1] = 0;

    if (!CX.w[1]) {
      pCQ->w[0] = CX.w[0] / CY.w[0];
      pCQ->w[1] = 0;
      pCR->w[1] = 0;
      pCR->w[0] = CX.w[0] - pCQ->w[0] * CY.w[0];
      return;
    } else if(CY.w[0]<0xd000000000000000ull) {

      // This path works for CX<2^116 only 

      // 2^64
      d64.i = 0x43f0000000000000ull;
      // 2^64
      dm64.i = 0x3bf0000000000000ull;
      // 1.5*2^(-52)
      ds.i = 0x3cb8000000000000ull;
      dx = (BINARY80) CX.w[1] * d64.d + (BINARY80) CX.w[0];
      dq = dx / (BINARY80) CY.w[0];
      dq -= dq * (ds.d);
      dqh = dq * dm64.d;
      Qh = (BID_UINT64) dqh;
      Ql = (BID_UINT64) (dq - ((double) Qh) * d64.d);
//printf("Qh=%016I64x, Ql=%016I64x\n",Qh, Ql);

      Rh = CX.w[0] - Ql * CY.w[0];
      Ql2 = Rh / CY.w[0];
      pCR->w[0] = Rh - Ql2 * CY.w[0];
      __add_carry_out ((pCQ->w[0]), carry, Ql, Ql2);
      pCQ->w[1] = Qh + carry;
    
	  return;
    }
  }
  // now CY.w[1] > 0 or CY.[0]>=13*2^(60)

  // 2^64
  t64.i = 0x5f800000;
  lx = (BINARY80) CX.w[1] * (BINARY80) t64.d + (BINARY80) CX.w[0];
  ly = (BINARY80) CY.w[1] * (BINARY80) t64.d + (BINARY80) CY.w[0];
  lq = lx / ly;
  pCQ->w[0] = (BID_UINT64) lq;

  pCQ->w[1] = 0;

  if (!pCQ->w[0]) {
    /*if(__unsigned_compare_ge_128(CX,CY))
       {
       pCQ->w[0] = 1;
       __sub_128_128((*pCR), CX, CY);
       }
       else */
    {
      pCR->w[1] = CX.w[1];
      pCR->w[0] = CX.w[0];
    }
    return;
  }

  if (CY.w[1] >= 16 || pCQ->w[0] <= 0x1000000000000000ull) {
    pCQ->w[0] = (BID_UINT64) lq - 1;
    __mul_64x128_full (Ph, CQB, (pCQ->w[0]), CY);
    __sub_128_128 (CA, CX, CQB);
    if (__unsigned_compare_ge_128 (CA, CY)) {
      __sub_128_128 (CA, CA, CY);
      pCQ->w[0]++;
      if (__unsigned_compare_ge_128 (CA, CY)) {
	__sub_128_128 (CA, CA, CY);
	pCQ->w[0]++;
      }
    }
    pCR->w[1] = CA.w[1];
    pCR->w[0] = CA.w[0];
  } else {
    pCQ->w[0] = (BID_UINT64) lq - 6;

    __mul_64x128_full (Ph, CQB, (pCQ->w[0]), CY);
    __sub_128_128 (CA, CX, CQB);

    CB8.w[1] = (CY.w[1] << 3) | (CY.w[0] >> 61);
    CB8.w[0] = CY.w[0] << 3;
    CB4.w[1] = (CY.w[1] << 2) | (CY.w[0] >> 62);
    CB4.w[0] = CY.w[0] << 2;
    CB2.w[1] = (CY.w[1] << 1) | (CY.w[0] >> 63);
    CB2.w[0] = CY.w[0] << 1;

    if (__unsigned_compare_ge_128 (CA, CB8)) {
      pCQ->w[0] += 8;
      __sub_128_128 (CA, CA, CB8);
    }
    if (__unsigned_compare_ge_128 (CA, CB4)) {
      pCQ->w[0] += 4;
      __sub_128_128 (CA, CA, CB4);
    }
    if (__unsigned_compare_ge_128 (CA, CB2)) {
      pCQ->w[0] += 2;
      __sub_128_128 (CA, CA, CB2);
    }
    if (__unsigned_compare_ge_128 (CA, CY)) {
      pCQ->w[0] += 1;
      __sub_128_128 (CA, CA, CY);
    }

    pCR->w[1] = CA.w[1];
    pCR->w[0] = CA.w[0];
  }
}






__BID_INLINE__ void
bid___div_256_by_128 (BID_UINT128 * pCQ, BID_UINT256 * pCA4, BID_UINT128 CY) {
  BID_UINT256 CQ2Y;
  BID_UINT128 CQ2, CQ3Y;
  BID_UINT64 Q3, carry64;
  int_double d64;
  BINARY80 lx, ly, lq, l64, l128;

  // 2^64
  d64.i = 0x43f0000000000000ull;
  l64 = (BINARY80) d64.d;
  // 2^128
  l128 = l64 * l64;

  lx =
    ((BINARY80) (*pCA4).w[3] * l64 +
     (BINARY80) (*pCA4).w[2]) * l128 +
    (BINARY80) (*pCA4).w[1] * l64 + (BINARY80) (*pCA4).w[0];
  ly = (BINARY80) CY.w[1] * l128 + (BINARY80) CY.w[0] * l64;

  lq = lx / ly;
  CQ2.w[1] = (BID_UINT64) lq;
  lq = (lq - CQ2.w[1]) * l64;
  CQ2.w[0] = (BID_UINT64) lq;

  // CQ2*CY
  __mul_128x128_to_256 (CQ2Y, CY, CQ2);

  // CQ2Y <= (*pCA4) ?
  if (CQ2Y.w[3] < (*pCA4).w[3]
      || (CQ2Y.w[3] == (*pCA4).w[3]
	  && (CQ2Y.w[2] < (*pCA4).w[2]
	      || (CQ2Y.w[2] == (*pCA4).w[2]
		  && (CQ2Y.w[1] < (*pCA4).w[1]
		      || (CQ2Y.w[1] == (*pCA4).w[1]
			  && (CQ2Y.w[0] <= (*pCA4).w[0]))))))) {

    // (*pCA4) -CQ2Y, guaranteed below 5*2^49*CY < 5*2^(49+128)
    __sub_borrow_out ((*pCA4).w[0], carry64, (*pCA4).w[0], CQ2Y.w[0]);
    __sub_borrow_in_out ((*pCA4).w[1], carry64, (*pCA4).w[1], CQ2Y.w[1],
			 carry64);
    (*pCA4).w[2] = (*pCA4).w[2] - CQ2Y.w[2] - carry64;

    lx = ((BINARY80) (*pCA4).w[2] * l128 +
	  ((BINARY80) (*pCA4).w[1] * l64 +
	   (BINARY80) (*pCA4).w[0])) * l64;
    lq = lx / ly;
    Q3 = (BID_UINT64) lq;

    if (Q3) {
      Q3--;
      __mul_64x128_short (CQ3Y, Q3, CY);
      __sub_borrow_out ((*pCA4).w[0], carry64, (*pCA4).w[0], CQ3Y.w[0]);
      (*pCA4).w[1] = (*pCA4).w[1] - CQ3Y.w[1] - carry64;

      if ((*pCA4).w[1] > CY.w[1]
	  || ((*pCA4).w[1] == CY.w[1] && (*pCA4).w[0] >= CY.w[0])) {
	Q3++;
	__sub_borrow_out ((*pCA4).w[0], carry64, (*pCA4).w[0], CY.w[0]);
	(*pCA4).w[1] = (*pCA4).w[1] - CY.w[1] - carry64;
	if ((*pCA4).w[1] > CY.w[1]
	    || ((*pCA4).w[1] == CY.w[1] && (*pCA4).w[0] >= CY.w[0])) {
	  Q3++;
	  __sub_borrow_out ((*pCA4).w[0], carry64, (*pCA4).w[0],
			    CY.w[0]);
	  (*pCA4).w[1] = (*pCA4).w[1] - CY.w[1] - carry64;
	}
      }
      // add Q3 to Q2
      __add_carry_out (CQ2.w[0], carry64, Q3, CQ2.w[0]);
      CQ2.w[1] += carry64;
    }
  } else {
    // CQ2Y - (*pCA4), guaranteed below 5*2^(49+128)
    __sub_borrow_out ((*pCA4).w[0], carry64, CQ2Y.w[0], (*pCA4).w[0]);
    __sub_borrow_in_out ((*pCA4).w[1], carry64, CQ2Y.w[1], (*pCA4).w[1],
			 carry64);
    (*pCA4).w[2] = CQ2Y.w[2] - (*pCA4).w[2] - carry64;

    lx =
      ((BINARY80) (*pCA4).w[2] * l128 +
       (BINARY80) (*pCA4).w[1] * l64 + (BINARY80) (*pCA4).w[0]) * l64;
    lq = lx / ly;
    Q3 = 1 + (BID_UINT64) lq;

    __mul_64x128_short (CQ3Y, Q3, CY);
    __sub_borrow_out ((*pCA4).w[0], carry64, CQ3Y.w[0], (*pCA4).w[0]);
    (*pCA4).w[1] = CQ3Y.w[1] - (*pCA4).w[1] - carry64;

    if ((BID_SINT64) (*pCA4).w[1] > (BID_SINT64) CY.w[1]
	|| ((*pCA4).w[1] == CY.w[1] && (*pCA4).w[0] >= CY.w[0])) {
      Q3--;
      __sub_borrow_out ((*pCA4).w[0], carry64, (*pCA4).w[0], CY.w[0]);
      (*pCA4).w[1] = (*pCA4).w[1] - CY.w[1] - carry64;
    } else if ((BID_SINT64) (*pCA4).w[1] < 0) {
      Q3++;
      __add_carry_out ((*pCA4).w[0], carry64, (*pCA4).w[0], CY.w[0]);
      (*pCA4).w[1] = (*pCA4).w[1] + CY.w[1] + carry64;
    }
    // subtract Q3 from Q2
    __sub_borrow_out (CQ2.w[0], carry64, CQ2.w[0], Q3);
    CQ2.w[1] -= carry64;
  }

  // (*pCQ) + CQ2 + carry
  __add_carry_out ((*pCQ).w[0], carry64, CQ2.w[0], (*pCQ).w[0]);
  (*pCQ).w[1] = (*pCQ).w[1] + CQ2.w[1] + carry64;


}
#else

__BID_INLINE__ void
bid___div_128_by_128 (BID_UINT128 * pCQ, BID_UINT128 * pCR, BID_UINT128 CX0, BID_UINT128 CY) {
  BID_UINT128 CY36, CY51, CQ, A2, CX, CQT;
  BID_UINT64 Q;
  int_double t64, d49, d60;
  double lx, ly, lq;

  if (!CX0.w[1] && !CY.w[1]) {
    pCQ->w[0] = CX0.w[0] / CY.w[0];
    pCQ->w[1] = 0;
    pCR->w[1] = pCR->w[0] = 0;
    pCR->w[0] = CX0.w[0] - pCQ->w[0] * CY.w[0];
    return;
  }

  CX.w[1] = CX0.w[1];
  CX.w[0] = CX0.w[0];

  // 2^64
  t64.i = 0x43f0000000000000ull;
  lx = (double) CX.w[1] * t64.d + (double) CX.w[0];
  ly = (double) CY.w[1] * t64.d + (double) CY.w[0];
  lq = lx / ly;

  CY36.w[1] = CY.w[0] >> (64 - 36);
  CY36.w[0] = CY.w[0] << 36;

  CQ.w[1] = CQ.w[0] = 0;

  // Q >= 2^100 ?
  if (!CY.w[1] && !CY36.w[1] && (CX.w[1] >= CY36.w[0])) {
    // then Q >= 2^100

    // 2^(-60)*CX/CY
    d60.i = 0x3c30000000000000ull;
    lq *= d60.d;
    Q = (BID_UINT64) lq - 4ull;

    // Q*CY
    __mul_64x64_to_128 (A2, Q, CY.w[0]);

    // A2 <<= 60
    A2.w[1] = (A2.w[1] << 60) | (A2.w[0] >> (64 - 60));
    A2.w[0] <<= 60;

    __sub_128_128 (CX, CX, A2);

    lx = (double) CX.w[1] * t64.d + (double) CX.w[0];
    lq = lx / ly;

    CQ.w[1] = Q >> (64 - 60);
    CQ.w[0] = Q << 60;
  }


  CY51.w[1] = (CY.w[1] << 51) | (CY.w[0] >> (64 - 51));
  CY51.w[0] = CY.w[0] << 51;

  if (CY.w[1] < (BID_UINT64) (1 << (64 - 51))
      && (__unsigned_compare_gt_128 (CX, CY51))) {
    // Q > 2^51 

    // 2^(-49)*CX/CY
    d49.i = 0x3ce0000000000000ull;
    lq *= d49.d;

    Q = (BID_UINT64) lq - 1ull;

    // Q*CY
    __mul_64x64_to_128 (A2, Q, CY.w[0]);
    A2.w[1] += Q * CY.w[1];

    // A2 <<= 49
    A2.w[1] = (A2.w[1] << 49) | (A2.w[0] >> (64 - 49));
    A2.w[0] <<= 49;

    __sub_128_128 (CX, CX, A2);

    CQT.w[1] = Q >> (64 - 49);
    CQT.w[0] = Q << 49;
    __add_128_128 (CQ, CQ, CQT);

    lx = (double) CX.w[1] * t64.d + (double) CX.w[0];
    lq = lx / ly;
  }

  Q = (BID_UINT64) lq;

  __mul_64x64_to_128 (A2, Q, CY.w[0]);
  A2.w[1] += Q * CY.w[1];

  __sub_128_128 (CX, CX, A2);
  if ((BID_SINT64) CX.w[1] < 0) {
    Q--;
    CX.w[0] += CY.w[0];
    if (CX.w[0] < CY.w[0])
      CX.w[1]++;
    CX.w[1] += CY.w[1];
    if ((BID_SINT64) CX.w[1] < 0) {
      Q--;
      CX.w[0] += CY.w[0];
      if (CX.w[0] < CY.w[0])
	CX.w[1]++;
      CX.w[1] += CY.w[1];
    }
  } else if (__unsigned_compare_ge_128 (CX, CY)) {
    Q++;
    __sub_128_128 (CX, CX, CY);
  }

  __add_128_64 (CQ, CQ, Q);


  pCQ->w[1] = CQ.w[1];
  pCQ->w[0] = CQ.w[0];
  pCR->w[1] = CX.w[1];
  pCR->w[0] = CX.w[0];
  return;
}


__BID_INLINE__ void
bid___div_256_by_128 (BID_UINT128 * pCQ, BID_UINT256 * pCA4, BID_UINT128 CY) {
  BID_UINT256 CA4, CA2, CY51, CY36;
  BID_UINT128 CQ, A2, A2h, CQT;
  BID_UINT64 Q, carry64;
  int_double t64, d49, d60;
  double lx, ly, lq, d128, d192;

  // the quotient is assumed to be at most 113 bits, 
  // as needed by BID128 divide routines

  // initial dividend
  CA4.w[3] = (*pCA4).w[3];
  CA4.w[2] = (*pCA4).w[2];
  CA4.w[1] = (*pCA4).w[1];
  CA4.w[0] = (*pCA4).w[0];
  CQ.w[1] = (*pCQ).w[1];
  CQ.w[0] = (*pCQ).w[0];

  // 2^64
  t64.i = 0x43f0000000000000ull;
  d128 = t64.d * t64.d;
  d192 = d128 * t64.d;
  lx = (double) CA4.w[3] * d192 + ((double) CA4.w[2] * d128 +
				   ((double) CA4.w[1] * t64.d +
				    (double) CA4.w[0]));
  ly = (double) CY.w[1] * t64.d + (double) CY.w[0];
  lq = lx / ly;

  CY36.w[2] = CY.w[1] >> (64 - 36);
  CY36.w[1] = (CY.w[1] << 36) | (CY.w[0] >> (64 - 36));
  CY36.w[0] = CY.w[0] << 36;

  //CQ.w[1] = (*pCQ).w[1];
  //CQ.w[0] = (*pCQ).w[0];

  // Q >= 2^100 ?
  if (CA4.w[3] > CY36.w[2]
      || (CA4.w[3] == CY36.w[2]
	  && (CA4.w[2] > CY36.w[1]
	      || (CA4.w[2] == CY36.w[1] && CA4.w[1] >= CY36.w[0])))) {
    // 2^(-60)*CA4/CY
    d60.i = 0x3c30000000000000ull;
    lq *= d60.d;
    Q = (BID_UINT64) lq - 4ull;

    // Q*CY
    __mul_64x128_to_192 (CA2, Q, CY);

    // CA2 <<= 60
    // CA2.w[3] = CA2.w[2] >> (64-60);
    CA2.w[2] = (CA2.w[2] << 60) | (CA2.w[1] >> (64 - 60));
    CA2.w[1] = (CA2.w[1] << 60) | (CA2.w[0] >> (64 - 60));
    CA2.w[0] <<= 60;

    // CA4 -= CA2
    __sub_borrow_out (CA4.w[0], carry64, CA4.w[0], CA2.w[0]);
    __sub_borrow_in_out (CA4.w[1], carry64, CA4.w[1], CA2.w[1],
			 carry64);
    CA4.w[2] = CA4.w[2] - CA2.w[2] - carry64;

    lx = ((double) CA4.w[2] * d128 +
	  ((double) CA4.w[1] * t64.d + (double) CA4.w[0]));
    lq = lx / ly;

    CQT.w[1] = Q >> (64 - 60);
    CQT.w[0] = Q << 60;
    __add_128_128 (CQ, CQ, CQT);
  }

  CY51.w[2] = CY.w[1] >> (64 - 51);
  CY51.w[1] = (CY.w[1] << 51) | (CY.w[0] >> (64 - 51));
  CY51.w[0] = CY.w[0] << 51;

  if (CA4.w[2] > CY51.w[2] || ((CA4.w[2] == CY51.w[2])
			       &&
			       (__unsigned_compare_gt_128 (CA4, CY51))))
  {
    // Q > 2^51 

    // 2^(-49)*CA4/CY
    d49.i = 0x3ce0000000000000ull;
    lq *= d49.d;

    Q = (BID_UINT64) lq - 1ull;

    // Q*CY
    __mul_64x64_to_128 (A2, Q, CY.w[0]);
    __mul_64x64_to_128 (A2h, Q, CY.w[1]);
    A2.w[1] += A2h.w[0];
    if (A2.w[1] < A2h.w[0])
      A2h.w[1]++;

    // A2 <<= 49
    CA2.w[2] = (A2h.w[1] << 49) | (A2.w[1] >> (64 - 49));
    CA2.w[1] = (A2.w[1] << 49) | (A2.w[0] >> (64 - 49));
    CA2.w[0] = A2.w[0] << 49;

    __sub_borrow_out (CA4.w[0], carry64, CA4.w[0], CA2.w[0]);
    __sub_borrow_in_out (CA4.w[1], carry64, CA4.w[1], CA2.w[1],
			 carry64);
    CA4.w[2] = CA4.w[2] - CA2.w[2] - carry64;

    CQT.w[1] = Q >> (64 - 49);
    CQT.w[0] = Q << 49;
    __add_128_128 (CQ, CQ, CQT);

    lx = ((double) CA4.w[2] * d128 +
	  ((double) CA4.w[1] * t64.d + (double) CA4.w[0]));
    lq = lx / ly;
  }

  Q = (BID_UINT64) lq;
  __mul_64x64_to_128 (A2, Q, CY.w[0]);
  A2.w[1] += Q * CY.w[1];

  __sub_128_128 (CA4, CA4, A2);
  if ((BID_SINT64) CA4.w[1] < 0) {
    Q--;
    CA4.w[0] += CY.w[0];
    if (CA4.w[0] < CY.w[0])
      CA4.w[1]++;
    CA4.w[1] += CY.w[1];
    if ((BID_SINT64) CA4.w[1] < 0) {
      Q--;
      CA4.w[0] += CY.w[0];
      if (CA4.w[0] < CY.w[0])
	CA4.w[1]++;
      CA4.w[1] += CY.w[1];
    }
  } else if (__unsigned_compare_ge_128 (CA4, CY)) {
    Q++;
    __sub_128_128 (CA4, CA4, CY);
  }

  __add_128_64 (CQ, CQ, Q);

  pCQ->w[1] = CQ.w[1];
  pCQ->w[0] = CQ.w[0];
  pCA4->w[1] = CA4.w[1];
  pCA4->w[0] = CA4.w[0];
  return;



}

#endif
#endif
