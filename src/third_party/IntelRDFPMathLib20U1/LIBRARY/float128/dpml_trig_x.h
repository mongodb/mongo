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

	/* sin class-to-action-mapping */
	/* 000 */ DATA_1x2( 0x00eba408, 0x64104100 ),

	/* cos class-to-action-mapping */
	/* 008 */ DATA_1x2( 0x00efb408, 0x54514510 ),

	/* sincos class-to-action-mapping */
	/* 016 */ DATA_1x2( 0x00f3c408, 0x44104100 ),

	/* sind class-to-action-mapping */
	/* 024 */ DATA_1x2( 0x00f7d408, 0x34100000 ),

	/* cosd class-to-action-mapping */
	/* 032 */ DATA_1x2( 0x00fbe408, 0x24514510 ),

	/* sincosd class-to-action-mapping */
	/* 040 */ DATA_1x2( 0x00fff408, 0x14100000 ),

	/* Data for the above mappings */
	/* 048 */ DATA_1x2( 0x00000001, 0x00000000 ),
	/* 056 */ DATA_1x2( 0x0000005e, 0x00000000 ),
	/* 064 */ DATA_1x2( 0x00000011, 0x00000000 ),
	/* 072 */ DATA_1x2( 0x0000005f, 0x00000000 ),
	/* 080 */ DATA_1x2( 0x00000062, 0x00000000 ),
	/* 088 */ DATA_1x2( 0x00000012, 0x00000000 ),
	/* 096 */ DATA_1x2( 0x00000060, 0x00000000 ),

	/* tan class-to-action-mapping */
	/* 104 */ DATA_1x2( 0x00e79408, 0x14104100 ),
	/* 112 */ DATA_1x2( 0x0000006a, 0x00000000 ),

	/* tand class-to-action-mapping */
	/* 120 */ DATA_1x2( 0x00e79408, 0x14100000 ),
	/* 128 */ DATA_1x2( 0x0000006d, 0x00000000 ),

	/* cot class-to-action-mapping */
	/* 136 */ DATA_1x2( 0x00e79408, 0x1efa0000 ),
	/* 144 */ DATA_1x2( 0x00000017, 0x00000000 ),
	/* 152 */ DATA_1x2( 0x00000018, 0x00000000 ),
	/* 160 */ DATA_1x2( 0x00000019, 0x00000000 ),

	/* cotd class-to-action-mapping */
	/* 168 */ DATA_1x2( 0x00e79408, 0x1f7cefa0 ),
	/* 176 */ DATA_1x2( 0x0000001d, 0x00000000 ),
	/* 184 */ DATA_1x2( 0x0000001b, 0x00000000 ),
	/* 192 */ DATA_1x2( 0x0000001c, 0x00000000 ),
	/* 200 */ DATA_1x2( 0x0000001e, 0x00000000 ),
	/* 208 */ DATA_1x2( 0x0000001f, 0x00000000 ),

	/* Unpacked constants pi/180 */
	/* 216 */ POS, 00-5, DATA_2x2( 0x94e9c8ae, 0x8efa3512, 0x9485c4d9, 0x0ec5f66e ),

	/* Packed constants 1 */
	/* 240 */ DATA_4( 0x00000000, 0x00000000, 0x00000000, 0x3fff0000 ),
	/* 256 */ DATA_1x2( 0x05b05b06, 0x5b05b05b ),
	/* 264 */ DATA_1x2( 0x55555556, 0x15555555 ),

	/* Fixed point coefficients for sin and cos evaluation */
	/* 272 */ DATA_4( 0x9e634562, 0x00000003, 0x00000000, 0x00000000 ),
	/* 288 */ DATA_4( 0xdce17a1d, 0x000009f9, 0x00000000, 0x00000000 ),
	/* 304 */ DATA_4( 0x083a3075, 0x001761b4, 0x00000000, 0x00000000 ),
	/* 320 */ DATA_4( 0xb1d75408, 0x2e371ded, 0x00000000, 0x00000000 ),
	/* 336 */ DATA_4( 0x013c755b, 0xd26d1a05, 0x0000004b, 0x00000000 ),
	/* 352 */ DATA_4( 0x28320429, 0x1dc0c2b5, 0x0000654b, 0x00000000 ),
	/* 368 */ DATA_4( 0x4701facd, 0x9ccee07c, 0x006b9fcf, 0x00000000 ),
	/* 384 */ DATA_4( 0x8dfbf381, 0xa1b425f2, 0x5849184e, 0x00000000 ),
	/* 400 */ DATA_4( 0x8fc76db3, 0x89c71fce, 0xcc8acfea, 0x00000035 ),
	/* 416 */ DATA_4( 0xc88e2826, 0x338faac1, 0xe3a556c7, 0x0000171d ),
	/* 432 */ DATA_4( 0x68067e8f, 0x80680680, 0x06806806, 0x00068068 ),
	/* 448 */ DATA_4( 0x11111106, 0x11111111, 0x11111111, 0x01111111 ),
	/* 464 */ DATA_4( 0x55555555, 0x55555555, 0x55555555, 0x15555555 ),
	/* 480 */ DATA_4( 0x00000000, 0x00000000, 0x00000000, 0x80000000 ),
	/* 496 */ DATA_1x2( 0x00000001, 0x00000000 ),
	/* 504 */ DATA_4( 0xa9fb87e2, 0x00000061, 0x00000000, 0x00000000 ),
	/* 520 */ DATA_4( 0x69688c7c, 0x0000f966, 0x00000000, 0x00000000 ),
	/* 536 */ DATA_4( 0x77c1de0c, 0x0219c72c, 0x00000000, 0x00000000 ),
	/* 552 */ DATA_4( 0x51903a09, 0xca85747f, 0x00000003, 0x00000000 ),
	/* 568 */ DATA_4( 0xeb393833, 0x9e18ee5e, 0x000005a0, 0x00000000 ),
	/* 584 */ DATA_4( 0x9837094c, 0xf9ccee07, 0x0006b9fc, 0x00000000 ),
	/* 600 */ DATA_4( 0x23772903, 0x301f2748, 0x064e5d2a, 0x00000000 ),
	/* 616 */ DATA_4( 0x34a72fa4, 0x3625ed51, 0x7bb63bfe, 0x00000004 ),
	/* 632 */ DATA_4( 0x2d6a41e7, 0xeb8e5de0, 0xc9f6ef13, 0x0000024f ),
	/* 648 */ DATA_4( 0x0cfbffe1, 0xd00d00d0, 0x00d00d00, 0x0000d00d ),
	/* 664 */ DATA_4( 0xd82d4910, 0x82d82d82, 0x2d82d82d, 0x002d82d8 ),
	/* 680 */ DATA_4( 0x555553eb, 0x55555555, 0x55555555, 0x05555555 ),
	/* 696 */ DATA_4( 0xfffffffc, 0xffffffff, 0xffffffff, 0x3fffffff ),
	/* 712 */ DATA_4( 0x00000000, 0x00000000, 0x00000000, 0x80000000 ),
	/* 728 */ DATA_1x2( 0x00000001, 0x00000000 ),

	/* Fixed point coefficients for tan and cot evaluation */
	/* 736 */ DATA_2x2( 0x00000000, 0x00000000, 0x00000000, 0x00000000 ),
	/* 752 */ DATA_4( 0xab86d966, 0x02e36384, 0x004583dc, 0x00000000 ),
	/* 768 */ DATA_4( 0xc57437cf, 0xfe661c77, 0xdf2fb0d7, 0x00000001 ),
	/* 784 */ DATA_4( 0x0062ee42, 0xe5f9c219, 0x46cae26e, 0x0000036f ),
	/* 800 */ DATA_4( 0xe15a162f, 0x9c878717, 0xa6fa2240, 0x000269dc ),
	/* 816 */ DATA_4( 0xe65127b0, 0xc5b462ff, 0x4c0b8a2c, 0x00b209c0 ),
	/* 832 */ DATA_4( 0x67f59a68, 0xa273c258, 0xd7a587c9, 0x12f6c9c3 ),
	/* 848 */ DATA_4( 0x00000000, 0x00000000, 0x00000000, 0x80000000 ),
	/* 864 */ DATA_1x2( 0x00000001, 0x00000000 ),
	/* 872 */ DATA_4( 0xcbfc02d6, 0x196c967a, 0x0000a9ba, 0x00000000 ),
	/* 888 */ DATA_4( 0xcf5f2fe6, 0xd03d3831, 0x0e1ae92d, 0x00000000 ),
	/* 904 */ DATA_4( 0xfd241f97, 0x40a36a10, 0x4c98a51d, 0x0000002e ),
	/* 920 */ DATA_4( 0x72e00402, 0xb12a527e, 0x85b96b4f, 0x00003380 ),
	/* 936 */ DATA_4( 0x2c15cbc5, 0xf5e92d64, 0x56378673, 0x001739c3 ),
	/* 952 */ DATA_4( 0x86202dfc, 0x7902cb9a, 0xbbbfdf42, 0x042c1f7e ),
	/* 968 */ DATA_4( 0x12a04513, 0x4d1e6d03, 0x82503274, 0x3da1746e ),
	/* 984 */ DATA_4( 0x00000000, 0x00000000, 0x00000000, 0x80000000 ),
	/* 1000 */ DATA_1x2( 0x00000001, 0x00000000 ),

	/* Unpacked value of pi/4 */
	/* 1008 */ POS, 0000, DATA_2x2( 0x2168c234, 0xc90fdaa2, 0x80dc1cd1, 0xc4c6628b ),
	};

#define	SIN_CLASS_TO_ACTION_MAP		((U_WORD const *) ((char *) TABLE_NAME + 0))
#define	COS_CLASS_TO_ACTION_MAP		((U_WORD const *) ((char *) TABLE_NAME + 8))
#define	SINCOS_CLASS_TO_ACTION_MAP	((U_WORD const *) ((char *) TABLE_NAME + 16))
#define	SIND_CLASS_TO_ACTION_MAP	((U_WORD const *) ((char *) TABLE_NAME + 24))
#define	COSD_CLASS_TO_ACTION_MAP	((U_WORD const *) ((char *) TABLE_NAME + 32))
#define	SINCOSD_CLASS_TO_ACTION_MAP	((U_WORD const *) ((char *) TABLE_NAME + 40))
#define	TAN_CLASS_TO_ACTION_MAP		((U_WORD const *) ((char *) TABLE_NAME + 104))
#define	TAND_CLASS_TO_ACTION_MAP	((U_WORD const *) ((char *) TABLE_NAME + 120))
#define	COT_CLASS_TO_ACTION_MAP		((U_WORD const *) ((char *) TABLE_NAME + 136))
#define	COTD_CLASS_TO_ACTION_MAP	((U_WORD const *) ((char *) TABLE_NAME + 168))
#define	UX_PI_OVER_180			((UX_FLOAT *) ((char *) TABLE_NAME + 216))
#define	_X_ONE				((long double *) ((char *) TABLE_NAME + 240))
#define	MSD_OF_RECIP_90			*((UX_FRACTION_DIGIT_TYPE *) ((char *) TABLE_NAME + 256))
#define	RECIP_TWELVE			*((UX_FRACTION_DIGIT_TYPE *) ((char *) TABLE_NAME + 264))
#define	SINCOS_COEF_ARRAY		((FIXED_128 *) ((char *) TABLE_NAME + 272))
#define	SINCOS_COEF_ARRAY_DEGREE	(( signed __int64 ) 0x000000000000000d )
#define	TANCOT_COEF_ARRAY		((FIXED_128 *) ((char *) TABLE_NAME + 736))
#define	TANCOT_COEF_ARRAY_DEGREE	(( signed __int64 ) 0x0000000000000007 )
#define	UX_PI_OVER_FOUR	((UX_FLOAT *) ((char *) TABLE_NAME + 1008))
