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


    static const TABLE_UNION __pow_x_table[] = { 

	/* ansi-c class-to-action-mapping */
	/* 000 */ DATA_1x2( 0x00000000, 0x10000000 ),
	/* 008 */ DATA_1x2( 0x98765432, 0x000000ba ),
	/* 016 */ DATA_1x2( 0x08208208, 0xa4d32082 ),
	/* 024 */ DATA_1x2( 0x10410410, 0x94d34104 ),
	/* 032 */ DATA_1x2( 0x94494449, 0x84d34944 ),
	/* 040 */ DATA_1x2( 0x3d4bd449, 0x74d34bd0 ),
	/* 048 */ DATA_1x2( 0x00000449, 0x64d34d30 ),
	/* 056 */ DATA_1x2( 0x00000449, 0x54d3f7d0 ),
	/* 064 */ DATA_1x2( 0x00512449, 0x44d34d30 ),
	/* 072 */ DATA_1x2( 0x00f52449, 0x34d3f7d0 ),
	/* 080 */ DATA_1x2( 0x92512449, 0x24d3f92f ),
	/* 088 */ DATA_1x2( 0x40f92449, 0x14d3f92f ),

	/* data for the above mapping */
	/* 096 */ DATA_1x2( 0x00000000, 0x00000000 ),
	/* 104 */ DATA_1x2( 0x00000000, 0x00000000 ),
	/* 112 */ DATA_1x2( 0x00000001, 0x00000000 ),
	/* 120 */ DATA_1x2( 0x00000009, 0x00000000 ),
	/* 128 */ DATA_1x2( 0x00000046, 0x00000000 ),
	/* 136 */ DATA_1x2( 0x00000047, 0x00000000 ),

	/* fortran class-to-action-mapping */
	/* 144 */ DATA_1x2( 0x00000408, 0x00000000 ),
	/* 152 */ DATA_1x2( 0x25242300, 0x00000076 ),
	/* 160 */ DATA_1x2( 0x7df7d449, 0x6f7df7df ),
	/* 168 */ DATA_1x2( 0x94494449, 0x5f7d4944 ),
	/* 176 */ DATA_1x2( 0x00000449, 0x44d34d30 ),
	/* 184 */ DATA_1x2( 0x00512449, 0x34d34d30 ),
	/* 192 */ DATA_1x2( 0x92512449, 0x2f7df92f ),
	/* 200 */ DATA_1x2( 0x5af52449, 0x1f7df52f ),

	/* data for the above mapping */
	/* 208 */ DATA_1x2( 0x00000000, 0x00000000 ),
	/* 216 */ DATA_1x2( 0x00000000, 0x00000000 ),
	/* 224 */ DATA_1x2( 0x00000001, 0x00000000 ),
	/* 232 */ DATA_1x2( 0x00000009, 0x00000000 ),
	/* 240 */ DATA_1x2( 0x00000046, 0x00000000 ),
	/* 248 */ DATA_1x2( 0x00000047, 0x00000000 ),

	/* exp2 class-to-action-mapping */
	/* 256 */ DATA_1x2( 0x00ebb408, 0x14514510 ),

	/* Data for the class to action mappings */
	/* 264 */ DATA_1x2( 0x00000001, 0x00000000 ),
	/* 272 */ DATA_1x2( 0x00000091, 0x00000000 ),
	/* 280 */ DATA_1x2( 0x00000090, 0x00000000 ),

	/* high word of sqrt(2) and ln2 */
	/* 288 */ DATA_1x2( 0xf9de6484, 0xb504f333 ),
	/* 296 */ DATA_1x2( 0xd1cf79ab, 0xb17217f7 ),

	/* 1, 1/ln2 and log2_lo/ln2 in unpacked format */
	/* 304 */ POS, 0001, DATA_2x2( 0x00000000, 0x80000000, 0x00000000, 0x00000000 ),
	/* 328 */ POS, 0002, DATA_2x2( 0x5c17f0bb, 0xb8aa3b29, 0x691d3e88, 0xbe87fed0 ),
	/* 352 */ POS, 0-63, DATA_2x2( 0x9e45c2c0, 0x91a1e8f2, 0x505ad73a, 0xb3dc7e64 ),

	/* Fixed point coefficients for log2 evaluation */
	/* 376 */ DATA_4( 0x9c3d3269, 0x846f0cdb, 0x00000116, 0x00000000 ),
	/* 392 */ DATA_4( 0x54ec30fa, 0x0ed54db2, 0x0000072b, 0x00000000 ),
	/* 408 */ DATA_4( 0xdfe33a4b, 0xc9fa6284, 0x000041bc, 0x00000000 ),
	/* 424 */ DATA_4( 0xfc256daa, 0x99f674de, 0x00024519, 0x00000000 ),
	/* 440 */ DATA_4( 0xfbd9eaf3, 0xd7b95b07, 0x00143436, 0x00000000 ),
	/* 456 */ DATA_4( 0x7dba85bc, 0xa13ba581, 0x00b4aaab, 0x00000000 ),
	/* 472 */ DATA_4( 0x19ecd788, 0xb7a943e6, 0x06587797, 0x00000000 ),
	/* 488 */ DATA_4( 0x0e2310dc, 0x50fcda14, 0x396c809c, 0x00000000 ),
	/* 504 */ DATA_4( 0xfc4954a4, 0x20dc94f8, 0x0b9cbe4a, 0x00000002 ),
	/* 520 */ DATA_4( 0xa00351a9, 0x726ae205, 0xd2328609, 0x00000012 ),
	/* 536 */ DATA_4( 0x2e72008c, 0x746df395, 0x210e17e1, 0x000000af ),
	/* 552 */ DATA_4( 0xfb43dec4, 0x13599009, 0x700e7651, 0x00000674 ),
	/* 568 */ DATA_4( 0xf62944cf, 0xd038e4ea, 0xd7c437db, 0x00003e01 ),
	/* 584 */ DATA_4( 0x28865f8f, 0xaee9df3b, 0xe54d1542, 0x00026219 ),
	/* 600 */ DATA_4( 0x7a082390, 0x5d557e39, 0x56fd52e7, 0x00184022 ),
	/* 616 */ DATA_4( 0x7aa6f59b, 0x2932877a, 0x87a04e84, 0x01039501 ),
	/* 632 */ DATA_4( 0x8c267804, 0x47a3ed39, 0xd62f144c, 0x0bd19a0f ),
	/* 648 */ DATA_4( 0xdd11fee3, 0x5079024e, 0x641da382, 0xa3fe9ffd ),
	/* 664 */ DATA_1x2( 0x000000-4, 0x00000000 ),

	/* Fixed point coefficients for 2^h evaluation */
	/* 672 */ DATA_4( 0x151832ab, 0x00002b4c, 0x00000000, 0x00000000 ),
	/* 688 */ DATA_4( 0x42ddb787, 0x000561d1, 0x00000000, 0x00000000 ),
	/* 704 */ DATA_4( 0xd1c367c8, 0x00a2d67f, 0x00000000, 0x00000000 ),
	/* 720 */ DATA_4( 0x57182134, 0x125a7da0, 0x00000000, 0x00000000 ),
	/* 736 */ DATA_4( 0xba507c6d, 0xf7176bc7, 0x00000001, 0x00000000 ),
	/* 752 */ DATA_4( 0x8fac4875, 0x088968a2, 0x00000033, 0x00000000 ),
	/* 768 */ DATA_4( 0x115b54c3, 0xa26b9e85, 0x000004e3, 0x00000000 ),
	/* 784 */ DATA_4( 0xd6ab2988, 0xa10ec0e8, 0x000070db, 0x00000000 ),
	/* 800 */ DATA_4( 0x3fcb6035, 0x26ac3c53, 0x00098a4b, 0x00000000 ),
	/* 816 */ DATA_4( 0x8532c06f, 0x8b3687ce, 0x00c0b0c9, 0x00000000 ),
	/* 832 */ DATA_4( 0x8e3a0b6b, 0x7e14c2f1, 0x0e1deb28, 0x00000000 ),
	/* 848 */ DATA_4( 0x57ee4711, 0x8dd92607, 0xf465639a, 0x00000000 ),
	/* 864 */ DATA_4( 0xcc717d30, 0xc764fb7e, 0x267a8ac5, 0x0000000f ),
	/* 880 */ DATA_4( 0x8c4cb47e, 0x3e1ed253, 0x929e9caf, 0x000000da ),
	/* 896 */ DATA_4( 0x3074cb1a, 0x11fec7ff, 0x0111d2e4, 0x00000b16 ),
	/* 912 */ DATA_4( 0x31ee7ad0, 0x1a1ac547, 0xff1622c3, 0x00007ff2 ),
	/* 928 */ DATA_4( 0x61aa9a77, 0xdbd2c2a2, 0x4be1b1e1, 0x00050c24 ),
	/* 944 */ DATA_4( 0x4a2a80b1, 0x20e2fed3, 0xcf14ce62, 0x002bb0ff ),
	/* 960 */ DATA_4( 0x53eeb456, 0x9ccbbe0b, 0xfba4e772, 0x013b2ab6 ),
	/* 976 */ DATA_4( 0xccaf4903, 0xcce9d8ae, 0xc1282fe2, 0x071ac235 ),
	/* 992 */ DATA_4( 0xc9735fbe, 0x6f16b06e, 0x82c58ea8, 0x1ebfbdff ),
	/* 1008 */ DATA_4( 0x01f97b59, 0xe4f1d9cc, 0xe8e7bcd5, 0x58b90bfb ),
	/* 1024 */ DATA_4( 0x00000000, 0x00000000, 0x00000000, 0x80000000 ),
	/* 1040 */ DATA_1x2( 0x00000001, 0x00000000 ),
	};

#define	ANSI_C_POW_CLASS_TO_ACTION_MAP	((U_WORD const *) ((char *) __pow_x_table + 0))
#define	FORTRAN_POW_CLASS_TO_ACTION_MAP	((U_WORD const *) ((char *) __pow_x_table + 144))
#define	EXP2_CLASS_TO_ACTION_MAP		((U_WORD const *) ((char *) __pow_x_table + 256))
#define	ONE_OVER_SQRT_2			*((UX_FRACTION_DIGIT_TYPE *) ((char *) __pow_x_table + 288))
#define	MSD_OF_LN2			*((UX_FRACTION_DIGIT_TYPE *) ((char *) __pow_x_table + 296))
#define	UX_ONE				((UX_FLOAT *) ((char *) __pow_x_table + 304))
#define	UX_TWO_OVER_LN2			((UX_FLOAT *) ((char *) __pow_x_table + 328))
#define	UX_LN2_LO_OVER_LN2		((UX_FLOAT *) ((char *) __pow_x_table + 352))
#define	POW_LOG2_COEF_ARRAY		((FIXED_128 *) ((char *) __pow_x_table + 376))
#define	POW_LOG2_COEF_ARRAY_DEGREE	(( signed __int64 ) 0x0000000000000011 )
#define	POW2_COEF_ARRAY			((FIXED_128 *) ((char *) __pow_x_table + 672))
#define	POW2_COEF_ARRAY_DEGREE		(( signed __int64 ) 0x0000000000000016 )
