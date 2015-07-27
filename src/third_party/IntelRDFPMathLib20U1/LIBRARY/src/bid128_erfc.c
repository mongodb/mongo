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

#include "bid_trans.h"

// 2-part conversion.

BID_EXTERN_C void bid128_to_binary128_2part(BID_F128_TYPE *,BID_F128_TYPE *,BID_UINT128);

// 10^-6000, to create dummy underflowing computation
static BID_UINT128 BID128_10POWN6000 =
 {BID128_LH_INIT( 0x0000000000000001ull, 0x0160000000000000ull )};

static BID_UINT128 BID128_1 = 
 {BID128_LH_INIT( 0x0000000000000001ull, 0x3040000000000000ull )};
 
// Miscellaneous constants  
BID_F128_CONST_DEF( c_2_ov_sqrt_pi, 3fff20dd750429b6, d11ae3a914fed7fe); // 2/sqrt(pi)
BID_F128_CONST_DEF( c_1_ov_sqrt_pi, 3ffe20dd750429b6, d11ae3a914fed7fe); // 1/sqrt(pi)
BID_F128_CONST_DEF( c_one,          3fff000000000000, 0000000000000000); // 1.0
BID_F128_CONST_DEF( c_105,          4005a40000000000, 0000000000000000); // 105
BID_F128_CONST_DEF( c_120,          4005e00000000000, 0000000000000000); // 120
BID_F128_CONST_DEF( c_1em40,        3f7a16c262777579, c58c46475896767b); // 1E-40 

// Polynomial constants
BID_F128_CONST_DEF(c12, 401926841857e3ff, fff920c8098a1091);	// 77205601.3732910156Q
BID_F128_CONST_DEF(c11, c01599c2ea378000, 0000000000000000);	// -6713530.55419921875Q
BID_F128_CONST_DEF(c10, 40123832fb980000, 0000000000000000);	// 639383.8623046875Q
BID_F128_CONST_DEF( c9, c00f06e790800000, 0000000000000000);	// -67303.564453125Q
BID_F128_CONST_DEF( c8, 400beee110000000, 0000000000000000);	// 7918.06640625Q
BID_F128_CONST_DEF( c7, c00907ef80000000, c00907ef80000000);	// -1055.7421875Q
BID_F128_CONST_DEF( c6, 400644d800000000, 0000000000000000);	// 162.421875Q
BID_F128_CONST_DEF( c5, c003d88000000000, 0000000000000000);	// -29.53125Q
BID_F128_CONST_DEF( c4, 4001a40000000000, 0000000000000000);	// 6.5625Q
BID_F128_CONST_DEF( c3, bfffe00000000000, 0000000000000000);	// -1.875Q
BID_F128_CONST_DEF( c2, 3ffe800000000000, 0000000000000000);	// 0.75Q
BID_F128_CONST_DEF( c1, bffe000000000000, 0000000000000000);	// -0.5Q

BID128_FUNCTION_ARG1 (bid128_erfc, x)

// Declare local variables

  BID_UINT128 res, x2_hi, x2_lo, y_hi, y_lo;
  BID_F128_TYPE xd, ed, wd, yd, zd, xdi, xi2, pd;
  int cmp_res;
  BID_F128_TYPE rt, abs_xd;

// Check for NaN and just return the same NaN, quieted and canonized

  if ((x.w[BID_HIGH_128W] & NAN_MASK64) == NAN_MASK64)
   {
     #ifdef BID_SET_STATUS_FLAGS
     if (((x.w[BID_HIGH_128W] & SNAN_MASK64) == SNAN_MASK64))
        __set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
     #endif
     res.w[BID_HIGH_128W] = x.w[BID_HIGH_128W] & 0xfc003fffffffffffull;
     res.w[BID_LOW_128W] = x.w[BID_LOW_128W];
     if (((res.w[BID_HIGH_128W] & 0x00003fffffffffffull) >
          0x0000314dc6448d93ull) ||
         (((res.w[BID_HIGH_128W] & 0x00003fffffffffffull) ==
            0x0000314dc6448d93ull) &&
          res.w[BID_LOW_128W] >= 0x38c15b0a00000000ull))
      { res.w[BID_HIGH_128W] &= ~0x00003fffffffffffull;
        res.w[BID_LOW_128W] = 0ull;
      }
     BID_RETURN(res);
   }

// If the input is exactly zero, return 1

  BIDECIMAL_CALL1_NORND_NOSTAT (bid128_isZero,cmp_res,x);
  if (cmp_res)
   { res = BID128_1;
     BID_RETURN(res);                                                
   }

// Convert now, for more convenience

  BIDECIMAL_CALL1(bid128_to_binary128,xd,x);

// If the input is very small, just do 1 - x to get directed rounding

   __bid_f128_fabs(abs_xd, xd);
  if (__bid_f128_lt(abs_xd, c_1em40.v))
   { BIDECIMAL_CALL2(bid128_sub,res,BID128_1,x);
     BID_RETURN(res);                                                
   }        

// Check if the input is negative. If it is, then the operation is
// wellconditioned and we can do it naively.

  if (x.w[BID_HIGH_128W] & (1ull<<63))
   { BIDECIMAL_CALL1(bid128_to_binary128,xd,x);
     __bid_f128_erfc(yd, xd);
     BIDECIMAL_CALL1(binary128_to_bid128,res,yd);
     BID_RETURN(res);
   }

// Otherwise, if 0 <= x <= 105, the computation in quad does not underflow.
// However, it's badly conditioned near the top so we always correct it
// using a derivative approximation erfc'(x) = [-2/sqrt(pi)] * exp(-x^2).

  
  if (__bid_f128_lt(xd, c_105.v))
   { BID_F128_TYPE rt, rd;
     bid128_to_binary128_2part(&xd,&ed,x);
     __bid_f128_mul(rt, xd, xd);
     __bid_f128_neg(rt, rt);
     __bid_f128_exp(rt, rt);
     __bid_f128_mul(rt, c_2_ov_sqrt_pi.v, rt);
     __bid_f128_mul(rt, rt, ed);
     __bid_f128_erfc(rd, xd);
     __bid_f128_sub(yd, rd, rt); 
     BIDECIMAL_CALL1(binary128_to_bid128,res,yd);
     BID_RETURN(res);
   }

// Otherwise if x >= 120 then we underflow to zero, so just do
// a dummy underflowing computation for the sake of flags and rounding modes.

  if (__bid_f128_gt(xd, c_120.v))
   { BIDECIMAL_CALL2(bid128_mul,res,BID128_10POWN6000,BID128_10POWN6000);
     BID_RETURN (res);
   }

// In the tricky zone 105 <= x <= 120 there seems to be no easy way of
// using the binary function, so we explicitly use the asymptotic
// expansion.
//
// erfc(z) = [1 / (sqrt(pi) * x e^{x^2})] *
//           [c_0 + c_1 / x^2 + c_2 / (x^2)^ 2 + ...]
//
// where c_n = (-1)^n (1 * 3 * ... * (2 n - 1)) / 2^n
//
// We're OK by using terms up to the 11th, I believe. The coefficients are:
//
// c_0 = 1
// c_1 = -1/2
// c_2 = 3/4
// c_3 = -15/8
// c_4 = 105/16
// c_5 = -945/32
// c_6 = 10395/64
// c_7 = -135135/128
// c_8 = 2027025/256
// c_9 = -34459425/512
// c_10 = 654729075/1024
// c_11 = -13749310575/2048
// c_12 = 316234143225/4096
//
// Note that even the computation of e^{-x^2} itself is illconditioned.
// We need to keep extra precision when squaring.

  BIDECIMAL_CALL2(bid128_mul,x2_hi,x,x);             // x2_hi =~= x^2
  x2_hi.w[BID_HIGH_128W] ^= (1ull<<63);                  // x2_hi =~= -x^2
  BIDECIMAL_CALL3(bid128_fma,x2_lo,x,x,x2_hi);       // x2_hi - x2_lo = -x^2
  x2_lo.w[BID_HIGH_128W] ^= (1ull<<63);                  // x2_hi + x2_lo = -x^2
  BIDECIMAL_CALL1(bid128_exp,y_hi,x2_hi);            // y_hi =~= e^{-x^2}
  BIDECIMAL_CALL3(bid128_fma,y_hi,y_hi,x2_lo,y_hi);  // y_hi = e^{-x^2}

// Compute all the other components but the e^{-x^2} in quad and
// then convert back.

  __bid_f128_div(xdi, c_one.v, xd);  			// xdi = 1.0Q / xd;
  __bid_f128_mul(xi2, xdi, xdi);        			// xi2 = xdi * xdi;

  __bid_f128_mul(pd, xi2, c12.v); 	// pd = -6713530.55419921875Q + xi2 * 77205601.3732910156Q;
  __bid_f128_add(pd, c11.v, pd);
  __bid_f128_mul(pd, xi2, pd);
  __bid_f128_add(pd, c10.v, pd);        // pd = 639383.8623046875Q + xi2 * pd;
  __bid_f128_mul(pd, xi2, pd);
  __bid_f128_add(pd, c9.v, pd);		// pd = -67303.564453125Q + xi2 * pd;
  __bid_f128_mul(pd, xi2, pd);
  __bid_f128_add(pd, c8.v, pd);		// pd = 7918.06640625Q + xi2 * pd;
  __bid_f128_mul(pd, xi2, pd);
  __bid_f128_add(pd, c7.v, pd);		// pd = -1055.7421875Q + xi2 * pd;
  __bid_f128_mul(pd, xi2, pd);
  __bid_f128_add(pd, c6.v, pd);		// pd = 162.421875Q + xi2 * pd;
  __bid_f128_mul(pd, xi2, pd);
  __bid_f128_add(pd, c5.v, pd);		// pd = -29.53125Q + xi2 * pd;
  __bid_f128_mul(pd, xi2, pd);
  __bid_f128_add(pd, c4.v, pd);		// pd = 6.5625Q + xi2 * pd;
  __bid_f128_mul(pd, xi2, pd);
  __bid_f128_add(pd, c3.v, pd);		// pd = -1.875Q + xi2 * pd;
  __bid_f128_mul(pd, xi2, pd);
  __bid_f128_add(pd, c2.v, pd);		// pd = 0.75Q + xi2 * pd;
  __bid_f128_mul(pd, xi2, pd);
  __bid_f128_add(pd, c1.v, pd);		// pd = -0.5Q + xi2 * pd;
  __bid_f128_mul(pd, xi2, pd);
  __bid_f128_add(pd, c_one.v, pd);	// pd = 1.0Q + xi2 * pd;
  __bid_f128_mul(rt, xdi, c_1_ov_sqrt_pi.v);
					// rt = xdi / sqrt(pi)
  __bid_f128_mul(pd, rt, pd);		// pd = pd*xdi/sqrt(pi)
  BIDECIMAL_CALL1(binary128_to_bid128,y_lo,pd);

// Multiply them together.

  BIDECIMAL_CALL2(bid128_mul,res,y_hi,y_lo);
  BID_RETURN (res);
}
