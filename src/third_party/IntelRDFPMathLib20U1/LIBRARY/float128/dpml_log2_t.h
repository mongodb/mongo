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

#include "endian.h"





    static const TABLE_UNION __log2_t_table[] = { 

	/* 1.0 in working precision */
	/* 000 */ DATA_1x2( 0x00000000, 0x3ff00000 ),

	/* poly coeffs, near 1 */
	/* 008 */ DATA_1x2( 0x652b82fe, 0xbfe71547 ),
	/* 016 */ DATA_1x2( 0xdc3a05d3, 0x3fdec709 ),
	/* 024 */ DATA_1x2( 0x652b871a, 0xbfd71547 ),
	/* 032 */ DATA_1x2( 0x50e135f1, 0x3fd2776c ),
	/* 040 */ DATA_1x2( 0xdc0d1e80, 0xbfcec709 ),
	/* 048 */ DATA_1x2( 0x7d1eeff0, 0x3fca6176 ),
	/* 056 */ DATA_1x2( 0xcdda1141, 0xbfc71547 ),
	/* 064 */ DATA_1x2( 0x1c4cb82d, 0x3fc48446 ),
	/* 072 */ DATA_1x2( 0x0542bb33, 0xbfc276f6 ),
	/* 080 */ DATA_1x2( 0x60821257, 0x3fc10990 ),
	/* 088 */ DATA_1x2( 0xea869e4b, 0xbfbf46f0 ),

	/* poly coeffs, quotient, near 1 */
	/* 096 */ DATA_1x2( 0xdc3a05d3, 0x3fbec709 ),
	/* 104 */ DATA_1x2( 0x50e13578, 0x3f92776c ),
	/* 112 */ DATA_1x2( 0x7d20b59a, 0x3f6a6176 ),
	/* 120 */ DATA_1x2( 0x1aefa4b8, 0x3f448446 ),
	/* 128 */ DATA_1x2( 0xbe846c21, 0x3f210990 ),

	/* poly coeffs, away from 1 */
	/* 136 */ DATA_1x2( 0x652b82ff, 0xbfe71547 ),
	/* 144 */ DATA_1x2( 0xdc32988b, 0x3fdec709 ),
	/* 152 */ DATA_1x2( 0x6521ed89, 0xbfd71547 ),
	/* 160 */ DATA_1x2( 0x1a1a29cb, 0x3fd27780 ),
	/* 168 */ DATA_1x2( 0x6e93af0c, 0xbfcec731 ),

	/* poly coeffs, quotient, away from 1 */
	/* 176 */ DATA_1x2( 0xdc3a04a2, 0x3fbec709 ),
	/* 184 */ DATA_1x2( 0x5022681e, 0x3f92776c ),
	/* 192 */ DATA_1x2( 0x52fb6584, 0x3f6a621a ),


	/* log of 2 in hi and lo parts */
	/* 200 */ DATA_1x2( 0x00000000, 0x3ff00000 ),
	/* 208 */ DATA_1x2( 0x00000000, 0x00000000 ),

	/* log of e, in hi and lo parts */
	/* 216 */ DATA_1x2( 0x652b82fe, 0x3ff71547 ),
	/* 224 */ DATA_1x2( 0x68000000, 0x3ff71547 ),
	/* 232 */ DATA_1x2( 0x00000000, 0x3c778000 ),
	/* 240 */ DATA_1x2( 0x0f440000, 0xbe46a3e8 ),


	/* Table of F, 1/F, and hi and lo log of F */
	/* 248 */	 DATA_4x2( 0x00000000, 0x3ff01000, 0xe01fe020, 0x3fefe01f, 0x6d7c0000, 0x3f7709c4, 0x52642db7, 0xbd35388b ),	/* row 0 */ 
	/* 280 */	 DATA_4x2( 0x00000000, 0x3ff03000, 0xaa01fa12, 0x3fefa11c, 0x17a90000, 0x3f913631, 0xed069b24, 0x3d3ec312 ),	/* row 1 */ 
	/* 312 */	 DATA_4x2( 0x00000000, 0x3ff05000, 0xaca0dbb5, 0x3fef6310, 0xba850000, 0x3f9c9363, 0xdd01ee2f, 0x3d0f0ccc ),	/* row 2 */ 
	/* 344 */	 DATA_4x2( 0x00000000, 0x3ff07000, 0x44230ab5, 0x3fef25f6, 0x94688000, 0x3fa3ed30, 0xec5a47e0, 0xbd32ecef ),	/* row 3 */ 
	/* 376 */	 DATA_4x2( 0x00000000, 0x3ff09000, 0xf8458e02, 0x3feee9c7, 0xc3498000, 0x3fa985bf, 0xaf8e2578, 0xbd3735cf ),	/* row 4 */ 
	/* 408 */	 DATA_4x2( 0x00000000, 0x3ff0b000, 0x7aba01eb, 0x3feeae80, 0x83328000, 0x3faf1389, 0x197ca224, 0xbd36302f ),	/* row 5 */ 
	/* 440 */	 DATA_4x2( 0x00000000, 0x3ff0d000, 0xa59750e4, 0x3fee741a, 0x7e134000, 0x3fb24b5b, 0xa2cf3516, 0x3d3a3c89 ),	/* row 6 */ 
	/* 472 */	 DATA_4x2( 0x00000000, 0x3ff0f000, 0x79dc1a73, 0x3fee3a91, 0x36034000, 0x3fb507b8, 0x34b21259, 0xbd1124ac ),	/* row 7 */ 
	/* 504 */	 DATA_4x2( 0x00000000, 0x3ff11000, 0x1e01e01e, 0x3fee01e0, 0x96b8c000, 0x3fb7beee, 0xbe51cdcb, 0xbd3d7ec3 ),	/* row 8 */ 
	/* 536 */	 DATA_4x2( 0x00000000, 0x3ff13000, 0xdca01dca, 0x3fedca01, 0xdf348000, 0x3fba7111, 0x116078ef, 0x3d124fad ),	/* row 9 */ 
	/* 568 */	 DATA_4x2( 0x00000000, 0x3ff15000, 0x231e7f8a, 0x3fed92f2, 0xe35b8000, 0x3fbd1e34, 0x59c7991e, 0x3d06d268 ),	/* row 10 */ 
	/* 600 */	 DATA_4x2( 0x00000000, 0x3ff17000, 0x807572b2, 0x3fed5cac, 0x0f0b0000, 0x3fbfc66a, 0xa68c72a1, 0x3ce49209 ),	/* row 11 */ 
	/* 632 */	 DATA_4x2( 0x00000000, 0x3ff19000, 0xa3fc5b1a, 0x3fed272c, 0xb4890000, 0x3fc134e1, 0xd690403e, 0x3d28b7fc ),	/* row 12 */ 
	/* 664 */	 DATA_4x2( 0x00000000, 0x3ff1b000, 0x5c44bfc6, 0x3fecf26e, 0x4b07a000, 0x3fc28429, 0xda2ab291, 0x3d28fe35 ),	/* row 13 */ 
	/* 696 */	 DATA_4x2( 0x00000000, 0x3ff1d000, 0x9601cbe7, 0x3fecbe6d, 0x6d9a8000, 0x3fc3d114, 0x166e1f56, 0x3d34c7e0 ),	/* row 14 */ 
	/* 728 */	 DATA_4x2( 0x00000000, 0x3ff1f000, 0x5afb8a42, 0x3fec8b26, 0x907a6000, 0x3fc51bab, 0x7fbb3d20, 0xbd1badba ),	/* row 15 */ 
	/* 760 */	 DATA_4x2( 0x00000000, 0x3ff21000, 0xd10d4986, 0x3fec5894, 0xfac92000, 0x3fc663f6, 0x6758fb3d, 0xbd39d306 ),	/* row 16 */ 
	/* 792 */	 DATA_4x2( 0x00000000, 0x3ff23000, 0x392ea01c, 0x3fec26b5, 0xc7d06000, 0x3fc7a9fe, 0x0d92f890, 0xbd110874 ),	/* row 17 */ 
	/* 824 */	 DATA_4x2( 0x00000000, 0x3ff25000, 0xee868d8b, 0x3febf583, 0xe8352000, 0x3fc8edca, 0x9a843329, 0x3d36d76b ),	/* row 18 */ 
	/* 856 */	 DATA_4x2( 0x00000000, 0x3ff27000, 0x65883e7b, 0x3febc4fd, 0x2320c000, 0x3fca2f63, 0x1deb636a, 0xbd2e54d7 ),	/* row 19 */ 
	/* 888 */	 DATA_4x2( 0x00000000, 0x3ff29000, 0x2b18ff23, 0x3feb951e, 0x175fa000, 0x3fcb6ecf, 0x24e663a1, 0xbd342d28 ),	/* row 20 */ 
	/* 920 */	 DATA_4x2( 0x00000000, 0x3ff2b000, 0xe3beee05, 0x3feb65e2, 0x3c770000, 0x3fccac16, 0x8994b162, 0x3d3b912d ),	/* row 21 */ 
	/* 952 */	 DATA_4x2( 0x00000000, 0x3ff2d000, 0x4ad806ce, 0x3feb3748, 0xe3b14000, 0x3fcde73f, 0x7c84e79a, 0x3d301dc3 ),	/* row 22 */ 
	/* 984 */	 DATA_4x2( 0x00000000, 0x3ff2f000, 0x31d922a4, 0x3feb094b, 0x39208000, 0x3fcf2053, 0xea54ce63, 0x3d3e4e8e ),	/* row 23 */ 
	/* 1016 */	 DATA_4x2( 0x00000000, 0x3ff31000, 0x7f94905e, 0x3feadbe8, 0xa24d0000, 0x3fd02bab, 0x5e85b29f, 0x3d398eec ),	/* row 24 */ 
	/* 1048 */	 DATA_4x2( 0x00000000, 0x3ff33000, 0x2f87ebfd, 0x3feaaf1d, 0x75543000, 0x3fd0c629, 0x13816f9f, 0xbd35c56c ),	/* row 25 */ 
	/* 1080 */	 DATA_4x2( 0x00000000, 0x3ff35000, 0x5130e159, 0x3fea82e6, 0x76bb1000, 0x3fd15fa6, 0x071eeb10, 0xbd3c029a ),	/* row 26 */ 
	/* 1112 */	 DATA_4x2( 0x00000000, 0x3ff37000, 0x07688a4a, 0x3fea5741, 0xf6d89000, 0x3fd1f825, 0x7972c083, 0xbd1ecd41 ),	/* row 27 */ 
	/* 1144 */	 DATA_4x2( 0x00000000, 0x3ff39000, 0x87c51ca0, 0x3fea2c2a, 0x35b32000, 0x3fd28fab, 0x0e85a909, 0x3d3a0d8c ),	/* row 28 */ 
	/* 1176 */	 DATA_4x2( 0x00000000, 0x3ff3b000, 0x1a01a01a, 0x3fea01a0, 0x636b3000, 0x3fd32639, 0x7070fc4b, 0xbd3f2994 ),	/* row 29 */ 
	/* 1208 */	 DATA_4x2( 0x00000000, 0x3ff3d000, 0x176b682d, 0x3fe9d79f, 0xa0a1e000, 0x3fd3bbd3, 0xe791792d, 0xbd282b53 ),	/* row 30 */ 
	/* 1240 */	 DATA_4x2( 0x00000000, 0x3ff3f000, 0xea5510da, 0x3fe9ae24, 0xfedd5000, 0x3fd4507c, 0x2fb0019d, 0xbcee3595 ),	/* row 31 */ 
	/* 1272 */	 DATA_4x2( 0x00000000, 0x3ff41000, 0x0d8ec0ff, 0x3fe9852f, 0x80e90000, 0x3fd4e438, 0x0a38f4e9, 0xbd325811 ),	/* row 32 */ 
	/* 1304 */	 DATA_4x2( 0x00000000, 0x3ff43000, 0x0be377ae, 0x3fe95cbb, 0x1b338000, 0x3fd57709, 0xe1f94c50, 0xbd3cd53b ),	/* row 33 */ 
	/* 1336 */	 DATA_4x2( 0x00000000, 0x3ff45000, 0x7f9b2ce6, 0x3fe934c6, 0xb4295000, 0x3fd608f1, 0x3fc62b7e, 0xbd3d49a4 ),	/* row 34 */ 
	/* 1368 */	 DATA_4x2( 0x00000000, 0x3ff47000, 0x120190d5, 0x3fe90d4f, 0x248cd000, 0x3fd699f5, 0x152150d3, 0x3d32e1a3 ),	/* row 35 */ 
	/* 1400 */	 DATA_4x2( 0x00000000, 0x3ff49000, 0x7af1373f, 0x3fe8e652, 0x37cbc000, 0x3fd72a16, 0x4d994a2a, 0x3d182943 ),	/* row 36 */ 
	/* 1432 */	 DATA_4x2( 0x00000000, 0x3ff4b000, 0x8062ff3a, 0x3fe8bfce, 0xac51b000, 0x3fd7b957, 0x7240b049, 0xbd34eea2 ),	/* row 37 */ 
	/* 1464 */	 DATA_4x2( 0x00000000, 0x3ff4d000, 0xf601899c, 0x3fe899c0, 0x33d86000, 0x3fd847bc, 0x094eee51, 0x3d18dc7c ),	/* row 38 */ 
	/* 1496 */	 DATA_4x2( 0x00000000, 0x3ff4f000, 0xbcc092b9, 0x3fe87427, 0x73b5c000, 0x3fd8d546, 0xe8492d6e, 0x3d2b8d59 ),	/* row 39 */ 
	/* 1528 */	 DATA_4x2( 0x00000000, 0x3ff51000, 0xc2780614, 0x3fe84f00, 0x05274000, 0x3fd961f9, 0xb3af5b8d, 0x3d03719e ),	/* row 40 */ 
	/* 1560 */	 DATA_4x2( 0x00000000, 0x3ff53000, 0x0182a4a0, 0x3fe82a4a, 0x759b2000, 0x3fd9edd6, 0x6c73e71b, 0x3d377e23 ),	/* row 41 */ 
	/* 1592 */	 DATA_4x2( 0x00000000, 0x3ff55000, 0x80601806, 0x3fe80601, 0x46f7c000, 0x3fda78e1, 0xdfa568f7, 0xbd10bad7 ),	/* row 42 */ 
	/* 1624 */	 DATA_4x2( 0x00000000, 0x3ff57000, 0x515a4f1d, 0x3fe7e225, 0xefe06000, 0x3fdb031b, 0x805b0aec, 0x3d30d199 ),	/* row 43 */ 
	/* 1656 */	 DATA_4x2( 0x00000000, 0x3ff59000, 0x922e017c, 0x3fe7beb3, 0xdbf88000, 0x3fdb8c88, 0xd77582e2, 0x3d39e65c ),	/* row 44 */ 
	/* 1688 */	 DATA_4x2( 0x00000000, 0x3ff5b000, 0x6bb6398b, 0x3fe79baa, 0x6c24d000, 0x3fdc152a, 0x68d6d2d3, 0xbd3468ff ),	/* row 45 */ 
	/* 1720 */	 DATA_4x2( 0x00000000, 0x3ff5d000, 0x119ac60d, 0x3fe77908, 0xf6ca4000, 0x3fdc9d02, 0xff1e8ea2, 0x3d3ecf4d ),	/* row 46 */ 
	/* 1752 */	 DATA_4x2( 0x00000000, 0x3ff5f000, 0xc201756d, 0x3fe756ca, 0xc80bf000, 0x3fdd2414, 0xadf6a54a, 0x3d23ea90 ),	/* row 47 */ 
	/* 1784 */	 DATA_4x2( 0x00000000, 0x3ff61000, 0xc541fe8d, 0x3fe734f0, 0x22065000, 0x3fddaa62, 0x2d6a3aa8, 0xbcf1bfb6 ),	/* row 48 */ 
	/* 1816 */	 DATA_4x2( 0x00000000, 0x3ff63000, 0x6d9c7c09, 0x3fe71378, 0x3d097000, 0x3fde2fed, 0x912ab9d1, 0x3d24c06f ),	/* row 49 */ 
	/* 1848 */	 DATA_4x2( 0x00000000, 0x3ff65000, 0x16f26017, 0x3fe6f260, 0x47d16000, 0x3fdeb4b8, 0x68d867f1, 0xbd30c6b0 ),	/* row 50 */ 
	/* 1880 */	 DATA_4x2( 0x00000000, 0x3ff67000, 0x2681c861, 0x3fe6d1a6, 0x67bcc000, 0x3fdf38c5, 0xf8df4b43, 0x3d350343 ),	/* row 51 */ 
	/* 1912 */	 DATA_4x2( 0x00000000, 0x3ff69000, 0x0aa31a3d, 0x3fe6b149, 0xb9027000, 0x3fdfbc16, 0x5c999d62, 0xbd3fd771 ),	/* row 52 */ 
	/* 1944 */	 DATA_4x2( 0x00000000, 0x3ff6b000, 0x3a88d0c0, 0x3fe69147, 0x27726800, 0x3fe01f57, 0x6a042173, 0xbd39006e ),	/* row 53 */ 
	/* 1976 */	 DATA_4x2( 0x00000000, 0x3ff6d000, 0x3601671a, 0x3fe6719f, 0x19f25000, 0x3fe06047, 0x6e0e0c68, 0xbd24dc16 ),	/* row 54 */ 
	/* 2008 */	 DATA_4x2( 0x00000000, 0x3ff6f000, 0x853b4aa3, 0x3fe6524f, 0x34f8e000, 0x3fe0a0dc, 0xd8d6cbcf, 0x3d2fbc00 ),	/* row 55 */ 
	/* 2040 */	 DATA_4x2( 0x00000000, 0x3ff71000, 0xb88ac0de, 0x3fe63356, 0x754d8000, 0x3fe0e117, 0x8aa1aed8, 0xbd3f7762 ),	/* row 56 */ 
	/* 2072 */	 DATA_4x2( 0x00000000, 0x3ff73000, 0x6831ae94, 0x3fe614b3, 0xd39e1800, 0x3fe120f9, 0x4581174d, 0x3cccb52b ),	/* row 57 */ 
	/* 2104 */	 DATA_4x2( 0x00000000, 0x3ff75000, 0x34292dfc, 0x3fe5f664, 0x4495e000, 0x3fe16084, 0x378ff59d, 0x3cc81e2b ),	/* row 58 */ 
	/* 2136 */	 DATA_4x2( 0x00000000, 0x3ff77000, 0xc3ece2a5, 0x3fe5d867, 0xb8f32800, 0x3fe19fb7, 0xf1e559fe, 0xbd3ef474 ),	/* row 59 */ 
	/* 2168 */	 DATA_4x2( 0x00000000, 0x3ff79000, 0xc647fa91, 0x3fe5babc, 0x1d9cb800, 0x3fe1de95, 0xd2643639, 0x3d3d30c3 ),	/* row 60 */ 
	/* 2200 */	 DATA_4x2( 0x00000000, 0x3ff7b000, 0xf123ccaa, 0x3fe59d61, 0x5bb6d800, 0x3fe21d1d, 0x785e97ab, 0xbd332e75 ),	/* row 61 */ 
	/* 2232 */	 DATA_4x2( 0x00000000, 0x3ff7d000, 0x01580560, 0x3fe58056, 0x58b74000, 0x3fe25b51, 0xcb97f73c, 0xbd37dd4b ),	/* row 62 */ 
	/* 2264 */	 DATA_4x2( 0x00000000, 0x3ff7f000, 0xba7c52e2, 0x3fe56397, 0xf6791800, 0x3fe29931, 0x61311744, 0xbd34fd70 ),	/* row 63 */ 
	/* 2296 */	 DATA_4x2( 0x00000000, 0x3ff81000, 0xe6bb82fe, 0x3fe54725, 0x13501000, 0x3fe2d6c0, 0x4a7145e3, 0x3d3c0325 ),	/* row 64 */ 
	/* 2328 */	 DATA_4x2( 0x00000000, 0x3ff83000, 0x56a8054b, 0x3fe52aff, 0x8a1b3800, 0x3fe313fc, 0xdc5befec, 0xbd20dc60 ),	/* row 65 */ 
	/* 2360 */	 DATA_4x2( 0x00000000, 0x3ff85000, 0xe111c4c5, 0x3fe50f22, 0x32570800, 0x3fe350e8, 0x9b3611ce, 0xbcf38bc9 ),	/* row 66 */ 
	/* 2392 */	 DATA_4x2( 0x00000000, 0x3ff87000, 0x62dd4c9b, 0x3fe4f38f, 0xe02f6000, 0x3fe38d83, 0x20f1e8c4, 0xbd37b6bf ),	/* row 67 */ 
	/* 2424 */	 DATA_4x2( 0x00000000, 0x3ff89000, 0xbedc2c4c, 0x3fe4d843, 0x6490b000, 0x3fe3c9d0, 0x3ea4d6f1, 0xbd2eddd3 ),	/* row 68 */ 
	/* 2456 */	 DATA_4x2( 0x00000000, 0x3ff8b000, 0xdda68fe1, 0x3fe4bd3e, 0x8d38f800, 0x3fe405ce, 0x968271ab, 0xbd3a20a0 ),	/* row 69 */ 
	/* 2488 */	 DATA_4x2( 0x00000000, 0x3ff8d000, 0xad76014a, 0x3fe4a27f, 0x24c82000, 0x3fe4417f, 0x1adca9a8, 0x3d264adb ),	/* row 70 */ 
	/* 2520 */	 DATA_4x2( 0x00000000, 0x3ff8f000, 0x22014880, 0x3fe48805, 0xf2d02800, 0x3fe47ce2, 0xe378b903, 0xbd33c8e5 ),	/* row 71 */ 
	/* 2552 */	 DATA_4x2( 0x00000000, 0x3ff91000, 0x34596066, 0x3fe46dce, 0xbbe49800, 0x3fe4b7fa, 0xe6bca777, 0xbd0aa0e9 ),	/* row 72 */ 
	/* 2584 */	 DATA_4x2( 0x00000000, 0x3ff93000, 0xe2c776ca, 0x3fe453d9, 0x41a9f000, 0x3fe4f2c7, 0x83f85c08, 0x3d39f3ba ),	/* row 73 */ 
	/* 2616 */	 DATA_4x2( 0x00000000, 0x3ff95000, 0x30abee4d, 0x3fe43a27, 0x42e47800, 0x3fe52d49, 0x49b8484b, 0x3d2094ef ),	/* row 74 */ 
	/* 2648 */	 DATA_4x2( 0x00000000, 0x3ff97000, 0x265e5951, 0x3fe420b5, 0x7b86b000, 0x3fe56781, 0xd35fdc18, 0x3cf63d8b ),	/* row 75 */ 
	/* 2680 */	 DATA_4x2( 0x00000000, 0x3ff99000, 0xd10e6566, 0x3fe40782, 0xa4bf9000, 0x3fe5a170, 0xcbe86c17, 0xbd351ea1 ),	/* row 76 */ 
	/* 2712 */	 DATA_4x2( 0x00000000, 0x3ff9b000, 0x42a5af07, 0x3fe3ee8f, 0x75084000, 0x3fe5db17, 0x247ab6af, 0x3d23c49c ),	/* row 77 */ 
	/* 2744 */	 DATA_4x2( 0x00000000, 0x3ff9d000, 0x91aa75c6, 0x3fe3d5d9, 0xa031b000, 0x3fe61476, 0xee46ebe4, 0x3d20899c ),	/* row 78 */ 
	/* 2776 */	 DATA_4x2( 0x00000000, 0x3ff9f000, 0xd9232955, 0x3fe3bd60, 0xd7719800, 0x3fe64d8e, 0xdf8803d1, 0x3d3f7cc3 ),	/* row 79 */ 
	/* 2808 */	 DATA_4x2( 0x00000000, 0x3ffa1000, 0x387ac822, 0x3fe3a524, 0xc96f7000, 0x3fe68660, 0x11f8a0ce, 0xbd0e321d ),	/* row 80 */ 
	/* 2840 */	 DATA_4x2( 0x00000000, 0x3ffa3000, 0xd366088e, 0x3fe38d22, 0x2250d000, 0x3fe6beed, 0xaac2fde9, 0xbd328fa3 ),	/* row 81 */ 
	/* 2872 */	 DATA_4x2( 0x00000000, 0x3ffa5000, 0xd1c945ee, 0x3fe3755b, 0x8bc5c800, 0x3fe6f734, 0x80f0cdc7, 0xbd2e8597 ),	/* row 82 */ 
	/* 2904 */	 DATA_4x2( 0x00000000, 0x3ffa7000, 0x5f9f2af8, 0x3fe35dce, 0xad14c800, 0x3fe72f37, 0x174c8d06, 0xbd3281a3 ),	/* row 83 */ 
	/* 2936 */	 DATA_4x2( 0x00000000, 0x3ffa9000, 0xace01346, 0x3fe34679, 0x2b264000, 0x3fe766f7, 0xea4cc5a4, 0xbd309190 ),	/* row 84 */ 
	/* 2968 */	 DATA_4x2( 0x00000000, 0x3ffab000, 0xed6a1dfa, 0x3fe32f5c, 0xa8900800, 0x3fe79e73, 0x36c24b74, 0xbd2e03b3 ),	/* row 85 */ 
	/* 3000 */	 DATA_4x2( 0x00000000, 0x3ffad000, 0x58e9ebb6, 0x3fe31877, 0xc5a07800, 0x3fe7d5ad, 0x4776534a, 0x3d148808 ),	/* row 86 */ 
	/* 3032 */	 DATA_4x2( 0x00000000, 0x3ffaf000, 0x2ac40260, 0x3fe301c8, 0x20695000, 0x3fe80ca6, 0x72eda08e, 0xbd3039b7 ),	/* row 87 */ 
	/* 3064 */	 DATA_4x2( 0x00000000, 0x3ffb1000, 0xa1fed14b, 0x3fe2eb4e, 0x54ca3800, 0x3fe8435d, 0x313e79cf, 0xbd118906 ),	/* row 88 */ 
	/* 3096 */	 DATA_4x2( 0x00000000, 0x3ffb3000, 0x012d50a0, 0x3fe2d50a, 0xfc7b3800, 0x3fe879d3, 0x204507b9, 0x3d3b85f3 ),	/* row 89 */ 
	/* 3128 */	 DATA_4x2( 0x00000000, 0x3ffb5000, 0x8e5a3711, 0x3fe2bef9, 0xaf16d800, 0x3fe8b00a, 0x9b9239d6, 0xbd3ab8f4 ),	/* row 90 */ 
	/* 3160 */	 DATA_4x2( 0x00000000, 0x3ffb7000, 0x92f3c105, 0x3fe2a91c, 0x0223d800, 0x3fe8e602, 0x53fcfec0, 0xbd29f781 ),	/* row 91 */ 
	/* 3192 */	 DATA_4x2( 0x00000000, 0x3ffb9000, 0x5bb804a5, 0x3fe29372, 0x891f1800, 0x3fe91bba, 0xa95f528f, 0xbd1ee969 ),	/* row 92 */ 
	/* 3224 */	 DATA_4x2( 0x00000000, 0x3ffbb000, 0x38a1ce4d, 0x3fe27dfa, 0xd584e000, 0x3fe95134, 0x63cddadf, 0x3d371b3a ),	/* row 93 */ 
	/* 3256 */	 DATA_4x2( 0x00000000, 0x3ffbd000, 0x7cd60127, 0x3fe268b3, 0x76da3800, 0x3fe98671, 0xb2827cf0, 0x3cf742a6 ),	/* row 94 */ 
	/* 3288 */	 DATA_4x2( 0x00000000, 0x3ffbf000, 0x7e9177b2, 0x3fe2539d, 0xfab5d000, 0x3fe9bb70, 0x36337985, 0xbd2b37bd ),	/* row 95 */ 
	/* 3320 */	 DATA_4x2( 0x00000000, 0x3ffc1000, 0x9717605b, 0x3fe23eb7, 0xecc8e800, 0x3fe9f033, 0xcd0e0880, 0x3d25651c ),	/* row 96 */ 
	/* 3352 */	 DATA_4x2( 0x00000000, 0x3ffc3000, 0x22a0122a, 0x3fe22a01, 0xd6e7f800, 0x3fea24ba, 0x30409355, 0x3d3bb557 ),	/* row 97 */ 
	/* 3384 */	 DATA_4x2( 0x00000000, 0x3ffc5000, 0x804855e6, 0x3fe21579, 0x41131800, 0x3fea5906, 0xcc0d43ba, 0xbd34ded0 ),	/* row 98 */ 
	/* 3416 */	 DATA_4x2( 0x00000000, 0x3ffc7000, 0x12012012, 0x3fe20120, 0xb17e2800, 0x3fea8d16, 0xe6b40f5e, 0xbd1769a8 ),	/* row 99 */ 
	/* 3448 */	 DATA_4x2( 0x00000000, 0x3ffc9000, 0x3c7fb84c, 0x3fe1ecf4, 0xac991000, 0x3feac0ec, 0xe9e746a4, 0x3d39d941 ),	/* row 100 */ 
	/* 3480 */	 DATA_4x2( 0x00000000, 0x3ffcb000, 0x672e4abd, 0x3fe1d8f5, 0xb5179000, 0x3feaf488, 0xac74b87f, 0x3d36ad5b ),	/* row 101 */ 
	/* 3512 */	 DATA_4x2( 0x00000000, 0x3ffcd000, 0xfc1ce059, 0x3fe1c522, 0x4bf8f000, 0x3feb27eb, 0xd646cb9d, 0x3d1148da ),	/* row 102 */ 
	/* 3544 */	 DATA_4x2( 0x00000000, 0x3ffcf000, 0x67f2bae3, 0x3fe1b17c, 0xf08f9800, 0x3feb5b14, 0x9619fe2f, 0xbd29a197 ),	/* row 103 */ 
	/* 3576 */	 DATA_4x2( 0x00000000, 0x3ffd1000, 0x19e0119e, 0x3fe19e01, 0x20887000, 0x3feb8e06, 0xd1d92d88, 0x3d3848e9 ),	/* row 104 */ 
	/* 3608 */	 DATA_4x2( 0x00000000, 0x3ffd3000, 0x83902bdb, 0x3fe18ab0, 0x57f23800, 0x3febc0bf, 0xac948d1a, 0xbd2fa6e2 ),	/* row 105 */ 
	/* 3640 */	 DATA_4x2( 0x00000000, 0x3ffd5000, 0x191bd684, 0x3fe1778a, 0x11446800, 0x3febf341, 0x05c241a2, 0xbd3aca19 ),	/* row 106 */ 
	/* 3672 */	 DATA_4x2( 0x00000000, 0x3ffd7000, 0x50fc3201, 0x3fe1648d, 0xc5664800, 0x3fec258b, 0xdf4083bc, 0x3cf455be ),	/* row 107 */ 
	/* 3704 */	 DATA_4x2( 0x00000000, 0x3ffd9000, 0xa3fdd5c9, 0x3fe151b9, 0xebb5b800, 0x3fec579f, 0x66976b91, 0xbd2a82ed ),	/* row 108 */ 
	/* 3736 */	 DATA_4x2( 0x00000000, 0x3ffdb000, 0x8d344724, 0x3fe13f0e, 0xfa0db800, 0x3fec897d, 0x64ed16b7, 0xbd3393c6 ),	/* row 109 */ 
	/* 3768 */	 DATA_4x2( 0x00000000, 0x3ffdd000, 0x89edc0ac, 0x3fe12c8b, 0x64cd0000, 0x3fecbb26, 0x9427dd61, 0xbd385289 ),	/* row 110 */ 
	/* 3800 */	 DATA_4x2( 0x00000000, 0x3ffdf000, 0x19a74826, 0x3fe11a30, 0x9edc5000, 0x3fecec99, 0x15bb7de4, 0x3d3017eb ),	/* row 111 */ 
	/* 3832 */	 DATA_4x2( 0x00000000, 0x3ffe1000, 0xbe011080, 0x3fe107fb, 0x19b4c000, 0x3fed1dd8, 0x3917b407, 0x3d3f8649 ),	/* row 112 */ 
	/* 3864 */	 DATA_4x2( 0x00000000, 0x3ffe3000, 0xfab325a2, 0x3fe0f5ed, 0x4565c800, 0x3fed4ee2, 0x44e2aaee, 0xbd2d54d2 ),	/* row 113 */ 
	/* 3896 */	 DATA_4x2( 0x00000000, 0x3ffe5000, 0x55826011, 0x3fe0e406, 0x909b2800, 0x3fed7fb8, 0x81400bcf, 0x3d33623c ),	/* row 114 */ 
	/* 3928 */	 DATA_4x2( 0x00000000, 0x3ffe7000, 0x56359e3a, 0x3fe0d244, 0x68a2f800, 0x3fedb05b, 0xff5e0495, 0x3d3b253d ),	/* row 115 */ 
	/* 3960 */	 DATA_4x2( 0x00000000, 0x3ffe9000, 0x868b4171, 0x3fe0c0a7, 0x39733800, 0x3fede0cb, 0xa7bb24b2, 0x3d148cd0 ),	/* row 116 */ 
	/* 3992 */	 DATA_4x2( 0x00000000, 0x3ffeb000, 0x722eecb5, 0x3fe0af2f, 0x6daf7800, 0x3fee1108, 0x9b992dce, 0xbd3b4eb0 ),	/* row 117 */ 
	/* 4024 */	 DATA_4x2( 0x00000000, 0x3ffed000, 0xa6af8360, 0x3fe09ddb, 0x6eae5800, 0x3fee4113, 0xd82263ed, 0xbd361728 ),	/* row 118 */ 
	/* 4056 */	 DATA_4x2( 0x00000000, 0x3ffef000, 0xb37565e2, 0x3fe08cab, 0xa47ef800, 0x3fee70ec, 0xfdf33295, 0x3d0bb98a ),	/* row 119 */ 
	/* 4088 */	 DATA_4x2( 0x00000000, 0x3fff1000, 0x29b8eae2, 0x3fe07b9f, 0x75ee4000, 0x3feea094, 0x31517b71, 0xbd3e308e ),	/* row 120 */ 
	/* 4120 */	 DATA_4x2( 0x00000000, 0x3fff3000, 0x9c7912fb, 0x3fe06ab5, 0x488bf000, 0x3feed00b, 0x81c547e6, 0xbd3ee547 ),	/* row 121 */ 
	/* 4152 */	 DATA_4x2( 0x00000000, 0x3fff5000, 0xa0727586, 0x3fe059ee, 0x80afd000, 0x3feeff51, 0x799da52d, 0x3d3f1e32 ),	/* row 122 */ 
	/* 4184 */	 DATA_4x2( 0x00000000, 0x3fff7000, 0xcc1664c5, 0x3fe04949, 0x817eb800, 0x3fef2e67, 0x8bb672e8, 0x3d0142b0 ),	/* row 123 */ 
	/* 4216 */	 DATA_4x2( 0x00000000, 0x3fff9000, 0xb78247fc, 0x3fe038c6, 0xacef3800, 0x3fef5d4d, 0x220ccf53, 0xbd241a2f ),	/* row 124 */ 
	/* 4248 */	 DATA_4x2( 0x00000000, 0x3fffb000, 0xfc7729e9, 0x3fe02864, 0x63ce9000, 0x3fef8c04, 0x7240b4e8, 0xbd3cbb8d ),	/* row 125 */ 
	/* 4280 */	 DATA_4x2( 0x00000000, 0x3fffd000, 0x36517a37, 0x3fe01824, 0x05c54800, 0x3fefba8c, 0x8ac00d0f, 0xbd390602 ),	/* row 126 */ 
	/* 4312 */	 DATA_4x2( 0x00000000, 0x3ffff000, 0x02010080, 0x3fe00804, 0xf15bd000, 0x3fefe8e4, 0x9e2442e6, 0x3d2a025d ),	/* row 127 */ 
};


# define POLY_NEAR_M(x,y)  y =  (((x*(x*x))*(((POLY_ADDRESS_NEAR[1] \
	+x*POLY_ADDRESS_NEAR[2])+(x*x)*POLY_ADDRESS_NEAR[3])+(x*(x*x))*(POLY_ADDRESS_NEAR[4]+x*POLY_ADDRESS_NEAR[5]))) \
	+(((x*x)*(x*x))*((x*x)*(x*x)))*(((POLY_ADDRESS_NEAR[6]+x*POLY_ADDRESS_NEAR[7])+(x*x)*POLY_ADDRESS_NEAR[8])+(x*(x*x))*(POLY_ADDRESS_NEAR[9] \
	+x*POLY_ADDRESS_NEAR[10])))
# define POLY_NEAR_C(x,y)  y =  ((x*(x*x))*(POLY_ADDRESS_NEAR[1] \
	+x*(POLY_ADDRESS_NEAR[2]+x*(POLY_ADDRESS_NEAR[3]+x*(POLY_ADDRESS_NEAR[4]+x*(POLY_ADDRESS_NEAR[5] \
	+x*(POLY_ADDRESS_NEAR[6]+x*(POLY_ADDRESS_NEAR[7]+x*(POLY_ADDRESS_NEAR[8]+x*(POLY_ADDRESS_NEAR[9] \
	+x*POLY_ADDRESS_NEAR[10]))))))))))
# define POLY_NEAR SELECT_POLY(POLY_NEAR_)
# define POLY_NEAR_Q_M(x,y)  y =  ((x*(x*x))*(((B3+(x*x)*B5)+((x*x)*(x*x))*B7)+((x*x)*((x*x)*(x*x)))*(B9+(x*x)*B11)))
# define POLY_NEAR_Q_C(x,y)  y =  ((x*(x*x))*(B3+(x*x)*(B5+(x*x)*(B7+(x*x)*(B9+(x*x)*B11)))))
# define POLY_NEAR_Q SELECT_POLY(POLY_NEAR_Q_)
# define POLY_AWAY_M(x,y)  y =  ((x*x)*(((POLY_ADDRESS_AWAY[0]+x*POLY_ADDRESS_AWAY[1]) \
	+(x*x)*POLY_ADDRESS_AWAY[2])+(x*(x*x))*(POLY_ADDRESS_AWAY[3]+x*POLY_ADDRESS_AWAY[4])))
# define POLY_AWAY_C(x,y)  y =  ((x*x)*(POLY_ADDRESS_AWAY[0]+x*(POLY_ADDRESS_AWAY[1] \
	+x*(POLY_ADDRESS_AWAY[2]+x*(POLY_ADDRESS_AWAY[3]+x*POLY_ADDRESS_AWAY[4])))))
# define POLY_AWAY SELECT_POLY(POLY_AWAY_)
# define POLY_AWAY_Q_M(x,y)  y =  ((x*(x*x))*((C3+(x*x)*C5)+((x*x)*(x*x))*C7))
# define POLY_AWAY_Q_C(x,y)  y =  ((x*(x*x))*(C3+(x*x)*(C5+(x*x)*C7)))
# define POLY_AWAY_Q SELECT_POLY(POLY_AWAY_Q_)

#define LOG_TABLE_NAME __log2_t_table
#define TABLE_CONST  7
#define	F_ONE	*((double *) ((char *)__log2_t_table + 0))
#define	POLY_ADDRESS_NEAR	((double *) ((char *)__log2_t_table + 8))
#define	POLY_ADD_N_Q	((double *) ((char *)__log2_t_table + 96))
#define B3 POLY_ADD_N_Q[0]
#define B5 POLY_ADD_N_Q[1]
#define B7 POLY_ADD_N_Q[2]
#define B9 POLY_ADD_N_Q[3]
#define B11 POLY_ADD_N_Q[4]
#define	POLY_ADDRESS_AWAY	((double *) ((char *)__log2_t_table + 136))
#define	POLY_ADD_A_Q	((double *) ((char *)__log2_t_table + 176))
#define C3 POLY_ADD_A_Q[0]
#define C5 POLY_ADD_A_Q[1]
#define C7 POLY_ADD_A_Q[2]
#define	LOG2_HI	*((double *) ((char *)__log2_t_table + 200))
#define	LOG2_LO	*((double *) ((char *)__log2_t_table + 208))
#define	LOGE_HI	*((double *) ((char *)__log2_t_table + 216))
#define	LOGE_HI2	*((double *) ((char *)__log2_t_table + 224))
#define	LOGE_LO	*((double *) ((char *)__log2_t_table + 232))
#define	LOGE_LO2	*((double *) ((char *)__log2_t_table + 240))
#define LOG_F_TABLE  248 
#define T1_64  (WORD) 0x3fed900000000000  
#define T2_64  (WORD) 0x3ff1380000000000  
#define T1_32  (WORD) 0x3fed9000  
#define T2_32  (WORD) 0x3ff13800  
#define T2_MINUS_T1    (T2 - T1) 