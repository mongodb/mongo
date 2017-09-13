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

/*****************************************************************************
 *
 *  BID64 encoding:
 * ****************************************
 *  63  62              53 52           0
 * |---|------------------|--------------|
 * | S |  Biased Exp (E)  |  Coeff (c)   |
 * |---|------------------|--------------|
 *
 * bias = 398
 * number = (-1)^s * 10^(E-398) * c
 * coefficient range - 0 to (2^53)-1
 * COEFF_MAX = 2^53-1 = 9007199254740991
 *
 *****************************************************************************
 *
 * BID128 encoding:
 *   1-bit sign
 *   14-bit biased exponent in [0x21, 0x3020] = [33, 12320]
 *         unbiased exponent in [-6176, 6111]; exponent bias = 6176
 *   113-bit unsigned binary integer coefficient (49-bit high + 64-bit low)
 *   Note: 10^34-1 ~ 2^112.945555... < 2^113 => coefficient fits in 113 bits
 *
 *   Note: assume invalid encodings are not passed to this function
 *
 * Round a number C with q decimal digits, represented as a binary integer
 * to q - x digits. Six different routines are provided for different values 
 * of q. The maximum value of q used in the library is q = 3 * P - 1 where 
 * P = 16 or P = 34 (so q <= 111 decimal digits). 
 * The partitioning is based on the following, where Kx is the scaled
 * integer representing the value of 10^(-x) rounded up to a number of bits
 * sufficient to ensure correct rounding:
 *
 * --------------------------------------------------------------------------
 * q    x           max. value of  a            max number      min. number 
 *                                              of bits in C    of bits in Kx
 * --------------------------------------------------------------------------
 *
 *                          GROUP 1: 64 bits
 *                          bid_round64_2_18 ()
 *
 * 2    [1,1]       10^1 - 1 < 2^3.33            4              4
 * ...  ...         ...                         ...             ...
 * 18   [1,17]      10^18 - 1 < 2^59.80         60              61
 *
 *                        GROUP 2: 128 bits
 *                        bid_round128_19_38 ()
 *
 * 19   [1,18]      10^19 - 1 < 2^63.11         64              65
 * 20   [1,19]      10^20 - 1 < 2^66.44         67              68
 * ...  ...         ...                         ...             ...
 * 38   [1,37]      10^38 - 1 < 2^126.24        127             128
 *
 *                        GROUP 3: 192 bits
 *                        bid_round192_39_57 ()
 *
 * 39   [1,38]      10^39 - 1 < 2^129.56        130             131
 * ...  ...         ...                         ...             ...
 * 57   [1,56]      10^57 - 1 < 2^189.35        190             191
 *
 *                        GROUP 4: 256 bits
 *                        bid_round256_58_76 ()
 *
 * 58   [1,57]      10^58 - 1 < 2^192.68        193             194
 * ...  ...         ...                         ...             ...
 * 76   [1,75]      10^76 - 1 < 2^252.47        253             254
 *
 *                        GROUP 5: 320 bits
 *                        round320_77_96 ()
 *
 * 77   [1,76]      10^77 - 1 < 2^255.79        256             257
 * 78   [1,77]      10^78 - 1 < 2^259.12        260             261
 * ...  ...         ...                         ...             ...
 * 96   [1,95]      10^96 - 1 < 2^318.91        319             320
 *
 *                        GROUP 6: 384 bits
 *                        round384_97_115 ()
 *
 * 97   [1,96]      10^97 - 1 < 2^322.23        323             324 
 * ...  ...         ...                         ...             ...
 * 115  [1,114]     10^115 - 1 < 2^382.03       383             384
 *
 ****************************************************************************/

#include "bid_internal.h"

void bid_round64_2_18 (int q,
	      int x,
	      BID_UINT64 C,
	      BID_UINT64 * ptr_Cstar,
	      int *incr_exp,
	      int *ptr_is_midpoint_lt_even,
	      int *ptr_is_midpoint_gt_even,
	      int *ptr_is_inexact_lt_midpoint,
	      int *ptr_is_inexact_gt_midpoint) {

  BID_UINT128 P128;
  BID_UINT128 fstar;
  BID_UINT64 Cstar;
  BID_UINT64 tmp64;
  int shift;
  int ind;

  // Note:
  //    In round128_2_18() positive numbers with 2 <= q <= 18 will be 
  //    rounded to nearest only for 1 <= x <= 3:
  //     x = 1 or x = 2 when q = 17
  //     x = 2 or x = 3 when q = 18
  // However, for generality and possible uses outside the frame of IEEE 754
  // this implementation works for 1 <= x <= q - 1

  // assume *ptr_is_midpoint_lt_even, *ptr_is_midpoint_gt_even,
  // *ptr_is_inexact_lt_midpoint, and *ptr_is_inexact_gt_midpoint are
  // initialized to 0 by the caller

  // round a number C with q decimal digits, 2 <= q <= 18
  // to q - x digits, 1 <= x <= 17
  // C = C + 1/2 * 10^x where the result C fits in 64 bits
  // (because the largest value is 999999999999999999 + 50000000000000000 =
  // 0x0e92596fd628ffff, which fits in 60 bits)
  ind = x - 1;	// 0 <= ind <= 16
  C = C + bid_midpoint64[ind];
  // kx ~= 10^(-x), kx = bid_Kx64[ind] * 2^(-Ex), 0 <= ind <= 16
  // P128 = (C + 1/2 * 10^x) * kx * 2^Ex = (C + 1/2 * 10^x) * Kx
  // the approximation kx of 10^(-x) was rounded up to 64 bits
  __mul_64x64_to_128MACH (P128, C, bid_Kx64[ind]);
  // calculate C* = floor (P128) and f*
  // Cstar = P128 >> Ex
  // fstar = low Ex bits of P128
  shift = bid_Ex64m64[ind];	// in [3, 56]
  Cstar = P128.w[1] >> shift;
  fstar.w[1] = P128.w[1] & bid_mask64[ind];
  fstar.w[0] = P128.w[0];
  // the top Ex bits of 10^(-x) are T* = bid_ten2mxtrunc64[ind], e.g.
  // if x=1, T*=bid_ten2mxtrunc64[0]=0xcccccccccccccccc
  // if (0 < f* < 10^(-x)) then the result is a midpoint
  //   if floor(C*) is even then C* = floor(C*) - logical right
  //       shift; C* has q - x decimal digits, correct by Prop. 1)
  //   else if floor(C*) is odd C* = floor(C*)-1 (logical right
  //       shift; C* has q - x decimal digits, correct by Pr. 1)
  // else
  //   C* = floor(C*) (logical right shift; C has q - x decimal digits,
  //       correct by Property 1)
  // in the caling function n = C* * 10^(e+x)

  // determine inexactness of the rounding of C*
  // if (0 < f* - 1/2 < 10^(-x)) then
  //   the result is exact
  // else // if (f* - 1/2 > T*) then
  //   the result is inexact
  if (fstar.w[1] > bid_half64[ind] ||
      (fstar.w[1] == bid_half64[ind] && fstar.w[0])) {
    // f* > 1/2 and the result may be exact
    // Calculate f* - 1/2
    tmp64 = fstar.w[1] - bid_half64[ind];
    if (tmp64 || fstar.w[0] > bid_ten2mxtrunc64[ind]) {	// f* - 1/2 > 10^(-x)
      *ptr_is_inexact_lt_midpoint = 1;
    }	// else the result is exact
  } else {	// the result is inexact; f2* <= 1/2
    *ptr_is_inexact_gt_midpoint = 1;
  }
  // check for midpoints (could do this before determining inexactness)
  if (fstar.w[1] == 0 && fstar.w[0] <= bid_ten2mxtrunc64[ind]) {
    // the result is a midpoint
    if (Cstar & 0x01) {	// Cstar is odd; MP in [EVEN, ODD]
      // if floor(C*) is odd C = floor(C*) - 1; the result may be 0
      Cstar--;	// Cstar is now even
      *ptr_is_midpoint_gt_even = 1;
      *ptr_is_inexact_lt_midpoint = 0;
      *ptr_is_inexact_gt_midpoint = 0;
    } else {	// else MP in [ODD, EVEN]
      *ptr_is_midpoint_lt_even = 1;
      *ptr_is_inexact_lt_midpoint = 0;
      *ptr_is_inexact_gt_midpoint = 0;
    }
  }
  // check for rounding overflow, which occurs if Cstar = 10^(q-x)
  ind = q - x;	// 1 <= ind <= q - 1
  if (Cstar == bid_ten2k64[ind]) {	// if  Cstar = 10^(q-x)
    Cstar = bid_ten2k64[ind - 1];	// Cstar = 10^(q-x-1)
    *incr_exp = 1;
  } else {	// 10^33 <= Cstar <= 10^34 - 1
    *incr_exp = 0;
  }
  *ptr_Cstar = Cstar;
}


void bid_round128_19_38 (int q,
		int x,
		BID_UINT128 C,
		BID_UINT128 * ptr_Cstar,
		int *incr_exp,
		int *ptr_is_midpoint_lt_even,
		int *ptr_is_midpoint_gt_even,
		int *ptr_is_inexact_lt_midpoint,
		int *ptr_is_inexact_gt_midpoint) {

  BID_UINT256 P256;
  BID_UINT256 fstar;
  BID_UINT128 Cstar;
  BID_UINT64 tmp64;
  int shift;
  int ind;

  // Note:
  //    In bid_round128_19_38() positive numbers with 19 <= q <= 38 will be 
  //    rounded to nearest only for 1 <= x <= 23:
  //     x = 3 or x = 4 when q = 19
  //     x = 4 or x = 5 when q = 20
  //     ...
  //     x = 18 or x = 19 when q = 34
  //     x = 1 or x = 2 or x = 19 or x = 20 when q = 35
  //     x = 2 or x = 3 or x = 20 or x = 21 when q = 36
  //     x = 3 or x = 4 or x = 21 or x = 22 when q = 37
  //     x = 4 or x = 5 or x = 22 or x = 23 when q = 38
  // However, for generality and possible uses outside the frame of IEEE 754
  // this implementation works for 1 <= x <= q - 1

  // assume *ptr_is_midpoint_lt_even, *ptr_is_midpoint_gt_even,
  // *ptr_is_inexact_lt_midpoint, and *ptr_is_inexact_gt_midpoint are
  // initialized to 0 by the caller

  // round a number C with q decimal digits, 19 <= q <= 38
  // to q - x digits, 1 <= x <= 37
  // C = C + 1/2 * 10^x where the result C fits in 128 bits
  // (because the largest value is 99999999999999999999999999999999999999 + 
  // 5000000000000000000000000000000000000 =
  // 0x4efe43b0c573e7e68a043d8fffffffff, which fits is 127 bits)

  ind = x - 1;	// 0 <= ind <= 36 
  if (ind <= 18) {	// if 0 <= ind <= 18
    tmp64 = C.w[0];
    C.w[0] = C.w[0] + bid_midpoint64[ind];
    if (C.w[0] < tmp64)
      C.w[1]++;
  } else {	// if 19 <= ind <= 37
    tmp64 = C.w[0];
    C.w[0] = C.w[0] + bid_midpoint128[ind - 19].w[0];
    if (C.w[0] < tmp64) {
      C.w[1]++;
    }
    C.w[1] = C.w[1] + bid_midpoint128[ind - 19].w[1];
  }
  // kx ~= 10^(-x), kx = bid_Kx128[ind] * 2^(-Ex), 0 <= ind <= 36
  // P256 = (C + 1/2 * 10^x) * kx * 2^Ex = (C + 1/2 * 10^x) * Kx
  // the approximation kx of 10^(-x) was rounded up to 128 bits
  __mul_128x128_to_256 (P256, C, bid_Kx128[ind]);
  // calculate C* = floor (P256) and f*
  // Cstar = P256 >> Ex
  // fstar = low Ex bits of P256
  shift = bid_Ex128m128[ind];	// in [2, 63] but have to consider two cases
  if (ind <= 18) {	// if 0 <= ind <= 18 
    Cstar.w[0] = (P256.w[2] >> shift) | (P256.w[3] << (64 - shift));
    Cstar.w[1] = (P256.w[3] >> shift);
    fstar.w[0] = P256.w[0];
    fstar.w[1] = P256.w[1];
    fstar.w[2] = P256.w[2] & bid_mask128[ind];
    fstar.w[3] = 0x0ULL;
  } else {	// if 19 <= ind <= 37
    Cstar.w[0] = P256.w[3] >> shift;
    Cstar.w[1] = 0x0ULL;
    fstar.w[0] = P256.w[0];
    fstar.w[1] = P256.w[1];
    fstar.w[2] = P256.w[2];
    fstar.w[3] = P256.w[3] & bid_mask128[ind];
  }
  // the top Ex bits of 10^(-x) are T* = bid_ten2mxtrunc64[ind], e.g.
  // if x=1, T*=bid_ten2mxtrunc128[0]=0xcccccccccccccccccccccccccccccccc
  // if (0 < f* < 10^(-x)) then the result is a midpoint
  //   if floor(C*) is even then C* = floor(C*) - logical right
  //       shift; C* has q - x decimal digits, correct by Prop. 1)
  //   else if floor(C*) is odd C* = floor(C*)-1 (logical right
  //       shift; C* has q - x decimal digits, correct by Pr. 1)
  // else
  //   C* = floor(C*) (logical right shift; C has q - x decimal digits,
  //       correct by Property 1)
  // in the caling function n = C* * 10^(e+x)

  // determine inexactness of the rounding of C*
  // if (0 < f* - 1/2 < 10^(-x)) then
  //   the result is exact
  // else // if (f* - 1/2 > T*) then
  //   the result is inexact
  if (ind <= 18) {	// if 0 <= ind <= 18
    if (fstar.w[2] > bid_half128[ind] ||
	(fstar.w[2] == bid_half128[ind] && (fstar.w[1] || fstar.w[0]))) {
      // f* > 1/2 and the result may be exact
      // Calculate f* - 1/2
      tmp64 = fstar.w[2] - bid_half128[ind];
      if (tmp64 || fstar.w[1] > bid_ten2mxtrunc128[ind].w[1] || (fstar.w[1] == bid_ten2mxtrunc128[ind].w[1] && fstar.w[0] > bid_ten2mxtrunc128[ind].w[0])) {	// f* - 1/2 > 10^(-x)
	*ptr_is_inexact_lt_midpoint = 1;
      }	// else the result is exact
    } else {	// the result is inexact; f2* <= 1/2
      *ptr_is_inexact_gt_midpoint = 1;
    }
  } else {	// if 19 <= ind <= 37
    if (fstar.w[3] > bid_half128[ind] || (fstar.w[3] == bid_half128[ind] &&
				      (fstar.w[2] || fstar.w[1]
				       || fstar.w[0]))) {
      // f* > 1/2 and the result may be exact
      // Calculate f* - 1/2
      tmp64 = fstar.w[3] - bid_half128[ind];
      if (tmp64 || fstar.w[2] || fstar.w[1] > bid_ten2mxtrunc128[ind].w[1] || (fstar.w[1] == bid_ten2mxtrunc128[ind].w[1] && fstar.w[0] > bid_ten2mxtrunc128[ind].w[0])) {	// f* - 1/2 > 10^(-x)
	*ptr_is_inexact_lt_midpoint = 1;
      }	// else the result is exact
    } else {	// the result is inexact; f2* <= 1/2
      *ptr_is_inexact_gt_midpoint = 1;
    }
  }
  // check for midpoints (could do this before determining inexactness)
  if (fstar.w[3] == 0 && fstar.w[2] == 0 &&
      (fstar.w[1] < bid_ten2mxtrunc128[ind].w[1] ||
       (fstar.w[1] == bid_ten2mxtrunc128[ind].w[1] &&
	fstar.w[0] <= bid_ten2mxtrunc128[ind].w[0]))) {
    // the result is a midpoint
    if (Cstar.w[0] & 0x01) {	// Cstar is odd; MP in [EVEN, ODD]
      // if floor(C*) is odd C = floor(C*) - 1; the result may be 0
      Cstar.w[0]--;	// Cstar is now even
      if (Cstar.w[0] == 0xffffffffffffffffULL) {
	Cstar.w[1]--;
      }
      *ptr_is_midpoint_gt_even = 1;
      *ptr_is_inexact_lt_midpoint = 0;
      *ptr_is_inexact_gt_midpoint = 0;
    } else {	// else MP in [ODD, EVEN]
      *ptr_is_midpoint_lt_even = 1;
      *ptr_is_inexact_lt_midpoint = 0;
      *ptr_is_inexact_gt_midpoint = 0;
    }
  }
  // check for rounding overflow, which occurs if Cstar = 10^(q-x)
  ind = q - x;	// 1 <= ind <= q - 1
  if (ind <= 19) {
    if (Cstar.w[1] == 0x0ULL && Cstar.w[0] == bid_ten2k64[ind]) {
      // if  Cstar = 10^(q-x)
      Cstar.w[0] = bid_ten2k64[ind - 1];	// Cstar = 10^(q-x-1)
      *incr_exp = 1;
    } else {
      *incr_exp = 0;
    }
  } else if (ind == 20) {
    // if ind = 20
    if (Cstar.w[1] == bid_ten2k128[0].w[1]
	&& Cstar.w[0] == bid_ten2k128[0].w[0]) {
      // if  Cstar = 10^(q-x)
      Cstar.w[0] = bid_ten2k64[19];	// Cstar = 10^(q-x-1)
      Cstar.w[1] = 0x0ULL;
      *incr_exp = 1;
    } else {
      *incr_exp = 0;
    }
  } else {	// if 21 <= ind <= 37
    if (Cstar.w[1] == bid_ten2k128[ind - 20].w[1] &&
	Cstar.w[0] == bid_ten2k128[ind - 20].w[0]) {
      // if  Cstar = 10^(q-x)
      Cstar.w[0] = bid_ten2k128[ind - 21].w[0];	// Cstar = 10^(q-x-1)
      Cstar.w[1] = bid_ten2k128[ind - 21].w[1];
      *incr_exp = 1;
    } else {
      *incr_exp = 0;
    }
  }
  ptr_Cstar->w[1] = Cstar.w[1];
  ptr_Cstar->w[0] = Cstar.w[0];
}


void bid_round192_39_57 (int q,
		int x,
		BID_UINT192 C,
		BID_UINT192 * ptr_Cstar,
		int *incr_exp,
		int *ptr_is_midpoint_lt_even,
		int *ptr_is_midpoint_gt_even,
		int *ptr_is_inexact_lt_midpoint,
		int *ptr_is_inexact_gt_midpoint) {

  BID_UINT384 P384;
  BID_UINT384 fstar;
  BID_UINT192 Cstar;
  BID_UINT64 tmp64;
  int shift;
  int ind;

  // Note:
  //    In bid_round192_39_57() positive numbers with 39 <= q <= 57 will be 
  //    rounded to nearest only for 5 <= x <= 42:
  //     x = 23 or x = 24 or x = 5 or x = 6 when q = 39
  //     x = 24 or x = 25 or x = 6 or x = 7 when q = 40
  //     ...
  //     x = 41 or x = 42 or x = 23 or x = 24 when q = 57
  // However, for generality and possible uses outside the frame of IEEE 754
  // this implementation works for 1 <= x <= q - 1

  // assume *ptr_is_midpoint_lt_even, *ptr_is_midpoint_gt_even,
  // *ptr_is_inexact_lt_midpoint, and *ptr_is_inexact_gt_midpoint are
  // initialized to 0 by the caller

  // round a number C with q decimal digits, 39 <= q <= 57
  // to q - x digits, 1 <= x <= 56
  // C = C + 1/2 * 10^x where the result C fits in 192 bits
  // (because the largest value is
  // 999999999999999999999999999999999999999999999999999999999 +
  //  50000000000000000000000000000000000000000000000000000000 =
  // 0x2ad282f212a1da846afdaf18c034ff09da7fffffffffffff, which fits in 190 bits)
  ind = x - 1;	// 0 <= ind <= 55
  if (ind <= 18) {	// if 0 <= ind <= 18
    tmp64 = C.w[0];
    C.w[0] = C.w[0] + bid_midpoint64[ind];
    if (C.w[0] < tmp64) {
      C.w[1]++;
      if (C.w[1] == 0x0) {
	C.w[2]++;
      }
    }
  } else if (ind <= 37) {	// if 19 <= ind <= 37
    tmp64 = C.w[0];
    C.w[0] = C.w[0] + bid_midpoint128[ind - 19].w[0];
    if (C.w[0] < tmp64) {
      C.w[1]++;
      if (C.w[1] == 0x0) {
	C.w[2]++;
      }
    }
    tmp64 = C.w[1];
    C.w[1] = C.w[1] + bid_midpoint128[ind - 19].w[1];
    if (C.w[1] < tmp64) {
      C.w[2]++;
    }
  } else {	// if 38 <= ind <= 57 (actually ind <= 55)
    tmp64 = C.w[0];
    C.w[0] = C.w[0] + bid_midpoint192[ind - 38].w[0];
    if (C.w[0] < tmp64) {
      C.w[1]++;
      if (C.w[1] == 0x0ull) {
	C.w[2]++;
      }
    }
    tmp64 = C.w[1];
    C.w[1] = C.w[1] + bid_midpoint192[ind - 38].w[1];
    if (C.w[1] < tmp64) {
      C.w[2]++;
    }
    C.w[2] = C.w[2] + bid_midpoint192[ind - 38].w[2];
  }
  // kx ~= 10^(-x), kx = bid_Kx192[ind] * 2^(-Ex), 0 <= ind <= 55
  // P384 = (C + 1/2 * 10^x) * kx * 2^Ex = (C + 1/2 * 10^x) * Kx
  // the approximation kx of 10^(-x) was rounded up to 192 bits
  __mul_192x192_to_384 (P384, C, bid_Kx192[ind]);
  // calculate C* = floor (P384) and f*
  // Cstar = P384 >> Ex
  // fstar = low Ex bits of P384
  shift = bid_Ex192m192[ind];	// in [1, 63] but have to consider three cases
  if (ind <= 18) {	// if 0 <= ind <= 18 
    Cstar.w[2] = (P384.w[5] >> shift);
    Cstar.w[1] = (P384.w[5] << (64 - shift)) | (P384.w[4] >> shift);
    Cstar.w[0] = (P384.w[4] << (64 - shift)) | (P384.w[3] >> shift);
    fstar.w[5] = 0x0ULL;
    fstar.w[4] = 0x0ULL;
    fstar.w[3] = P384.w[3] & bid_mask192[ind];
    fstar.w[2] = P384.w[2];
    fstar.w[1] = P384.w[1];
    fstar.w[0] = P384.w[0];
  } else if (ind <= 37) {	// if 19 <= ind <= 37
    Cstar.w[2] = 0x0ULL;
    Cstar.w[1] = P384.w[5] >> shift;
    Cstar.w[0] = (P384.w[5] << (64 - shift)) | (P384.w[4] >> shift);
    fstar.w[5] = 0x0ULL;
    fstar.w[4] = P384.w[4] & bid_mask192[ind];
    fstar.w[3] = P384.w[3];
    fstar.w[2] = P384.w[2];
    fstar.w[1] = P384.w[1];
    fstar.w[0] = P384.w[0];
  } else {	// if 38 <= ind <= 57
    Cstar.w[2] = 0x0ULL;
    Cstar.w[1] = 0x0ULL;
    Cstar.w[0] = P384.w[5] >> shift;
    fstar.w[5] = P384.w[5] & bid_mask192[ind];
    fstar.w[4] = P384.w[4];
    fstar.w[3] = P384.w[3];
    fstar.w[2] = P384.w[2];
    fstar.w[1] = P384.w[1];
    fstar.w[0] = P384.w[0];
  }

  // the top Ex bits of 10^(-x) are T* = bid_ten2mxtrunc192[ind], e.g. if x=1,
  // T*=bid_ten2mxtrunc192[0]=0xcccccccccccccccccccccccccccccccccccccccccccccccc
  // if (0 < f* < 10^(-x)) then the result is a midpoint
  //   if floor(C*) is even then C* = floor(C*) - logical right
  //       shift; C* has q - x decimal digits, correct by Prop. 1)
  //   else if floor(C*) is odd C* = floor(C*)-1 (logical right
  //       shift; C* has q - x decimal digits, correct by Pr. 1)
  // else
  //   C* = floor(C*) (logical right shift; C has q - x decimal digits,
  //       correct by Property 1)
  // in the caling function n = C* * 10^(e+x)

  // determine inexactness of the rounding of C*
  // if (0 < f* - 1/2 < 10^(-x)) then
  //   the result is exact
  // else // if (f* - 1/2 > T*) then
  //   the result is inexact
  if (ind <= 18) {	// if 0 <= ind <= 18
    if (fstar.w[3] > bid_half192[ind] || (fstar.w[3] == bid_half192[ind] &&
				      (fstar.w[2] || fstar.w[1]
				       || fstar.w[0]))) {
      // f* > 1/2 and the result may be exact
      // Calculate f* - 1/2
      tmp64 = fstar.w[3] - bid_half192[ind];
      if (tmp64 || fstar.w[2] > bid_ten2mxtrunc192[ind].w[2] || (fstar.w[2] == bid_ten2mxtrunc192[ind].w[2] && fstar.w[1] > bid_ten2mxtrunc192[ind].w[1]) || (fstar.w[2] == bid_ten2mxtrunc192[ind].w[2] && fstar.w[1] == bid_ten2mxtrunc192[ind].w[1] && fstar.w[0] > bid_ten2mxtrunc192[ind].w[0])) {	// f* - 1/2 > 10^(-x)
	*ptr_is_inexact_lt_midpoint = 1;
      }	// else the result is exact
    } else {	// the result is inexact; f2* <= 1/2
      *ptr_is_inexact_gt_midpoint = 1;
    }
  } else if (ind <= 37) {	// if 19 <= ind <= 37
    if (fstar.w[4] > bid_half192[ind] || (fstar.w[4] == bid_half192[ind] &&
				      (fstar.w[3] || fstar.w[2]
				       || fstar.w[1] || fstar.w[0]))) {
      // f* > 1/2 and the result may be exact
      // Calculate f* - 1/2
      tmp64 = fstar.w[4] - bid_half192[ind];
      if (tmp64 || fstar.w[3] || fstar.w[2] > bid_ten2mxtrunc192[ind].w[2] || (fstar.w[2] == bid_ten2mxtrunc192[ind].w[2] && fstar.w[1] > bid_ten2mxtrunc192[ind].w[1]) || (fstar.w[2] == bid_ten2mxtrunc192[ind].w[2] && fstar.w[1] == bid_ten2mxtrunc192[ind].w[1] && fstar.w[0] > bid_ten2mxtrunc192[ind].w[0])) {	// f* - 1/2 > 10^(-x)
	*ptr_is_inexact_lt_midpoint = 1;
      }	// else the result is exact
    } else {	// the result is inexact; f2* <= 1/2
      *ptr_is_inexact_gt_midpoint = 1;
    }
  } else {	// if 38 <= ind <= 55
    if (fstar.w[5] > bid_half192[ind] || (fstar.w[5] == bid_half192[ind] &&
				      (fstar.w[4] || fstar.w[3]
				       || fstar.w[2] || fstar.w[1]
				       || fstar.w[0]))) {
      // f* > 1/2 and the result may be exact
      // Calculate f* - 1/2
      tmp64 = fstar.w[5] - bid_half192[ind];
      if (tmp64 || fstar.w[4] || fstar.w[3] || fstar.w[2] > bid_ten2mxtrunc192[ind].w[2] || (fstar.w[2] == bid_ten2mxtrunc192[ind].w[2] && fstar.w[1] > bid_ten2mxtrunc192[ind].w[1]) || (fstar.w[2] == bid_ten2mxtrunc192[ind].w[2] && fstar.w[1] == bid_ten2mxtrunc192[ind].w[1] && fstar.w[0] > bid_ten2mxtrunc192[ind].w[0])) {	// f* - 1/2 > 10^(-x)
	*ptr_is_inexact_lt_midpoint = 1;
      }	// else the result is exact
    } else {	// the result is inexact; f2* <= 1/2
      *ptr_is_inexact_gt_midpoint = 1;
    }
  }
  // check for midpoints (could do this before determining inexactness)
  if (fstar.w[5] == 0 && fstar.w[4] == 0 && fstar.w[3] == 0 &&
      (fstar.w[2] < bid_ten2mxtrunc192[ind].w[2] ||
       (fstar.w[2] == bid_ten2mxtrunc192[ind].w[2] &&
	fstar.w[1] < bid_ten2mxtrunc192[ind].w[1]) ||
       (fstar.w[2] == bid_ten2mxtrunc192[ind].w[2] &&
	fstar.w[1] == bid_ten2mxtrunc192[ind].w[1] &&
	fstar.w[0] <= bid_ten2mxtrunc192[ind].w[0]))) {
    // the result is a midpoint
    if (Cstar.w[0] & 0x01) {	// Cstar is odd; MP in [EVEN, ODD]
      // if floor(C*) is odd C = floor(C*) - 1; the result may be 0
      Cstar.w[0]--;	// Cstar is now even
      if (Cstar.w[0] == 0xffffffffffffffffULL) {
	Cstar.w[1]--;
	if (Cstar.w[1] == 0xffffffffffffffffULL) {
	  Cstar.w[2]--;
	}
      }
      *ptr_is_midpoint_gt_even = 1;
      *ptr_is_inexact_lt_midpoint = 0;
      *ptr_is_inexact_gt_midpoint = 0;
    } else {	// else MP in [ODD, EVEN]
      *ptr_is_midpoint_lt_even = 1;
      *ptr_is_inexact_lt_midpoint = 0;
      *ptr_is_inexact_gt_midpoint = 0;
    }
  }
  // check for rounding overflow, which occurs if Cstar = 10^(q-x)
  ind = q - x;	// 1 <= ind <= q - 1
  if (ind <= 19) {
    if (Cstar.w[2] == 0x0ULL && Cstar.w[1] == 0x0ULL &&
	Cstar.w[0] == bid_ten2k64[ind]) {
      // if  Cstar = 10^(q-x)
      Cstar.w[0] = bid_ten2k64[ind - 1];	// Cstar = 10^(q-x-1)
      *incr_exp = 1;
    } else {
      *incr_exp = 0;
    }
  } else if (ind == 20) {
    // if ind = 20
    if (Cstar.w[2] == 0x0ULL && Cstar.w[1] == bid_ten2k128[0].w[1] &&
	Cstar.w[0] == bid_ten2k128[0].w[0]) {
      // if  Cstar = 10^(q-x)
      Cstar.w[0] = bid_ten2k64[19];	// Cstar = 10^(q-x-1)
      Cstar.w[1] = 0x0ULL;
      *incr_exp = 1;
    } else {
      *incr_exp = 0;
    }
  } else if (ind <= 38) {	// if 21 <= ind <= 38
    if (Cstar.w[2] == 0x0ULL && Cstar.w[1] == bid_ten2k128[ind - 20].w[1] &&
	Cstar.w[0] == bid_ten2k128[ind - 20].w[0]) {
      // if  Cstar = 10^(q-x)
      Cstar.w[0] = bid_ten2k128[ind - 21].w[0];	// Cstar = 10^(q-x-1)
      Cstar.w[1] = bid_ten2k128[ind - 21].w[1];
      *incr_exp = 1;
    } else {
      *incr_exp = 0;
    }
  } else if (ind == 39) {
    if (Cstar.w[2] == bid_ten2k256[0].w[2] && Cstar.w[1] == bid_ten2k256[0].w[1]
	&& Cstar.w[0] == bid_ten2k256[0].w[0]) {
      // if  Cstar = 10^(q-x)
      Cstar.w[0] = bid_ten2k128[18].w[0];	// Cstar = 10^(q-x-1)
      Cstar.w[1] = bid_ten2k128[18].w[1];
      Cstar.w[2] = 0x0ULL;
      *incr_exp = 1;
    } else {
      *incr_exp = 0;
    }
  } else {	// if 40 <= ind <= 56
    if (Cstar.w[2] == bid_ten2k256[ind - 39].w[2] &&
	Cstar.w[1] == bid_ten2k256[ind - 39].w[1] &&
	Cstar.w[0] == bid_ten2k256[ind - 39].w[0]) {
      // if  Cstar = 10^(q-x)
      Cstar.w[0] = bid_ten2k256[ind - 40].w[0];	// Cstar = 10^(q-x-1)
      Cstar.w[1] = bid_ten2k256[ind - 40].w[1];
      Cstar.w[2] = bid_ten2k256[ind - 40].w[2];
      *incr_exp = 1;
    } else {
      *incr_exp = 0;
    }
  }
  ptr_Cstar->w[2] = Cstar.w[2];
  ptr_Cstar->w[1] = Cstar.w[1];
  ptr_Cstar->w[0] = Cstar.w[0];
}


void bid_round256_58_76 (int q,
		int x,
		BID_UINT256 C,
		BID_UINT256 * ptr_Cstar,
		int *incr_exp,
		int *ptr_is_midpoint_lt_even,
		int *ptr_is_midpoint_gt_even,
		int *ptr_is_inexact_lt_midpoint,
		int *ptr_is_inexact_gt_midpoint) {

  BID_UINT512 P512;
  BID_UINT512 fstar;
  BID_UINT256 Cstar;
  BID_UINT64 tmp64;
  int shift;
  int ind;

  // Note:
  //    In bid_round256_58_76() positive numbers with 58 <= q <= 76 will be 
  //    rounded to nearest only for 24 <= x <= 61:
  //     x = 42 or x = 43 or x = 24 or x = 25 when q = 58
  //     x = 43 or x = 44 or x = 25 or x = 26 when q = 59
  //     ...
  //     x = 60 or x = 61 or x = 42 or x = 43 when q = 76
  // However, for generality and possible uses outside the frame of IEEE 754
  // this implementation works for 1 <= x <= q - 1

  // assume *ptr_is_midpoint_lt_even, *ptr_is_midpoint_gt_even,
  // *ptr_is_inexact_lt_midpoint, and *ptr_is_inexact_gt_midpoint are
  // initialized to 0 by the caller

  // round a number C with q decimal digits, 58 <= q <= 76
  // to q - x digits, 1 <= x <= 75
  // C = C + 1/2 * 10^x where the result C fits in 256 bits
  // (because the largest value is 9999999999999999999999999999999999999999
  //     999999999999999999999999999999999999 + 500000000000000000000000000
  //     000000000000000000000000000000000000000000000000 =
  //     0x1736ca15d27a56cae15cf0e7b403d1f2bd6ebb0a50dc83ffffffffffffffffff, 
  // which fits in 253 bits)
  ind = x - 1;	// 0 <= ind <= 74
  if (ind <= 18) {	// if 0 <= ind <= 18
    tmp64 = C.w[0];
    C.w[0] = C.w[0] + bid_midpoint64[ind];
    if (C.w[0] < tmp64) {
      C.w[1]++;
      if (C.w[1] == 0x0) {
	C.w[2]++;
	if (C.w[2] == 0x0) {
	  C.w[3]++;
	}
      }
    }
  } else if (ind <= 37) {	// if 19 <= ind <= 37
    tmp64 = C.w[0];
    C.w[0] = C.w[0] + bid_midpoint128[ind - 19].w[0];
    if (C.w[0] < tmp64) {
      C.w[1]++;
      if (C.w[1] == 0x0) {
	C.w[2]++;
	if (C.w[2] == 0x0) {
	  C.w[3]++;
	}
      }
    }
    tmp64 = C.w[1];
    C.w[1] = C.w[1] + bid_midpoint128[ind - 19].w[1];
    if (C.w[1] < tmp64) {
      C.w[2]++;
      if (C.w[2] == 0x0) {
	C.w[3]++;
      }
    }
  } else if (ind <= 57) {	// if 38 <= ind <= 57
    tmp64 = C.w[0];
    C.w[0] = C.w[0] + bid_midpoint192[ind - 38].w[0];
    if (C.w[0] < tmp64) {
      C.w[1]++;
      if (C.w[1] == 0x0ull) {
	C.w[2]++;
	if (C.w[2] == 0x0) {
	  C.w[3]++;
	}
      }
    }
    tmp64 = C.w[1];
    C.w[1] = C.w[1] + bid_midpoint192[ind - 38].w[1];
    if (C.w[1] < tmp64) {
      C.w[2]++;
      if (C.w[2] == 0x0) {
	C.w[3]++;
      }
    }
    tmp64 = C.w[2];
    C.w[2] = C.w[2] + bid_midpoint192[ind - 38].w[2];
    if (C.w[2] < tmp64) {
      C.w[3]++;
    }
  } else {	// if 58 <= ind <= 76 (actually 58 <= ind <= 74)
    tmp64 = C.w[0];
    C.w[0] = C.w[0] + bid_midpoint256[ind - 58].w[0];
    if (C.w[0] < tmp64) {
      C.w[1]++;
      if (C.w[1] == 0x0ull) {
	C.w[2]++;
	if (C.w[2] == 0x0) {
	  C.w[3]++;
	}
      }
    }
    tmp64 = C.w[1];
    C.w[1] = C.w[1] + bid_midpoint256[ind - 58].w[1];
    if (C.w[1] < tmp64) {
      C.w[2]++;
      if (C.w[2] == 0x0) {
	C.w[3]++;
      }
    }
    tmp64 = C.w[2];
    C.w[2] = C.w[2] + bid_midpoint256[ind - 58].w[2];
    if (C.w[2] < tmp64) {
      C.w[3]++;
    }
    C.w[3] = C.w[3] + bid_midpoint256[ind - 58].w[3];
  }
  // kx ~= 10^(-x), kx = bid_Kx256[ind] * 2^(-Ex), 0 <= ind <= 74
  // P512 = (C + 1/2 * 10^x) * kx * 2^Ex = (C + 1/2 * 10^x) * Kx
  // the approximation kx of 10^(-x) was rounded up to 192 bits
  __mul_256x256_to_512 (P512, C, bid_Kx256[ind]);
  // calculate C* = floor (P512) and f*
  // Cstar = P512 >> Ex
  // fstar = low Ex bits of P512
  shift = bid_Ex256m256[ind];	// in [0, 63] but have to consider four cases
  if (ind <= 18) {	// if 0 <= ind <= 18 
    Cstar.w[3] = (P512.w[7] >> shift);
    Cstar.w[2] = (P512.w[7] << (64 - shift)) | (P512.w[6] >> shift);
    Cstar.w[1] = (P512.w[6] << (64 - shift)) | (P512.w[5] >> shift);
    Cstar.w[0] = (P512.w[5] << (64 - shift)) | (P512.w[4] >> shift);
    fstar.w[7] = 0x0ULL;
    fstar.w[6] = 0x0ULL;
    fstar.w[5] = 0x0ULL;
    fstar.w[4] = P512.w[4] & bid_mask256[ind];
    fstar.w[3] = P512.w[3];
    fstar.w[2] = P512.w[2];
    fstar.w[1] = P512.w[1];
    fstar.w[0] = P512.w[0];
  } else if (ind <= 37) {	// if 19 <= ind <= 37
    Cstar.w[3] = 0x0ULL;
    Cstar.w[2] = P512.w[7] >> shift;
    Cstar.w[1] = (P512.w[7] << (64 - shift)) | (P512.w[6] >> shift);
    Cstar.w[0] = (P512.w[6] << (64 - shift)) | (P512.w[5] >> shift);
    fstar.w[7] = 0x0ULL;
    fstar.w[6] = 0x0ULL;
    fstar.w[5] = P512.w[5] & bid_mask256[ind];
    fstar.w[4] = P512.w[4];
    fstar.w[3] = P512.w[3];
    fstar.w[2] = P512.w[2];
    fstar.w[1] = P512.w[1];
    fstar.w[0] = P512.w[0];
  } else if (ind <= 56) {	// if 38 <= ind <= 56
    Cstar.w[3] = 0x0ULL;
    Cstar.w[2] = 0x0ULL;
    Cstar.w[1] = P512.w[7] >> shift;
    Cstar.w[0] = (P512.w[7] << (64 - shift)) | (P512.w[6] >> shift);
    fstar.w[7] = 0x0ULL;
    fstar.w[6] = P512.w[6] & bid_mask256[ind];
    fstar.w[5] = P512.w[5];
    fstar.w[4] = P512.w[4];
    fstar.w[3] = P512.w[3];
    fstar.w[2] = P512.w[2];
    fstar.w[1] = P512.w[1];
    fstar.w[0] = P512.w[0];
  } else if (ind == 57) {
    Cstar.w[3] = 0x0ULL;
    Cstar.w[2] = 0x0ULL;
    Cstar.w[1] = 0x0ULL;
    Cstar.w[0] = P512.w[7];
    fstar.w[7] = 0x0ULL;
    fstar.w[6] = P512.w[6];
    fstar.w[5] = P512.w[5];
    fstar.w[4] = P512.w[4];
    fstar.w[3] = P512.w[3];
    fstar.w[2] = P512.w[2];
    fstar.w[1] = P512.w[1];
    fstar.w[0] = P512.w[0];
  } else {	// if 58 <= ind <= 74
    Cstar.w[3] = 0x0ULL;
    Cstar.w[2] = 0x0ULL;
    Cstar.w[1] = 0x0ULL;
    Cstar.w[0] = P512.w[7] >> shift;
    fstar.w[7] = P512.w[7] & bid_mask256[ind];
    fstar.w[6] = P512.w[6];
    fstar.w[5] = P512.w[5];
    fstar.w[4] = P512.w[4];
    fstar.w[3] = P512.w[3];
    fstar.w[2] = P512.w[2];
    fstar.w[1] = P512.w[1];
    fstar.w[0] = P512.w[0];
  }

  // the top Ex bits of 10^(-x) are T* = bid_ten2mxtrunc256[ind], e.g. if x=1,
  // T*=bid_ten2mxtrunc256[0]=
  //     0xcccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc
  // if (0 < f* < 10^(-x)) then the result is a midpoint
  //   if floor(C*) is even then C* = floor(C*) - logical right
  //       shift; C* has q - x decimal digits, correct by Prop. 1)
  //   else if floor(C*) is odd C* = floor(C*)-1 (logical right
  //       shift; C* has q - x decimal digits, correct by Pr. 1)
  // else
  //   C* = floor(C*) (logical right shift; C has q - x decimal digits,
  //       correct by Property 1)
  // in the caling function n = C* * 10^(e+x)

  // determine inexactness of the rounding of C*
  // if (0 < f* - 1/2 < 10^(-x)) then
  //   the result is exact
  // else // if (f* - 1/2 > T*) then
  //   the result is inexact
  if (ind <= 18) {	// if 0 <= ind <= 18
    if (fstar.w[4] > bid_half256[ind] || (fstar.w[4] == bid_half256[ind] &&
				      (fstar.w[3] || fstar.w[2]
				       || fstar.w[1] || fstar.w[0]))) {
      // f* > 1/2 and the result may be exact
      // Calculate f* - 1/2
      tmp64 = fstar.w[4] - bid_half256[ind];
      if (tmp64 || fstar.w[3] > bid_ten2mxtrunc256[ind].w[2] || (fstar.w[3] == bid_ten2mxtrunc256[ind].w[3] && fstar.w[2] > bid_ten2mxtrunc256[ind].w[2]) || (fstar.w[3] == bid_ten2mxtrunc256[ind].w[3] && fstar.w[2] == bid_ten2mxtrunc256[ind].w[2] && fstar.w[1] > bid_ten2mxtrunc256[ind].w[1]) || (fstar.w[3] == bid_ten2mxtrunc256[ind].w[3] && fstar.w[2] == bid_ten2mxtrunc256[ind].w[2] && fstar.w[1] == bid_ten2mxtrunc256[ind].w[1] && fstar.w[0] > bid_ten2mxtrunc256[ind].w[0])) {	// f* - 1/2 > 10^(-x)
	*ptr_is_inexact_lt_midpoint = 1;
      }	// else the result is exact
    } else {	// the result is inexact; f2* <= 1/2
      *ptr_is_inexact_gt_midpoint = 1;
    }
  } else if (ind <= 37) {	// if 19 <= ind <= 37
    if (fstar.w[5] > bid_half256[ind] || (fstar.w[5] == bid_half256[ind] &&
				      (fstar.w[4] || fstar.w[3]
				       || fstar.w[2] || fstar.w[1]
				       || fstar.w[0]))) {
      // f* > 1/2 and the result may be exact
      // Calculate f* - 1/2
      tmp64 = fstar.w[5] - bid_half256[ind];
      if (tmp64 || fstar.w[4] || fstar.w[3] > bid_ten2mxtrunc256[ind].w[3] || (fstar.w[3] == bid_ten2mxtrunc256[ind].w[3] && fstar.w[2] > bid_ten2mxtrunc256[ind].w[2]) || (fstar.w[3] == bid_ten2mxtrunc256[ind].w[3] && fstar.w[2] == bid_ten2mxtrunc256[ind].w[2] && fstar.w[1] > bid_ten2mxtrunc256[ind].w[1]) || (fstar.w[3] == bid_ten2mxtrunc256[ind].w[3] && fstar.w[2] == bid_ten2mxtrunc256[ind].w[2] && fstar.w[1] == bid_ten2mxtrunc256[ind].w[1] && fstar.w[0] > bid_ten2mxtrunc256[ind].w[0])) {	// f* - 1/2 > 10^(-x)
	*ptr_is_inexact_lt_midpoint = 1;
      }	// else the result is exact
    } else {	// the result is inexact; f2* <= 1/2
      *ptr_is_inexact_gt_midpoint = 1;
    }
  } else if (ind <= 57) {	// if 38 <= ind <= 57
    if (fstar.w[6] > bid_half256[ind] || (fstar.w[6] == bid_half256[ind] &&
				      (fstar.w[5] || fstar.w[4]
				       || fstar.w[3] || fstar.w[2]
				       || fstar.w[1] || fstar.w[0]))) {
      // f* > 1/2 and the result may be exact
      // Calculate f* - 1/2
      tmp64 = fstar.w[6] - bid_half256[ind];
      if (tmp64 || fstar.w[5] || fstar.w[4] || fstar.w[3] > bid_ten2mxtrunc256[ind].w[3] || (fstar.w[3] == bid_ten2mxtrunc256[ind].w[3] && fstar.w[2] > bid_ten2mxtrunc256[ind].w[2]) || (fstar.w[3] == bid_ten2mxtrunc256[ind].w[3] && fstar.w[2] == bid_ten2mxtrunc256[ind].w[2] && fstar.w[1] > bid_ten2mxtrunc256[ind].w[1]) || (fstar.w[3] == bid_ten2mxtrunc256[ind].w[3] && fstar.w[2] == bid_ten2mxtrunc256[ind].w[2] && fstar.w[1] == bid_ten2mxtrunc256[ind].w[1] && fstar.w[0] > bid_ten2mxtrunc256[ind].w[0])) {	// f* - 1/2 > 10^(-x)
	*ptr_is_inexact_lt_midpoint = 1;
      }	// else the result is exact
    } else {	// the result is inexact; f2* <= 1/2
      *ptr_is_inexact_gt_midpoint = 1;
    }
  } else {	// if 58 <= ind <= 74
    if (fstar.w[7] > bid_half256[ind] || (fstar.w[7] == bid_half256[ind] &&
				      (fstar.w[6] || fstar.w[5]
				       || fstar.w[4] || fstar.w[3]
				       || fstar.w[2] || fstar.w[1]
				       || fstar.w[0]))) {
      // f* > 1/2 and the result may be exact
      // Calculate f* - 1/2
      tmp64 = fstar.w[7] - bid_half256[ind];
      if (tmp64 || fstar.w[6] || fstar.w[5] || fstar.w[4] || fstar.w[3] > bid_ten2mxtrunc256[ind].w[3] || (fstar.w[3] == bid_ten2mxtrunc256[ind].w[3] && fstar.w[2] > bid_ten2mxtrunc256[ind].w[2]) || (fstar.w[3] == bid_ten2mxtrunc256[ind].w[3] && fstar.w[2] == bid_ten2mxtrunc256[ind].w[2] && fstar.w[1] > bid_ten2mxtrunc256[ind].w[1]) || (fstar.w[3] == bid_ten2mxtrunc256[ind].w[3] && fstar.w[2] == bid_ten2mxtrunc256[ind].w[2] && fstar.w[1] == bid_ten2mxtrunc256[ind].w[1] && fstar.w[0] > bid_ten2mxtrunc256[ind].w[0])) {	// f* - 1/2 > 10^(-x)
	*ptr_is_inexact_lt_midpoint = 1;
      }	// else the result is exact
    } else {	// the result is inexact; f2* <= 1/2
      *ptr_is_inexact_gt_midpoint = 1;
    }
  }
  // check for midpoints (could do this before determining inexactness)
  if (fstar.w[7] == 0 && fstar.w[6] == 0 &&
      fstar.w[5] == 0 && fstar.w[4] == 0 &&
      (fstar.w[3] < bid_ten2mxtrunc256[ind].w[3] ||
       (fstar.w[3] == bid_ten2mxtrunc256[ind].w[3] &&
	fstar.w[2] < bid_ten2mxtrunc256[ind].w[2]) ||
       (fstar.w[3] == bid_ten2mxtrunc256[ind].w[3] &&
	fstar.w[2] == bid_ten2mxtrunc256[ind].w[2] &&
	fstar.w[1] < bid_ten2mxtrunc256[ind].w[1]) ||
       (fstar.w[3] == bid_ten2mxtrunc256[ind].w[3] &&
	fstar.w[2] == bid_ten2mxtrunc256[ind].w[2] &&
	fstar.w[1] == bid_ten2mxtrunc256[ind].w[1] &&
	fstar.w[0] <= bid_ten2mxtrunc256[ind].w[0]))) {
    // the result is a midpoint
    if (Cstar.w[0] & 0x01) {	// Cstar is odd; MP in [EVEN, ODD]
      // if floor(C*) is odd C = floor(C*) - 1; the result may be 0
      Cstar.w[0]--;	// Cstar is now even
      if (Cstar.w[0] == 0xffffffffffffffffULL) {
	Cstar.w[1]--;
	if (Cstar.w[1] == 0xffffffffffffffffULL) {
	  Cstar.w[2]--;
	  if (Cstar.w[2] == 0xffffffffffffffffULL) {
	    Cstar.w[3]--;
	  }
	}
      }
      *ptr_is_midpoint_gt_even = 1;
      *ptr_is_inexact_lt_midpoint = 0;
      *ptr_is_inexact_gt_midpoint = 0;
    } else {	// else MP in [ODD, EVEN]
      *ptr_is_midpoint_lt_even = 1;
      *ptr_is_inexact_lt_midpoint = 0;
      *ptr_is_inexact_gt_midpoint = 0;
    }
  }
  // check for rounding overflow, which occurs if Cstar = 10^(q-x)
  ind = q - x;	// 1 <= ind <= q - 1
  if (ind <= 19) {
    if (Cstar.w[3] == 0x0ULL && Cstar.w[2] == 0x0ULL &&
	Cstar.w[1] == 0x0ULL && Cstar.w[0] == bid_ten2k64[ind]) {
      // if  Cstar = 10^(q-x)
      Cstar.w[0] = bid_ten2k64[ind - 1];	// Cstar = 10^(q-x-1)
      *incr_exp = 1;
    } else {
      *incr_exp = 0;
    }
  } else if (ind == 20) {
    // if ind = 20
    if (Cstar.w[3] == 0x0ULL && Cstar.w[2] == 0x0ULL &&
	Cstar.w[1] == bid_ten2k128[0].w[1]
	&& Cstar.w[0] == bid_ten2k128[0].w[0]) {
      // if  Cstar = 10^(q-x)
      Cstar.w[0] = bid_ten2k64[19];	// Cstar = 10^(q-x-1)
      Cstar.w[1] = 0x0ULL;
      *incr_exp = 1;
    } else {
      *incr_exp = 0;
    }
  } else if (ind <= 38) {	// if 21 <= ind <= 38
    if (Cstar.w[3] == 0x0ULL && Cstar.w[2] == 0x0ULL &&
	Cstar.w[1] == bid_ten2k128[ind - 20].w[1] &&
	Cstar.w[0] == bid_ten2k128[ind - 20].w[0]) {
      // if  Cstar = 10^(q-x)
      Cstar.w[0] = bid_ten2k128[ind - 21].w[0];	// Cstar = 10^(q-x-1)
      Cstar.w[1] = bid_ten2k128[ind - 21].w[1];
      *incr_exp = 1;
    } else {
      *incr_exp = 0;
    }
  } else if (ind == 39) {
    if (Cstar.w[3] == 0x0ULL && Cstar.w[2] == bid_ten2k256[0].w[2] &&
	Cstar.w[1] == bid_ten2k256[0].w[1]
	&& Cstar.w[0] == bid_ten2k256[0].w[0]) {
      // if  Cstar = 10^(q-x)
      Cstar.w[0] = bid_ten2k128[18].w[0];	// Cstar = 10^(q-x-1)
      Cstar.w[1] = bid_ten2k128[18].w[1];
      Cstar.w[2] = 0x0ULL;
      *incr_exp = 1;
    } else {
      *incr_exp = 0;
    }
  } else if (ind <= 57) {	// if 40 <= ind <= 57
    if (Cstar.w[3] == 0x0ULL && Cstar.w[2] == bid_ten2k256[ind - 39].w[2] &&
	Cstar.w[1] == bid_ten2k256[ind - 39].w[1] &&
	Cstar.w[0] == bid_ten2k256[ind - 39].w[0]) {
      // if  Cstar = 10^(q-x)
      Cstar.w[0] = bid_ten2k256[ind - 40].w[0];	// Cstar = 10^(q-x-1)
      Cstar.w[1] = bid_ten2k256[ind - 40].w[1];
      Cstar.w[2] = bid_ten2k256[ind - 40].w[2];
      *incr_exp = 1;
    } else {
      *incr_exp = 0;
    }
    // else if (ind == 58) is not needed becauae we do not have ten2k192[] yet
  } else {	// if 58 <= ind <= 77 (actually 58 <= ind <= 74)
    if (Cstar.w[3] == bid_ten2k256[ind - 39].w[3] &&
	Cstar.w[2] == bid_ten2k256[ind - 39].w[2] &&
	Cstar.w[1] == bid_ten2k256[ind - 39].w[1] &&
	Cstar.w[0] == bid_ten2k256[ind - 39].w[0]) {
      // if  Cstar = 10^(q-x)
      Cstar.w[0] = bid_ten2k256[ind - 40].w[0];	// Cstar = 10^(q-x-1)
      Cstar.w[1] = bid_ten2k256[ind - 40].w[1];
      Cstar.w[2] = bid_ten2k256[ind - 40].w[2];
      Cstar.w[3] = bid_ten2k256[ind - 40].w[3];
      *incr_exp = 1;
    } else {
      *incr_exp = 0;
    }
  }
  ptr_Cstar->w[3] = Cstar.w[3];
  ptr_Cstar->w[2] = Cstar.w[2];
  ptr_Cstar->w[1] = Cstar.w[1];
  ptr_Cstar->w[0] = Cstar.w[0];

}
