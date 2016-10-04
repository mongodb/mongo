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





    static const TABLE_UNION __log_t_table[] = { 

	/* 1.0 in working precision */
	/* 000 */ DATA_1x2( 0x00000000, 0x3ff00000 ),

	/* poly coeffs, near 1 */
	/* 008 */ DATA_1x2( 0x00000000, 0xbfe00000 ),
	/* 016 */ DATA_1x2( 0x555555a8, 0x3fd55555 ),
	/* 024 */ DATA_1x2( 0x000000b8, 0xbfd00000 ),
	/* 032 */ DATA_1x2( 0x9992f6d5, 0x3fc99999 ),
	/* 040 */ DATA_1x2( 0x554afda5, 0xbfc55555 ),
	/* 048 */ DATA_1x2( 0xab58eabc, 0x3fc24924 ),
	/* 056 */ DATA_1x2( 0x1fbcbf56, 0xbfc00000 ),
	/* 064 */ DATA_1x2( 0x962371db, 0x3fbc7171 ),
	/* 072 */ DATA_1x2( 0x2ed5abd9, 0xbfb9993b ),
	/* 080 */ DATA_1x2( 0x29274a73, 0x3fb788ff ),
	/* 088 */ DATA_1x2( 0x2a983eec, 0xbfb5988c ),

	/* poly coeffs, quotient, near 1 */
	/* 096 */ DATA_1x2( 0x555555a8, 0x3fb55555 ),
	/* 104 */ DATA_1x2( 0x9992f6ab, 0x3f899999 ),
	/* 112 */ DATA_1x2( 0xab595325, 0x3f624924 ),
	/* 120 */ DATA_1x2( 0x955008e2, 0x3f3c7171 ),
	/* 128 */ DATA_1x2( 0x74105fa7, 0x3f1788ff ),

	/* poly coeffs, away from 1 */
	/* 136 */ DATA_1x2( 0x00000000, 0xbfe00000 ),
	/* 144 */ DATA_1x2( 0x555030bd, 0x3fd55555 ),
	/* 152 */ DATA_1x2( 0xfff2b6c9, 0xbfcfffff ),
	/* 160 */ DATA_1x2( 0x07695009, 0x3fc999b5 ),
	/* 168 */ DATA_1x2( 0xc32834fd, 0xbfc55570 ),

	/* poly coeffs, quotient, away from 1 */
	/* 176 */ DATA_1x2( 0x555555c7, 0x3fb55555 ),
	/* 184 */ DATA_1x2( 0x987d20f4, 0x3f899999 ),
	/* 192 */ DATA_1x2( 0x5b736978, 0x3f624996 ),


	/* log of 2 in hi and lo parts */
	/* 200 */ DATA_1x2( 0xfefa3800, 0x3fe62e42 ),
	/* 208 */ DATA_1x2( 0x93c76730, 0x3d2ef357 ),


	/* Table of F, 1/F, and hi and lo log of F */
	/* 216 */	 DATA_4x2( 0x00000000, 0x3ff01000, 0xe01fe020, 0x3fefe01f, 0xa2b00000, 0x3f6ff00a, 0xa086b56a, 0x3d20bc04 ),	/* row 0 */ 
	/* 248 */	 DATA_4x2( 0x00000000, 0x3ff03000, 0xaa01fa12, 0x3fefa11c, 0x5f820000, 0x3f87dc47, 0x5b5da1f5, 0xbd3eb124 ),	/* row 1 */ 
	/* 280 */	 DATA_4x2( 0x00000000, 0x3ff05000, 0xaca0dbb5, 0x3fef6310, 0x43470000, 0x3f93cea4, 0x32d6a40b, 0xbd36a2c4 ),	/* row 2 */ 
	/* 312 */	 DATA_4x2( 0x00000000, 0x3ff07000, 0x44230ab5, 0x3fef25f6, 0x27b00000, 0x3f9b9fc0, 0x0ae6922a, 0xbd3b9a01 ),	/* row 3 */ 
	/* 344 */	 DATA_4x2( 0x00000000, 0x3ff09000, 0xf8458e02, 0x3feee9c7, 0x89240000, 0x3fa1b0d9, 0x9ae889bb, 0xbd33401e ),	/* row 4 */ 
	/* 376 */	 DATA_4x2( 0x00000000, 0x3ff0b000, 0x7aba01eb, 0x3feeae80, 0xafc90000, 0x3fa58a5b, 0x9570ad39, 0xbd2b2b73 ),	/* row 5 */ 
	/* 408 */	 DATA_4x2( 0x00000000, 0x3ff0d000, 0xa59750e4, 0x3fee741a, 0x0ec90000, 0x3fa95c83, 0x97c5feb8, 0xbd2c1482 ),	/* row 6 */ 
	/* 440 */	 DATA_4x2( 0x00000000, 0x3ff0f000, 0x79dc1a73, 0x3fee3a91, 0x8adb0000, 0x3fad276b, 0xc78a64b0, 0x3d16a423 ),	/* row 7 */ 
	/* 472 */	 DATA_4x2( 0x00000000, 0x3ff11000, 0x1e01e01e, 0x3fee01e0, 0x35990000, 0x3fb07598, 0xe4b59987, 0xbd3b8ecf ),	/* row 8 */ 
	/* 504 */	 DATA_4x2( 0x00000000, 0x3ff13000, 0xdca01dca, 0x3fedca01, 0x2f0a0000, 0x3fb253f6, 0xfb69a701, 0x3d3416f8 ),	/* row 9 */ 
	/* 536 */	 DATA_4x2( 0x00000000, 0x3ff15000, 0x231e7f8a, 0x3fed92f2, 0xbea64000, 0x3fb42edc, 0xea7c9acd, 0x3d1bc0ee ),	/* row 10 */ 
	/* 568 */	 DATA_4x2( 0x00000000, 0x3ff17000, 0x807572b2, 0x3fed5cac, 0xa9374000, 0x3fb60658, 0xdee9c4f8, 0x3d30c3b1 ),	/* row 11 */ 
	/* 600 */	 DATA_4x2( 0x00000000, 0x3ff19000, 0xa3fc5b1a, 0x3fed272c, 0x6d7b0000, 0x3fb7da76, 0x4480c89b, 0x3d32cc84 ),	/* row 12 */ 
	/* 632 */	 DATA_4x2( 0x00000000, 0x3ff1b000, 0x5c44bfc6, 0x3fecf26e, 0x46204000, 0x3fb9ab42, 0x26787061, 0xbd28a648 ),	/* row 13 */ 
	/* 664 */	 DATA_4x2( 0x00000000, 0x3ff1d000, 0x9601cbe7, 0x3fecbe6d, 0x2bb10000, 0x3fbb78c8, 0xbc3987e7, 0xbd325ef7 ),	/* row 14 */ 
	/* 696 */	 DATA_4x2( 0x00000000, 0x3ff1f000, 0x5afb8a42, 0x3fec8b26, 0xd66cc000, 0x3fbd4313, 0x79135713, 0xbd294543 ),	/* row 15 */ 
	/* 728 */	 DATA_4x2( 0x00000000, 0x3ff21000, 0xd10d4986, 0x3fec5894, 0xc0118000, 0x3fbf0a30, 0x83368e91, 0xbd3d599e ),	/* row 16 */ 
	/* 760 */	 DATA_4x2( 0x00000000, 0x3ff23000, 0x392ea01c, 0x3fec26b5, 0x12ca6000, 0x3fc06715, 0x9cdc0a3d, 0xbd2a4757 ),	/* row 17 */ 
	/* 792 */	 DATA_4x2( 0x00000000, 0x3ff25000, 0xee868d8b, 0x3febf583, 0x84674000, 0x3fc14785, 0x1027c750, 0x3d156345 ),	/* row 18 */ 
	/* 824 */	 DATA_4x2( 0x00000000, 0x3ff27000, 0x65883e7b, 0x3febc4fd, 0x190a6000, 0x3fc2266f, 0xb840e7f6, 0xbd24d20a ),	/* row 19 */ 
	/* 856 */	 DATA_4x2( 0x00000000, 0x3ff29000, 0x2b18ff23, 0x3feb951e, 0x18e48000, 0x3fc303d7, 0xce3ecb05, 0xbcd680b5 ),	/* row 20 */ 
	/* 888 */	 DATA_4x2( 0x00000000, 0x3ff2b000, 0xe3beee05, 0x3feb65e2, 0xb0ecc000, 0x3fc3dfc2, 0x62b8c13f, 0x3d28a72a ),	/* row 21 */ 
	/* 920 */	 DATA_4x2( 0x00000000, 0x3ff2d000, 0x4ad806ce, 0x3feb3748, 0xf39a6000, 0x3fc4ba36, 0xb3f219e5, 0xbd34354b ),	/* row 22 */ 
	/* 952 */	 DATA_4x2( 0x00000000, 0x3ff2f000, 0x31d922a4, 0x3feb094b, 0xd9982000, 0x3fc59338, 0xb7555d4a, 0x3cf0ba68 ),	/* row 23 */ 
	/* 984 */	 DATA_4x2( 0x00000000, 0x3ff31000, 0x7f94905e, 0x3feadbe8, 0x4272a000, 0x3fc66acd, 0xbfc6c785, 0x3d3aa1bd ),	/* row 24 */ 
	/* 1016 */	 DATA_4x2( 0x00000000, 0x3ff33000, 0x2f87ebfd, 0x3feaaf1d, 0xf5404000, 0x3fc740f8, 0x99018aa1, 0xbd30b66c ),	/* row 25 */ 
	/* 1048 */	 DATA_4x2( 0x00000000, 0x3ff35000, 0x5130e159, 0x3fea82e6, 0xa1436000, 0x3fc815c0, 0xf9201ce8, 0xbd302a52 ),	/* row 26 */ 
	/* 1080 */	 DATA_4x2( 0x00000000, 0x3ff37000, 0x07688a4a, 0x3fea5741, 0xde886000, 0x3fc8e928, 0xb13d72d5, 0x3d3a8154 ),	/* row 27 */ 
	/* 1112 */	 DATA_4x2( 0x00000000, 0x3ff39000, 0x87c51ca0, 0x3fea2c2a, 0x2e7e0000, 0x3fc9bb36, 0xa1ce0ffc, 0xbd21f2a8 ),	/* row 28 */ 
	/* 1144 */	 DATA_4x2( 0x00000000, 0x3ff3b000, 0x1a01a01a, 0x3fea01a0, 0xfc882000, 0x3fca8bec, 0xcf21b9cf, 0x3d3e3185 ),	/* row 29 */ 
	/* 1176 */	 DATA_4x2( 0x00000000, 0x3ff3d000, 0x176b682d, 0x3fe9d79f, 0x9e8fc000, 0x3fcb5b51, 0xec011f31, 0xbd34b722 ),	/* row 30 */ 
	/* 1208 */	 DATA_4x2( 0x00000000, 0x3ff3f000, 0xea5510da, 0x3fe9ae24, 0x558c2000, 0x3fcc2968, 0xdee38a40, 0xbd2cfd73 ),	/* row 31 */ 
	/* 1240 */	 DATA_4x2( 0x00000000, 0x3ff41000, 0x0d8ec0ff, 0x3fe9852f, 0x4e09c000, 0x3fccf635, 0x9a07d55b, 0x3d277123 ),	/* row 32 */ 
	/* 1272 */	 DATA_4x2( 0x00000000, 0x3ff43000, 0x0be377ae, 0x3fe95cbb, 0xa0abe000, 0x3fcdc1bc, 0xa628ccc6, 0x3d38fac1 ),	/* row 33 */ 
	/* 1304 */	 DATA_4x2( 0x00000000, 0x3ff45000, 0x7f9b2ce6, 0x3fe934c6, 0x52aa6000, 0x3fce8c02, 0x80e8e6ff, 0xbd26805b ),	/* row 34 */ 
	/* 1336 */	 DATA_4x2( 0x00000000, 0x3ff47000, 0x120190d5, 0x3fe90d4f, 0x564b8000, 0x3fcf550a, 0xa09202fe, 0xbd2323e3 ),	/* row 35 */ 
	/* 1368 */	 DATA_4x2( 0x00000000, 0x3ff49000, 0x7af1373f, 0x3fe8e652, 0x45ad5000, 0x3fd00e6c, 0x52e01203, 0x3cdcc68d ),	/* row 36 */ 
	/* 1400 */	 DATA_4x2( 0x00000000, 0x3ff4b000, 0x8062ff3a, 0x3fe8bfce, 0x5fcd6000, 0x3fd071b8, 0xa3e01a11, 0xbd3bcb8b ),	/* row 37 */ 
	/* 1432 */	 DATA_4x2( 0x00000000, 0x3ff4d000, 0xf601899c, 0x3fe899c0, 0x579ab000, 0x3fd0d46b, 0xf640e1e6, 0x3d3d2c81 ),	/* row 38 */ 
	/* 1464 */	 DATA_4x2( 0x00000000, 0x3ff4f000, 0xbcc092b9, 0x3fe87427, 0x0293b000, 0x3fd13687, 0x99d67123, 0xbd3d3e84 ),	/* row 39 */ 
	/* 1496 */	 DATA_4x2( 0x00000000, 0x3ff51000, 0xc2780614, 0x3fe84f00, 0x2dd42000, 0x3fd1980d, 0x7a361c9a, 0x3d2b7b3a ),	/* row 40 */ 
	/* 1528 */	 DATA_4x2( 0x00000000, 0x3ff53000, 0x0182a4a0, 0x3fe82a4a, 0x9e48a000, 0x3fd1f8ff, 0x040cbe77, 0x3d27946c ),	/* row 41 */ 
	/* 1560 */	 DATA_4x2( 0x00000000, 0x3ff55000, 0x80601806, 0x3fe80601, 0x10df7000, 0x3fd25960, 0x224ea3e3, 0x3d38e7bc ),	/* row 42 */ 
	/* 1592 */	 DATA_4x2( 0x00000000, 0x3ff57000, 0x515a4f1d, 0x3fe7e225, 0x3ab8a000, 0x3fd2b930, 0xd6bfb0a5, 0xbd26db12 ),	/* row 43 */ 
	/* 1624 */	 DATA_4x2( 0x00000000, 0x3ff59000, 0x922e017c, 0x3fe7beb3, 0xc9544000, 0x3fd31871, 0x94cecfd9, 0x3d184fab ),	/* row 44 */ 
	/* 1656 */	 DATA_4x2( 0x00000000, 0x3ff5b000, 0x6bb6398b, 0x3fe79baa, 0x62bfe000, 0x3fd37726, 0xac53b023, 0xbd3e9436 ),	/* row 45 */ 
	/* 1688 */	 DATA_4x2( 0x00000000, 0x3ff5d000, 0x119ac60d, 0x3fe77908, 0xa5c1f000, 0x3fd3d54f, 0xd9a395e3, 0x3d3c3e1c ),	/* row 46 */ 
	/* 1720 */	 DATA_4x2( 0x00000000, 0x3ff5f000, 0xc201756d, 0x3fe756ca, 0x2a04f000, 0x3fd432ef, 0x931715ad, 0xbd3fb129 ),	/* row 47 */ 
	/* 1752 */	 DATA_4x2( 0x00000000, 0x3ff61000, 0xc541fe8d, 0x3fe734f0, 0x80401000, 0x3fd49006, 0xfe1a0f8c, 0xbd38bccf ),	/* row 48 */ 
	/* 1784 */	 DATA_4x2( 0x00000000, 0x3ff63000, 0x6d9c7c09, 0x3fe71378, 0x32600000, 0x3fd4ec97, 0xaf04d104, 0x3d234d7a ),	/* row 49 */ 
	/* 1816 */	 DATA_4x2( 0x00000000, 0x3ff65000, 0x16f26017, 0x3fe6f260, 0xc3add000, 0x3fd548a2, 0x63081cf7, 0x3d23167e ),	/* row 50 */ 
	/* 1848 */	 DATA_4x2( 0x00000000, 0x3ff67000, 0x2681c861, 0x3fe6d1a6, 0xb0f4d000, 0x3fd5a42a, 0x2df7ba69, 0xbcde63af ),	/* row 51 */ 
	/* 1880 */	 DATA_4x2( 0x00000000, 0x3ff69000, 0x0aa31a3d, 0x3fe6b149, 0x70a79000, 0x3fd5ff30, 0x9f105039, 0x3d2e9e43 ),	/* row 52 */ 
	/* 1912 */	 DATA_4x2( 0x00000000, 0x3ff6b000, 0x3a88d0c0, 0x3fe69147, 0x7303e000, 0x3fd659b5, 0xb0af8efc, 0x3d1f281d ),	/* row 53 */ 
	/* 1944 */	 DATA_4x2( 0x00000000, 0x3ff6d000, 0x3601671a, 0x3fe6719f, 0x22359000, 0x3fd6b3bb, 0x7a933268, 0x3d30f625 ),	/* row 54 */ 
	/* 1976 */	 DATA_4x2( 0x00000000, 0x3ff6f000, 0x853b4aa3, 0x3fe6524f, 0xe2789000, 0x3fd70d42, 0x337ee287, 0x3d21aead ),	/* row 55 */ 
	/* 2008 */	 DATA_4x2( 0x00000000, 0x3ff71000, 0xb88ac0de, 0x3fe63356, 0x1239e000, 0x3fd7664e, 0x6aeb27af, 0xbd30c4fb ),	/* row 56 */ 
	/* 2040 */	 DATA_4x2( 0x00000000, 0x3ff73000, 0x6831ae94, 0x3fe614b3, 0x0a37b000, 0x3fd7bede, 0x3cb9801a, 0xbcf01878 ),	/* row 57 */ 
	/* 2072 */	 DATA_4x2( 0x00000000, 0x3ff75000, 0x34292dfc, 0x3fe5f664, 0x1da0d000, 0x3fd816f4, 0xdc35fb49, 0x3d3256d6 ),	/* row 58 */ 
	/* 2104 */	 DATA_4x2( 0x00000000, 0x3ff77000, 0xc3ece2a5, 0x3fe5d867, 0x9a331000, 0x3fd86e91, 0x0c9d2029, 0xbd317fd8 ),	/* row 59 */ 
	/* 2136 */	 DATA_4x2( 0x00000000, 0x3ff79000, 0xc647fa91, 0x3fe5babc, 0xc858b000, 0x3fd8c5b7, 0x54b02060, 0x3d322a1f ),	/* row 60 */ 
	/* 2168 */	 DATA_4x2( 0x00000000, 0x3ff7b000, 0xf123ccaa, 0x3fe59d61, 0xeb45b000, 0x3fd91c67, 0xe0ae234b, 0xbd3f09e0 ),	/* row 61 */ 
	/* 2200 */	 DATA_4x2( 0x00000000, 0x3ff7d000, 0x01580560, 0x3fe58056, 0x41135000, 0x3fd972a3, 0x027492dc, 0x3d158697 ),	/* row 62 */ 
	/* 2232 */	 DATA_4x2( 0x00000000, 0x3ff7f000, 0xba7c52e2, 0x3fe56397, 0x02dc1000, 0x3fd9c86b, 0x7eeb69dd, 0xbd3e7591 ),	/* row 63 */ 
	/* 2264 */	 DATA_4x2( 0x00000000, 0x3ff81000, 0xe6bb82fe, 0x3fe54725, 0x64d5c000, 0x3fda1dc0, 0xed796746, 0xbd39aa6f ),	/* row 64 */ 
	/* 2296 */	 DATA_4x2( 0x00000000, 0x3ff83000, 0x56a8054b, 0x3fe52aff, 0x966be000, 0x3fda72a4, 0x6253960a, 0xbd3857a5 ),	/* row 65 */ 
	/* 2328 */	 DATA_4x2( 0x00000000, 0x3ff85000, 0xe111c4c5, 0x3fe50f22, 0xc258b000, 0x3fdac718, 0x63d6f46f, 0x3d0c8181 ),	/* row 66 */ 
	/* 2360 */	 DATA_4x2( 0x00000000, 0x3ff87000, 0x62dd4c9b, 0x3fe4f38f, 0x0ebe0000, 0x3fdb1b1e, 0x70d3eeba, 0xbd2d24b7 ),	/* row 67 */ 
	/* 2392 */	 DATA_4x2( 0x00000000, 0x3ff89000, 0xbedc2c4c, 0x3fe4d843, 0x9d3cf000, 0x3fdb6eb5, 0x486659b3, 0x3d2aecea ),	/* row 68 */ 
	/* 2424 */	 DATA_4x2( 0x00000000, 0x3ff8b000, 0xdda68fe1, 0x3fe4bd3e, 0x8b0db000, 0x3fdbc1e0, 0x2f1f1f55, 0xbd27adec ),	/* row 69 */ 
	/* 2456 */	 DATA_4x2( 0x00000000, 0x3ff8d000, 0xad76014a, 0x3fe4a27f, 0xf115f000, 0x3fdc149f, 0x68de7f3a, 0x3ce35668 ),	/* row 70 */ 
	/* 2488 */	 DATA_4x2( 0x00000000, 0x3ff8f000, 0x22014880, 0x3fe48805, 0xe3ff7000, 0x3fdc66f4, 0x8e4b16d1, 0xbcc03052 ),	/* row 71 */ 
	/* 2520 */	 DATA_4x2( 0x00000000, 0x3ff91000, 0x34596066, 0x3fe46dce, 0x744d8000, 0x3fdcb8e0, 0x443cd10a, 0xbd34d80a ),	/* row 72 */ 
	/* 2552 */	 DATA_4x2( 0x00000000, 0x3ff93000, 0xe2c776ca, 0x3fe453d9, 0xae722000, 0x3fdd0a63, 0x663dda78, 0xbd19bdaa ),	/* row 73 */ 
	/* 2584 */	 DATA_4x2( 0x00000000, 0x3ff95000, 0x30abee4d, 0x3fe43a27, 0x9ae2c000, 0x3fdd5b7f, 0x20c03daa, 0x3d3a0f2c ),	/* row 74 */ 
	/* 2616 */	 DATA_4x2( 0x00000000, 0x3ff97000, 0x265e5951, 0x3fe420b5, 0x3e2c6000, 0x3fddac35, 0xc65a3f2f, 0xbd3aaf73 ),	/* row 75 */ 
	/* 2648 */	 DATA_4x2( 0x00000000, 0x3ff99000, 0xd10e6566, 0x3fe40782, 0x9906d000, 0x3fddfc85, 0xe1399f96, 0x3d36d501 ),	/* row 76 */ 
	/* 2680 */	 DATA_4x2( 0x00000000, 0x3ff9b000, 0x42a5af07, 0x3fe3ee8f, 0xa8687000, 0x3fde4c71, 0x3c91f0fb, 0x3d3c10b3 ),	/* row 77 */ 
	/* 2712 */	 DATA_4x2( 0x00000000, 0x3ff9d000, 0x91aa75c6, 0x3fe3d5d9, 0x65986000, 0x3fde9bfa, 0xebf1f6f8, 0x3d1f5646 ),	/* row 78 */ 
	/* 2744 */	 DATA_4x2( 0x00000000, 0x3ff9f000, 0xd9232955, 0x3fe3bd60, 0xc640e000, 0x3fdeeb20, 0xc8e28371, 0xbd205e53 ),	/* row 79 */ 
	/* 2776 */	 DATA_4x2( 0x00000000, 0x3ffa1000, 0x387ac822, 0x3fe3a524, 0xbc812000, 0x3fdf39e5, 0xf8eef763, 0xbd1a432f ),	/* row 80 */ 
	/* 2808 */	 DATA_4x2( 0x00000000, 0x3ffa3000, 0xd366088e, 0x3fe38d22, 0x36fea000, 0x3fdf884a, 0xd46c3fdf, 0xbd13dd39 ),	/* row 81 */ 
	/* 2840 */	 DATA_4x2( 0x00000000, 0x3ffa5000, 0xd1c945ee, 0x3fe3755b, 0x20f61000, 0x3fdfd64f, 0x27a9e98b, 0x3d35c729 ),	/* row 82 */ 
	/* 2872 */	 DATA_4x2( 0x00000000, 0x3ffa7000, 0x5f9f2af8, 0x3fe35dce, 0xb1260000, 0x3fe011fa, 0xc8afdee9, 0xbd0d79fb ),	/* row 83 */ 
	/* 2904 */	 DATA_4x2( 0x00000000, 0x3ffa9000, 0xace01346, 0x3fe34679, 0xefce6000, 0x3fe0389e, 0x55c53483, 0x3d39d9e1 ),	/* row 84 */ 
	/* 2936 */	 DATA_4x2( 0x00000000, 0x3ffab000, 0xed6a1dfa, 0x3fe32f5c, 0xbd264800, 0x3fe05f14, 0x291c46c2, 0xbd331fab ),	/* row 85 */ 
	/* 2968 */	 DATA_4x2( 0x00000000, 0x3ffad000, 0x58e9ebb6, 0x3fe31877, 0x884b4800, 0x3fe0855c, 0x4fb236c2, 0xbd378d1f ),	/* row 86 */ 
	/* 3000 */	 DATA_4x2( 0x00000000, 0x3ffaf000, 0x2ac40260, 0x3fe301c8, 0xbece1800, 0x3fe0ab76, 0x6c935454, 0xbd3971fd ),	/* row 87 */ 
	/* 3032 */	 DATA_4x2( 0x00000000, 0x3ffb1000, 0xa1fed14b, 0x3fe2eb4e, 0xccb9d800, 0x3fe0d163, 0xb9a9a8bc, 0xbd2481f7 ),	/* row 88 */ 
	/* 3064 */	 DATA_4x2( 0x00000000, 0x3ffb3000, 0x012d50a0, 0x3fe2d50a, 0x1c9b4800, 0x3fe0f724, 0x110ee76c, 0x3d27d4ea ),	/* row 89 */ 
	/* 3096 */	 DATA_4x2( 0x00000000, 0x3ffb5000, 0x8e5a3711, 0x3fe2bef9, 0x1787d000, 0x3fe11cb8, 0x8f0a9c06, 0xbd383dfb ),	/* row 90 */ 
	/* 3128 */	 DATA_4x2( 0x00000000, 0x3ffb7000, 0x92f3c105, 0x3fe2a91c, 0x25244000, 0x3fe14220, 0x43892b6d, 0xbd35d86b ),	/* row 91 */ 
	/* 3160 */	 DATA_4x2( 0x00000000, 0x3ffb9000, 0x5bb804a5, 0x3fe29372, 0xababa800, 0x3fe1675c, 0x3382a8f0, 0xbd2f1fc6 ),	/* row 92 */ 
	/* 3192 */	 DATA_4x2( 0x00000000, 0x3ffbb000, 0x38a1ce4d, 0x3fe27dfa, 0x0ff5d000, 0x3fe18c6e, 0xaebd3d3a, 0xbd1f3e89 ),	/* row 93 */ 
	/* 3224 */	 DATA_4x2( 0x00000000, 0x3ffbd000, 0x7cd60127, 0x3fe268b3, 0xb57da000, 0x3fe1b154, 0x70a5c125, 0x3d34f77f ),	/* row 94 */ 
	/* 3256 */	 DATA_4x2( 0x00000000, 0x3ffbf000, 0x7e9177b2, 0x3fe2539d, 0xfe677000, 0x3fe1d610, 0x63647964, 0x3cb84275 ),	/* row 95 */ 
	/* 3288 */	 DATA_4x2( 0x00000000, 0x3ffc1000, 0x9717605b, 0x3fe23eb7, 0x4b870800, 0x3fe1faa3, 0xbdc7bd0d, 0x3d24c0c0 ),	/* row 96 */ 
	/* 3320 */	 DATA_4x2( 0x00000000, 0x3ffc3000, 0x22a0122a, 0x3fe22a01, 0xfc65c000, 0x3fe21f0b, 0x4f0c9188, 0xbd2141e2 ),	/* row 97 */ 
	/* 3352 */	 DATA_4x2( 0x00000000, 0x3ffc5000, 0x804855e6, 0x3fe21579, 0x6f483800, 0x3fe2434b, 0x44730f09, 0x3d233e21 ),	/* row 98 */ 
	/* 3384 */	 DATA_4x2( 0x00000000, 0x3ffc7000, 0x12012012, 0x3fe20120, 0x01343000, 0x3fe26762, 0x5aa1f8e6, 0x3d1bf9a5 ),	/* row 99 */ 
	/* 3416 */	 DATA_4x2( 0x00000000, 0x3ffc9000, 0x3c7fb84c, 0x3fe1ecf4, 0x0df60800, 0x3fe28b50, 0x60605aab, 0xbd0f543f ),	/* row 100 */ 
	/* 3448 */	 DATA_4x2( 0x00000000, 0x3ffcb000, 0x672e4abd, 0x3fe1d8f5, 0xf0264000, 0x3fe2af15, 0x0c8a495a, 0x3d15a396 ),	/* row 101 */ 
	/* 3480 */	 DATA_4x2( 0x00000000, 0x3ffcd000, 0xfc1ce059, 0x3fe1c522, 0x012ee000, 0x3fe2d2b4, 0x2c593364, 0xbd3b12a2 ),	/* row 102 */ 
	/* 3512 */	 DATA_4x2( 0x00000000, 0x3ffcf000, 0x67f2bae3, 0x3fe1b17c, 0x99509800, 0x3fe2f62a, 0x9798c600, 0xbd35ce93 ),	/* row 103 */ 
	/* 3544 */	 DATA_4x2( 0x00000000, 0x3ffd1000, 0x19e0119e, 0x3fe19e01, 0x0fa80000, 0x3fe3197a, 0xcb70468f, 0xbd295e29 ),	/* row 104 */ 
	/* 3576 */	 DATA_4x2( 0x00000000, 0x3ffd3000, 0x83902bdb, 0x3fe18ab0, 0xba328800, 0x3fe33ca2, 0xae99bf42, 0x3d294c81 ),	/* row 105 */ 
	/* 3608 */	 DATA_4x2( 0x00000000, 0x3ffd5000, 0x191bd684, 0x3fe1778a, 0xedd37000, 0x3fe35fa4, 0x0572fed3, 0xbd25ffdb ),	/* row 106 */ 
	/* 3640 */	 DATA_4x2( 0x00000000, 0x3ffd7000, 0x50fc3201, 0x3fe1648d, 0xfe587800, 0x3fe38280, 0x90b27564, 0x3d27ebfa ),	/* row 107 */ 
	/* 3672 */	 DATA_4x2( 0x00000000, 0x3ffd9000, 0xa3fdd5c9, 0x3fe151b9, 0x3e7ec000, 0x3fe3a537, 0x1eeeb71f, 0xbd30339b ),	/* row 108 */ 
	/* 3704 */	 DATA_4x2( 0x00000000, 0x3ffdb000, 0x8d344724, 0x3fe13f0e, 0xfff73000, 0x3fe3c7c7, 0x7f248fda, 0x3d302e41 ),	/* row 109 */ 
	/* 3736 */	 DATA_4x2( 0x00000000, 0x3ffdd000, 0x89edc0ac, 0x3fe12c8b, 0x936b3000, 0x3fe3ea33, 0xc8b4509b, 0xbd148f84 ),	/* row 110 */ 
	/* 3768 */	 DATA_4x2( 0x00000000, 0x3ffdf000, 0x19a74826, 0x3fe11a30, 0x4880e000, 0x3fe40c7a, 0x0dd21803, 0xbd38b6eb ),	/* row 111 */ 
	/* 3800 */	 DATA_4x2( 0x00000000, 0x3ffe1000, 0xbe011080, 0x3fe107fb, 0x6ddf8000, 0x3fe42e9c, 0xf71e9942, 0x3d17e595 ),	/* row 112 */ 
	/* 3832 */	 DATA_4x2( 0x00000000, 0x3ffe3000, 0xfab325a2, 0x3fe0f5ed, 0x5133b800, 0x3fe4509a, 0xfc50a5af, 0x3d385281 ),	/* row 113 */ 
	/* 3864 */	 DATA_4x2( 0x00000000, 0x3ffe5000, 0x55826011, 0x3fe0e406, 0x3f33a800, 0x3fe47274, 0x6cf012a3, 0x3d35698d ),	/* row 114 */ 
	/* 3896 */	 DATA_4x2( 0x00000000, 0x3ffe7000, 0x56359e3a, 0x3fe0d244, 0x83a30000, 0x3fe4942a, 0xe757735b, 0xbd3fc425 ),	/* row 115 */ 
	/* 3928 */	 DATA_4x2( 0x00000000, 0x3ffe9000, 0x868b4171, 0x3fe0c0a7, 0x6956e000, 0x3fe4b5bd, 0x0bf2822b, 0x3d339c6f ),	/* row 116 */ 
	/* 3960 */	 DATA_4x2( 0x00000000, 0x3ffeb000, 0x722eecb5, 0x3fe0af2f, 0x3a3a0000, 0x3fe4d72d, 0x5657d640, 0xbd37fdc6 ),	/* row 117 */ 
	/* 3992 */	 DATA_4x2( 0x00000000, 0x3ffed000, 0xa6af8360, 0x3fe09ddb, 0x3f502800, 0x3fe4f87a, 0x2a2c6f3b, 0xbd2177a3 ),	/* row 118 */ 
	/* 4024 */	 DATA_4x2( 0x00000000, 0x3ffef000, 0xb37565e2, 0x3fe08cab, 0xc0ba3800, 0x3fe519a4, 0x9bdae36b, 0xbd3dccc9 ),	/* row 119 */ 
	/* 4056 */	 DATA_4x2( 0x00000000, 0x3fff1000, 0x29b8eae2, 0x3fe07b9f, 0x05b99800, 0x3fe53aad, 0x6e9f5a3b, 0x3d3be554 ),	/* row 120 */ 
	/* 4088 */	 DATA_4x2( 0x00000000, 0x3fff3000, 0x9c7912fb, 0x3fe06ab5, 0x54b40800, 0x3fe55b93, 0x197a357d, 0x3d3e69e4 ),	/* row 121 */ 
	/* 4120 */	 DATA_4x2( 0x00000000, 0x3fff5000, 0xa0727586, 0x3fe059ee, 0xf336f000, 0x3fe57c57, 0xb1710de0, 0x3d29085a ),	/* row 122 */ 
	/* 4152 */	 DATA_4x2( 0x00000000, 0x3fff7000, 0xcc1664c5, 0x3fe04949, 0x25fae800, 0x3fe59cfb, 0xadf754c7, 0x3d0f7dd1 ),	/* row 123 */ 
	/* 4184 */	 DATA_4x2( 0x00000000, 0x3fff9000, 0xb78247fc, 0x3fe038c6, 0x30e72000, 0x3fe5bd7d, 0x392c926a, 0xbd3c6720 ),	/* row 124 */ 
	/* 4216 */	 DATA_4x2( 0x00000000, 0x3fffb000, 0xfc7729e9, 0x3fe02864, 0x57149800, 0x3fe5ddde, 0xe8df5d7c, 0x3d223773 ),	/* row 125 */ 
	/* 4248 */	 DATA_4x2( 0x00000000, 0x3fffd000, 0x36517a37, 0x3fe01824, 0xdad18800, 0x3fe5fe1e, 0xd27bc79d, 0x3d2188d5 ),	/* row 126 */ 
	/* 4280 */	 DATA_4x2( 0x00000000, 0x3ffff000, 0x02010080, 0x3fe00804, 0xfda46800, 0x3fe61e3e, 0x6e4fdbdf, 0xbd3ccb43 ),	/* row 127 */ 
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

#define LOG_TABLE_NAME __log_t_table
#define TABLE_CONST  7
#define	F_ONE	*((double *) ((char *)__log_t_table + 0))
#define	POLY_ADDRESS_NEAR	((double *) ((char *)__log_t_table + 8))
#define	POLY_ADD_N_Q	((double *) ((char *)__log_t_table + 96))
#define B3 POLY_ADD_N_Q[0]
#define B5 POLY_ADD_N_Q[1]
#define B7 POLY_ADD_N_Q[2]
#define B9 POLY_ADD_N_Q[3]
#define B11 POLY_ADD_N_Q[4]
#define	POLY_ADDRESS_AWAY	((double *) ((char *)__log_t_table + 136))
#define	POLY_ADD_A_Q	((double *) ((char *)__log_t_table + 176))
#define C3 POLY_ADD_A_Q[0]
#define C5 POLY_ADD_A_Q[1]
#define C7 POLY_ADD_A_Q[2]
#define	LOG2_HI	*((double *) ((char *)__log_t_table + 200))
#define	LOG2_LO	*((double *) ((char *)__log_t_table + 208))
#define LOG_F_TABLE  216 
#define T1_64  (WORD) 0x3fede00000000000  
#define T2_64  (WORD) 0x3ff1100000000000  
#define T1_32  (WORD) 0x3fede000  
#define T2_32  (WORD) 0x3ff11000  
#define T2_MINUS_T1    (T2 - T1) 