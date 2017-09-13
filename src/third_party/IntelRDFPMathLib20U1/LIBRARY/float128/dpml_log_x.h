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

	/* log class-to-action-mapping */
	/* 000 */ DATA_1x2( 0x40e50408, 0x1e7ae40e ),
	/* 008 */ DATA_1x2( 0x00000034, 0x00000000 ),
	/* 016 */ DATA_1x2( 0x00000035, 0x00000000 ),

	/* log2 class-to-action-mapping */
	/* 024 */ DATA_1x2( 0x40e50408, 0x1e7ae40e ),
	/* 032 */ DATA_1x2( 0x00000036, 0x00000000 ),
	/* 040 */ DATA_1x2( 0x00000037, 0x00000000 ),

	/* log10 class-to-action-mapping */
	/* 048 */ DATA_1x2( 0x40e50408, 0x1e7ae40e ),
	/* 056 */ DATA_1x2( 0x00000038, 0x00000000 ),
	/* 064 */ DATA_1x2( 0x00000039, 0x00000000 ),

	/* log1p class-to-action-mapping */
	/* 072 */ DATA_1x2( 0x00e50408, 0x14104100 ),
	/* 080 */ DATA_1x2( 0x00000034, 0x00000000 ),

	/* MSD of sqrt(2) and 1/sqrt(2) (in fixed point) */
	/* 088 */ DATA_1x2( 0xf9de6484, 0xb504f333 ),
	/* 096 */ DATA_1x2( 0xfcef3242, 0x5a827999 ),

	/* Fixed point coefficients for log2 evaluation */
	/* 104 */ DATA_2x2( 0x56dac09b, 0x271eee7d, 0x2a1966ce, 0x06cc4d0d ),
	/* 120 */ DATA_2x2( 0x6f81e43d, 0x1ba3468b, 0x9caac22d, 0x05671139 ),
	/* 136 */ DATA_2x2( 0xa20f818f, 0xf7ca0b25, 0x32b2540a, 0x05f8b502 ),
	/* 152 */ DATA_2x2( 0x3f28f8fe, 0x7adfa93e, 0xcb8d055c, 0x065df4e9 ),
	/* 168 */ DATA_2x2( 0xf7891d9d, 0xce5c4ea3, 0x4c87d854, 0x06d6e780 ),
	/* 184 */ DATA_2x2( 0x9feb8d1e, 0xe820f58a, 0x5c44b19a, 0x0762f814 ),
	/* 200 */ DATA_2x2( 0xf720bb2c, 0xe8c1f4c0, 0x41dad530, 0x080766bf ),
	/* 216 */ DATA_2x2( 0x1df3812c, 0x80535f75, 0x7d59049f, 0x08cb2763 ),
	/* 232 */ DATA_2x2( 0x2c2ac1eb, 0x96e6a1d7, 0xa68ac838, 0x09b81e0f ),
	/* 248 */ DATA_2x2( 0x7df70971, 0x8c3b0c94, 0xba1f8070, 0x0adcd64d ),
	/* 264 */ DATA_2x2( 0x11d8754e, 0xa70095aa, 0x4a67ff05, 0x0c4f9d8b ),
	/* 280 */ DATA_2x2( 0x05f3cefe, 0x64f2a61e, 0x698bb00e, 0x0e347ab4 ),
	/* 296 */ DATA_2x2( 0x3936b199, 0x572dc64d, 0x94022d28, 0x10c9a849 ),
	/* 312 */ DATA_2x2( 0x8c4ac6fe, 0x6a80ddd5, 0x7c02a8f8, 0x1484b13d ),
	/* 328 */ DATA_2x2( 0xa5c4559c, 0x645c921f, 0x7aded93f, 0x1a61762a ),
	/* 344 */ DATA_2x2( 0xae4a965a, 0x594e6629, 0xdf37fcf2, 0x24eed8a1 ),
	/* 360 */ DATA_2x2( 0x785f1acb, 0x3f82aa45, 0x7407fae9, 0x3d8e13b8 ),
	/* 376 */ DATA_2x2( 0x691d3e89, 0xbe87fed0, 0x5c17f0bb, 0xb8aa3b29 ),
	/* 392 */ DATA_1x2( 0x00000002, 0x00000000 ),

	/* Unpacked constants 1, 2, log(2) and log(10) */
	/* 400 */ POS, 0001, DATA_2x2( 0x00000000, 0x80000000, 0x00000000, 0x00000000 ),
	/* 424 */ POS, 0002, DATA_2x2( 0x00000000, 0x80000000, 0x00000000, 0x00000000 ),
	/* 448 */ POS, 0000, DATA_2x2( 0xd1cf79ab, 0xb17217f7, 0x03f2f6af, 0xc9e3b398 ),
	/* 472 */ POS, 00-1, DATA_2x2( 0xfbcff798, 0x9a209a84, 0x0b7c9178, 0x8f8959ac ),
	};

#define	LOG_CLASS_TO_ACTION_MAP	((U_WORD const *) ((char *) TABLE_NAME + 0))
#define	LOG2_CLASS_TO_ACTION_MAP	((U_WORD const *) ((char *) TABLE_NAME + 24))
#define	LOG10_CLASS_TO_ACTION_MAP	((U_WORD const *) ((char *) TABLE_NAME + 48))
#define	LOG1P_CLASS_TO_ACTION_MAP	((U_WORD const *) ((char *) TABLE_NAME + 72))
#define	ONE_OVER_SQRT_2			*((UX_FRACTION_DIGIT_TYPE *) ((char *) TABLE_NAME + 88))
#define	I_SQRT_2			*((UX_FRACTION_DIGIT_TYPE *) ((char *) TABLE_NAME + 88))
#define	I_RECIP_SQRT_2			*((UX_FRACTION_DIGIT_TYPE *) ((char *) TABLE_NAME + 96))
#define	LOG2_COEF_ARRAY			((FIXED_128 *) ((char *) TABLE_NAME + 104))
#define	LOG2_COEF_ARRAY_DEGREE		(( signed __int64 ) 0x0000000000000011 )
#define	UX_ONE	((UX_FLOAT *) ((char *) TABLE_NAME + 400))
#define	UX_TWO	((UX_FLOAT *) ((char *) TABLE_NAME + 424))
#define	LN_2	((UX_FLOAT *) ((char *) TABLE_NAME + 448))
#define	LOG10_2	((UX_FLOAT *) ((char *) TABLE_NAME + 472))
