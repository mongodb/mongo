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



    static const TABLE_UNION TABLE_NAME[] = { 

	/* exp class-to-action-mapping */
	/* 000 */ DATA_1x2( 0x00ebb408, 0x54514510 ),

	/* expm1 class-to-action-mapping */
	/* 008 */ DATA_1x2( 0x00650408, 0x44104100 ),

	/* sinh class-to-action-mapping */
	/* 016 */ DATA_1x2( 0x00410400, 0x34104100 ),

	/* cosh class-to-action-mapping */
	/* 024 */ DATA_1x2( 0x00610408, 0x24514510 ),

	/* tanh class-to-action-mapping */
	/* 032 */ DATA_1x2( 0x00651408, 0x14104100 ),

	/* Data for the class to action mappings */
	/* 040 */ DATA_1x2( 0x00000001, 0x00000000 ),
	/* 048 */ DATA_1x2( 0x00000024, 0x00000000 ),
	/* 056 */ DATA_1x2( 0x00000023, 0x00000000 ),

	/* Constant structure for exp based evaluations */

	/* High digits of 1/ln2, ln2 and binary exponent of ln2 */
	/* 064 */ DATA_1x2( 0xae0bf85e, 0x5c551d94 ),
	/* 072 */ DATA_1x2( 0xd1cf79ac, 0xb17217f7 ),
	/* 080 */ DATA_1x2( 0x00000000, 0x00000000 ),

	/* ln2_lo = ln2 - ln2_hi in unpacked form */
	/* 088 */ NEG, 0-66, DATA_2x2( 0xf0342542, 0xd871319f, 0x359d2749, 0xfc32f366 ),

	/* Polynomial degree */
	/* 112 */ DATA_1x2( 0x00000016, 0x00000000 ),

	/* Fixed point coefficients for exp/expm1 evaluation */
	/* 120 */ DATA_4( 0x0393a749, 0x0219c729, 0x00000000, 0x00000000 ),
	/* 136 */ DATA_4( 0xb47b630c, 0x2e468fc7, 0x00000000, 0x00000000 ),
	/* 152 */ DATA_4( 0x7f5c80bd, 0xca85ad65, 0x00000003, 0x00000000 ),
	/* 168 */ DATA_4( 0x49b64eae, 0xd268b2cb, 0x0000004b, 0x00000000 ),
	/* 184 */ DATA_4( 0xeb90c661, 0x9e18d9e0, 0x000005a0, 0x00000000 ),
	/* 200 */ DATA_4( 0x8fe824f4, 0x1dc17846, 0x0000654b, 0x00000000 ),
	/* 216 */ DATA_4( 0x2631a1a2, 0xf9ccf184, 0x0006b9fc, 0x00000000 ),
	/* 232 */ DATA_4( 0x2f079eeb, 0x9ccece54, 0x006b9fcf, 0x00000000 ),
	/* 248 */ DATA_4( 0xf3934011, 0x301f26ef, 0x064e5d2a, 0x00000000 ),
	/* 264 */ DATA_4( 0x14562c06, 0xa1b4271d, 0x5849184e, 0x00000000 ),
	/* 280 */ DATA_4( 0x97a1173a, 0x3625ed56, 0x7bb63bfe, 0x00000004 ),
	/* 296 */ DATA_4( 0x4062e495, 0x89c71fc2, 0xcc8acfea, 0x00000035 ),
	/* 312 */ DATA_4( 0xf9b4c26e, 0xeb8e5ddf, 0xc9f6ef13, 0x0000024f ),
	/* 328 */ DATA_4( 0x198cd02d, 0x338faac2, 0xe3a556c7, 0x0000171d ),
	/* 344 */ DATA_4( 0x0e2c1d71, 0xd00d00d0, 0x00d00d00, 0x0000d00d ),
	/* 360 */ DATA_4( 0x66cfb7b5, 0x80680680, 0x06806806, 0x00068068 ),
	/* 376 */ DATA_4( 0xd829d3b1, 0x82d82d82, 0x2d82d82d, 0x002d82d8 ),
	/* 392 */ DATA_4( 0x1113746f, 0x11111111, 0x11111111, 0x01111111 ),
	/* 408 */ DATA_4( 0x55555aa3, 0x55555555, 0x55555555, 0x05555555 ),
	/* 424 */ DATA_4( 0x55555380, 0x55555555, 0x55555555, 0x15555555 ),
	/* 440 */ DATA_4( 0xfffffffe, 0xffffffff, 0xffffffff, 0x3fffffff ),
	/* 456 */ DATA_4( 0x00000000, 0x00000000, 0x00000000, 0x80000000 ),
	/* 472 */ DATA_4( 0x00000000, 0x00000000, 0x00000000, 0x80000000 ),
	/* 488 */ DATA_1x2( 0x00000001, 0x00000000 ),

	/* 1 in unpacked format */
	/* 496 */ POS, 0001, DATA_2x2( 0x00000000, 0x80000000, 0x00000000, 0x00000000 ),

	/* Constant structure for exp10 based evaluations */

	/* High digits of ln10/ln2, ln2/ln10 and binary exponent of ln2/ln10 */
	/* 520 */ DATA_1x2( 0xcd1b8afe, 0xd49a784b ),
	/* 528 */ DATA_1x2( 0xfbcff799, 0x9a209a84 ),
	/* 536 */ DATA_1x2( 0x000000-1, 0x00000000 ),

	/* ln2_ov_ln10_lo = ln2 - ln2_ov_ln10__hi in unpacked form */
	/* 544 */ NEG, 0-66, DATA_2x2( 0xe906dd0f, 0xe0ed4ca7, 0x785c196c, 0xb2a59e75 ),

	/* Polynomial degree */
	/* 568 */ DATA_1x2( 0x00000016, 0x00000000 ),

	/* Fixed point coefficients for exp10 evaluation */
	/* 576 */ DATA_4( 0xe12a5f3d, 0xaa326d76, 0x0005d18c, 0x00000000 ),
	/* 592 */ DATA_4( 0x6a135c14, 0xbb46d2d7, 0x0037bd19, 0x00000000 ),
	/* 608 */ DATA_4( 0x74d6a84b, 0x2188762e, 0x01fba820, 0x00000000 ),
	/* 624 */ DATA_4( 0xe5e25723, 0x10a5eeba, 0x11396f18, 0x00000000 ),
	/* 640 */ DATA_4( 0x246ea126, 0xb3fcd05a, 0x8e20e630, 0x00000000 ),
	/* 656 */ DATA_4( 0x20dd37fd, 0x11f8f23a, 0x570fb29c, 0x00000004 ),
	/* 672 */ DATA_4( 0x64bf3431, 0x167b5d1d, 0x0af8fbff, 0x00000020 ),
	/* 688 */ DATA_4( 0x854435f8, 0xb407c79f, 0xa8177bc6, 0x000000de ),
	/* 704 */ DATA_4( 0x0616e83b, 0xaef77a1b, 0x7a612e29, 0x000005aa ),
	/* 720 */ DATA_4( 0xd3c5fba9, 0x119b2348, 0x15a5882e, 0x00002273 ),
	/* 736 */ DATA_4( 0x1e07d507, 0x20d8613a, 0x096fc05f, 0x0000c27f ),
	/* 752 */ DATA_4( 0x3dd8f81c, 0x7f472bc7, 0xabb213ac, 0x0003f59f ),
	/* 768 */ DATA_4( 0x91a76481, 0x674c9f45, 0xb2d182af, 0x0012ea52 ),
	/* 784 */ DATA_4( 0x893bb4f4, 0xc9822f93, 0x1764f507, 0x005225f1 ),
	/* 800 */ DATA_4( 0xf92f4908, 0xf088ae28, 0x5fdaa5cd, 0x014116b0 ),
	/* 816 */ DATA_4( 0xaa4224b1, 0xc160bba8, 0x0ccea1ac, 0x045b937f ),
	/* 832 */ DATA_4( 0x6ebee310, 0xd9f3dcd3, 0x23e45aeb, 0x0d3f6b84 ),
	/* 848 */ DATA_4( 0x9124b3bc, 0x5c654225, 0x3a9aec44, 0x22853ffa ),
	/* 864 */ DATA_4( 0xd9f90d3b, 0xea51f65e, 0xf6631131, 0x4af5d827 ),
	/* 880 */ DATA_4( 0x0d46ba57, 0x6a4f9d82, 0xf1652304, 0x82382c8e ),
	/* 896 */ DATA_4( 0x2d65a6ec, 0x80a99ce5, 0xe753443a, 0xa9a92639 ),
	/* 912 */ DATA_4( 0x82d30a2c, 0xea56d62b, 0xaaa8ac16, 0x935d8ddd ),
	/* 928 */ DATA_4( 0x00000000, 0x00000000, 0x00000000, 0x40000000 ),
	/* 944 */ DATA_1x2( 0x00000002, 0x00000000 ),

	/* Fixed point coefficients for sinh/cosh evaluation */
	/* 952 */ DATA_2x2( 0x00000000, 0x00000000, 0x00000000, 0x00000000 ),
	/* 968 */ DATA_4( 0x84e45693, 0x2e4690eb, 0x00000000, 0x00000000 ),
	/* 984 */ DATA_4( 0x12ffd219, 0xd268b21c, 0x0000004b, 0x00000000 ),
	/* 1000 */ DATA_4( 0x45bbf199, 0x1dc17873, 0x0000654b, 0x00000000 ),
	/* 1016 */ DATA_4( 0xde16535a, 0x9ccece4d, 0x006b9fcf, 0x00000000 ),
	/* 1032 */ DATA_4( 0x9e5e08b2, 0xa1b4271d, 0x5849184e, 0x00000000 ),
	/* 1048 */ DATA_4( 0x391817aa, 0x89c71fc2, 0xcc8acfea, 0x00000035 ),
	/* 1064 */ DATA_4( 0x19c8d92f, 0x338faac2, 0xe3a556c7, 0x0000171d ),
	/* 1080 */ DATA_4( 0x66ce9bd9, 0x80680680, 0x06806806, 0x00068068 ),
	/* 1096 */ DATA_4( 0x11137719, 0x11111111, 0x11111111, 0x01111111 ),
	/* 1112 */ DATA_4( 0x5555537e, 0x55555555, 0x55555555, 0x15555555 ),
	/* 1128 */ DATA_4( 0x00000000, 0x00000000, 0x00000000, 0x80000000 ),
	/* 1144 */ DATA_1x2( 0x00000001, 0x00000000 ),
	/* 1152 */ DATA_4( 0xab2871a0, 0x021a7acf, 0x00000000, 0x00000000 ),
	/* 1168 */ DATA_4( 0x72a41925, 0xca853bed, 0x00000003, 0x00000000 ),
	/* 1184 */ DATA_4( 0xf7b71018, 0x9e18f89a, 0x000005a0, 0x00000000 ),
	/* 1200 */ DATA_4( 0x5c564d82, 0xf9ccecdb, 0x0006b9fc, 0x00000000 ),
	/* 1216 */ DATA_4( 0xee64c398, 0x301f275e, 0x064e5d2a, 0x00000000 ),
	/* 1232 */ DATA_4( 0x108a94fe, 0x3625ed50, 0x7bb63bfe, 0x00000004 ),
	/* 1248 */ DATA_4( 0x376e0580, 0xeb8e5de0, 0xc9f6ef13, 0x0000024f ),
	/* 1264 */ DATA_4( 0x0ccc1e48, 0xd00d00d0, 0x00d00d00, 0x0000d00d ),
	/* 1280 */ DATA_4( 0xd82e2a61, 0x82d82d82, 0x2d82d82d, 0x002d82d8 ),
	/* 1296 */ DATA_4( 0x55555442, 0x55555555, 0x55555555, 0x05555555 ),
	/* 1312 */ DATA_4( 0x00000001, 0x00000000, 0x00000000, 0x40000000 ),
	/* 1328 */ DATA_4( 0x00000000, 0x00000000, 0x00000000, 0x80000000 ),
	/* 1344 */ DATA_1x2( 0x00000001, 0x00000000 ),
	};

#define	EXP_CLASS_TO_ACTION_MAP		((U_WORD const *) ((char *) TABLE_NAME + 0))
#define	EXPM1_CLASS_TO_ACTION_MAP	((U_WORD const *) ((char *) TABLE_NAME + 8))
#define	SINH_CLASS_TO_ACTION_MAP	((U_WORD const *) ((char *) TABLE_NAME + 16))
#define	COSH_CLASS_TO_ACTION_MAP	((U_WORD const *) ((char *) TABLE_NAME + 24))
#define	TANH_CLASS_TO_ACTION_MAP	((U_WORD const *) ((char *) TABLE_NAME + 32))
#define	EXP_CONSTANT_TABLE_ADDRESS	((UX_FRACTION_DIGIT_TYPE *) ((char *) TABLE_NAME + 64))
#define EXP_DEGREE_INDEX		6
#define EXP_COEF_INDEX			7
#define	UX_ONE				((UX_FLOAT *) ((char *) TABLE_NAME + 496))
#define	EXP10_CONSTANT_TABLE_ADDRESS	((UX_FRACTION_DIGIT_TYPE *) ((char *) TABLE_NAME + 520))
#define	SINHCOSH_COEF_ARRAY		((FIXED_128 *) ((char *) TABLE_NAME + 952))
#define	SINHCOSH_COEF_ARRAY_DEGREE	(( signed __int64 ) 0x000000000000000b )
