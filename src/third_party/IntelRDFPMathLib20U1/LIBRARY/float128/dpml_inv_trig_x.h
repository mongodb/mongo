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

	/* asin class-to-action-mapping */
	/* 000 */ DATA_1x2( 0x00e79408, 0x14104100 ),
	/* 008 */ DATA_1x2( 0x00000003, 0x00000000 ),

	/* acos class-to-action-mapping */
	/* 016 */ DATA_1x2( 0x00e79408, 0x14924920 ),
	/* 024 */ DATA_1x2( 0x00000000, 0x00000000 ),
	/* 032 */ DATA_1x2( 0x00000004, 0x00000000 ),

	/* asind class-to-action-mapping */
	/* 040 */ DATA_1x2( 0x00e79408, 0x14100000 ),
	/* 048 */ DATA_1x2( 0x00000004, 0x00000000 ),

	/* acosd class-to-action-mapping */
	/* 056 */ DATA_1x2( 0x00e79408, 0x14924920 ),
	/* 064 */ DATA_1x2( 0x00000001, 0x00000000 ),
	/* 072 */ DATA_1x2( 0x00000007, 0x00000000 ),

	/* atan class-to-action-mapping */
	/* 080 */ DATA_1x2( 0x00651408, 0x14104100 ),
	/* 088 */ DATA_1x2( 0x00000004, 0x00000000 ),

	/* atand class-to-action-mapping */
	/* 096 */ DATA_1x2( 0x00651408, 0x14100000 ),
	/* 104 */ DATA_1x2( 0x00000007, 0x00000000 ),

	/* atan2(y,x) class-to-action-mapping */
	/* 112 */ DATA_1x2( 0x00000408, 0x80000000 ),
	/* 120 */ DATA_1x2( 0x33332200, 0x00000054 ),
	/* 128 */ DATA_1x2( 0x2caea449, 0x4b2cb2cb ),
	/* 136 */ DATA_1x2( 0x41bad449, 0x3b2c0410 ),
	/* 144 */ DATA_1x2( 0x90b90449, 0x2591b10b ),
	/* 152 */ DATA_1x2( 0x90b90449, 0x1b90b90b ),
	/* 160 */ DATA_1x2( 0x00000000, 0x00000000 ),
	/* 168 */ DATA_1x2( 0x00000005, 0x00000000 ),
	/* 176 */ DATA_1x2( 0x00000006, 0x00000000 ),
	/* 184 */ DATA_1x2( 0x00000004, 0x00000000 ),
	/* 192 */ DATA_1x2( 0x00000000, 0x00000000 ),
	/* 200 */ DATA_1x2( 0x00000003, 0x00000000 ),

	/* atan2d(y,x) class-to-action-mapping */
	/* 208 */ DATA_1x2( 0x00000408, 0x70000000 ),
	/* 216 */ DATA_1x2( 0x54543200, 0x00000076 ),
	/* 224 */ DATA_1x2( 0x14eba449, 0x65145145 ),
	/* 232 */ DATA_1x2( 0x1ceba449, 0x571c71c7 ),
	/* 240 */ DATA_1x2( 0x41595449, 0x45140410 ),
	/* 248 */ DATA_1x2( 0x4179d449, 0x371c0410 ),
	/* 256 */ DATA_1x2( 0x90590449, 0x2efb5105 ),
	/* 264 */ DATA_1x2( 0x90790449, 0x17bb7907 ),
	/* 272 */ DATA_1x2( 0x00000000, 0x00000000 ),
	/* 280 */ DATA_1x2( 0x0000000c, 0x00000000 ),
	/* 288 */ DATA_1x2( 0x0000000b, 0x00000000 ),
	/* 296 */ DATA_1x2( 0x00000007, 0x00000000 ),
	/* 304 */ DATA_1x2( 0x00000000, 0x00000000 ),
	/* 312 */ DATA_1x2( 0x00000008, 0x00000000 ),

	/* 0, pi/4, pi/2, 3pi/4, pi in unpacked format */
	/* 320 */ POS, -131072, DATA_2x2( 0x00000000, 0x00000000, 0x00000000, 0x00000000 ),
	/* 344 */ POS, 0000, DATA_2x2( 0x2168c234, 0xc90fdaa2, 0x80dc1cd1, 0xc4c6628b ),
	/* 368 */ POS, 0001, DATA_2x2( 0x2168c234, 0xc90fdaa2, 0x80dc1cd1, 0xc4c6628b ),
	/* 392 */ POS, 0002, DATA_2x2( 0x990e91a7, 0x96cbe3f9, 0xa0a5159c, 0x9394c9e8 ),
	/* 416 */ POS, 0002, DATA_2x2( 0x2168c234, 0xc90fdaa2, 0x80dc1cd1, 0xc4c6628b ),

	/* 1, 180/pi, 1/3 in unpacked format */
	/* 440 */ POS, 0001, DATA_2x2( 0x00000000, 0x80000000, 0x00000000, 0x00000000 ),
	/* 464 */ POS, 0006, DATA_2x2( 0x1e0fbdc3, 0xe52ee0d3, 0x40d257d7, 0x0a97537f ),
	/* 488 */ POS, 00-1, DATA_2x2( 0xaaaaaaaa, 0xaaaaaaaa, 0xaaaaaaaa, 0xaaaaaaaa ),

	/* Fixed point coefficients for atan evaluation */
	/* 512 */ DATA_2x2( 0x00000000, 0x00000000, 0x00000000, 0x00000000 ),
	/* 528 */ DATA_4( 0x17b033de, 0x9b21db18, 0x036a28b8, 0x00000000 ),
	/* 544 */ DATA_4( 0xbbb9e258, 0x7af48d0c, 0xa9d8aeac, 0x00000004 ),
	/* 560 */ DATA_4( 0xb5f5477a, 0x710b595c, 0x01b80364, 0x000001d6 ),
	/* 576 */ DATA_4( 0xbdc83502, 0x82ff5ad5, 0xdb2203cd, 0x00005360 ),
	/* 592 */ DATA_4( 0xb3ace8e0, 0xa46ea356, 0x5271c15d, 0x000803a1 ),
	/* 608 */ DATA_4( 0x47fd897a, 0x511728bc, 0xd71df9b4, 0x00752012 ),
	/* 624 */ DATA_4( 0x38e6ccd7, 0xb0eebd1d, 0x0c0e0aef, 0x04261aad ),
	/* 640 */ DATA_4( 0x2223a644, 0x715215ee, 0x069e5e06, 0x178d58e7 ),
	/* 656 */ DATA_4( 0xdaa31b09, 0x1a5b5968, 0x09775969, 0x515e68b9 ),
	/* 672 */ DATA_4( 0x68db7ef7, 0xa67de44d, 0xb65e0e57, 0x9c53edb8 ),
	/* 688 */ DATA_4( 0x00000000, 0x00000000, 0x00000000, 0x80000000 ),
	/* 704 */ DATA_1x2( 0x00000000, 0x00000000 ),
	/* 712 */ DATA_4( 0xa07a791a, 0x753b0a86, 0x00060285, 0x00000000 ),
	/* 728 */ DATA_4( 0xf41004bb, 0xb62b5e42, 0x1a6a8474, 0x00000000 ),
	/* 744 */ DATA_4( 0x4e1e2dad, 0x6af09bc2, 0xcf340cf3, 0x00000012 ),
	/* 760 */ DATA_4( 0x106af1a7, 0x49426ee8, 0xbce40e29, 0x00000523 ),
	/* 776 */ DATA_4( 0x6ccae258, 0xd77ad56c, 0x388d7935, 0x0000b5f6 ),
	/* 792 */ DATA_4( 0xa5d93fd4, 0x95aa5864, 0x505d9ab5, 0x000e856c ),
	/* 808 */ DATA_4( 0x49a8f559, 0xf9512f86, 0xc988c73a, 0x00b744f2 ),
	/* 824 */ DATA_4( 0x4ddd2493, 0x247ce9cc, 0x95031b41, 0x05c21354 ),
	/* 840 */ DATA_4( 0xf40a72fc, 0xc6922892, 0xde3bc4f4, 0x1d8eb88d ),
	/* 856 */ DATA_4( 0x97ff604a, 0x0785210e, 0x629e79e5, 0x5daf5bd2 ),
	/* 872 */ DATA_4( 0x13862999, 0x51288ef8, 0x6108b902, 0xa6fe9863 ),
	/* 888 */ DATA_4( 0x00000000, 0x00000000, 0x00000000, 0x80000000 ),
	/* 904 */ DATA_1x2( 0x00000001, 0x00000000 ),

	/* Fixed point coefficients for asin evaluation */
	/* 912 */ DATA_4( 0x285a9adb, 0xbc844bd3, 0x0018a298, 0x00000000 ),
	/* 928 */ DATA_4( 0xff2fc62e, 0x24543a40, 0x4b712f53, 0x00000000 ),
	/* 944 */ DATA_4( 0x4db90d47, 0x2553512c, 0x42b22a11, 0x0000002b ),
	/* 960 */ DATA_4( 0x9560de1d, 0x4670c8ac, 0x39855097, 0x00000a02 ),
	/* 976 */ DATA_4( 0x53ef4cb8, 0x022dda0e, 0xbd533bc9, 0x00013575 ),
	/* 992 */ DATA_4( 0x688e8800, 0xafc38a68, 0xece50095, 0x00160d59 ),
	/* 1008 */ DATA_4( 0xa5f3e527, 0x6123e0ee, 0x91e17495, 0x00fcc7ee ),
	/* 1024 */ DATA_4( 0xffd8cc09, 0xfa699043, 0x5647265e, 0x074facfd ),
	/* 1040 */ DATA_4( 0xdf4a1e6d, 0x7dd602b0, 0xe68005c2, 0x22edbcfc ),
	/* 1056 */ DATA_4( 0xd688d50a, 0xa938fa69, 0x129b3e51, 0x67f826ed ),
	/* 1072 */ DATA_4( 0x3865c5f2, 0xff93b5cb, 0xf163dd08, 0xaf5c9b73 ),
	/* 1088 */ DATA_4( 0x00000000, 0x00000000, 0x00000000, 0x80000000 ),
	/* 1104 */ DATA_1x2( 0x00000000, 0x00000000 ),
	/* 1112 */ DATA_4( 0x152467c1, 0xede27d48, 0x00882734, 0x00000000 ),
	/* 1128 */ DATA_4( 0xbe470341, 0x1d75e618, 0xca5275d0, 0x00000000 ),
	/* 1144 */ DATA_4( 0xc7d6f6e2, 0x001c0ab3, 0x9cc8243b, 0x00000055 ),
	/* 1160 */ DATA_4( 0xea1af30d, 0x36449091, 0x0f45b29d, 0x00001083 ),
	/* 1176 */ DATA_4( 0x4850f9dd, 0x9692608b, 0x726a35f0, 0x0001c28a ),
	/* 1192 */ DATA_4( 0x50b194c6, 0x755313b9, 0xaa0112de, 0x001d43c1 ),
	/* 1208 */ DATA_4( 0xd5bd1184, 0x555ff65f, 0x0042983f, 0x01382000 ),
	/* 1224 */ DATA_4( 0x044ad977, 0xa448034f, 0x9a59728a, 0x0884c109 ),
	/* 1240 */ DATA_4( 0x5361e105, 0x0743cfa3, 0xc3ec7bec, 0x26caad31 ),
	/* 1256 */ DATA_4( 0x42d6fdeb, 0x5329169c, 0xdbf406d1, 0x6ee5f75b ),
	/* 1272 */ DATA_4( 0x8dbb1b38, 0x54e90b20, 0x46b9325e, 0xb4b1f0c9 ),
	/* 1288 */ DATA_4( 0x00000000, 0x00000000, 0x00000000, 0x80000000 ),
	/* 1304 */ DATA_1x2( 0x00000001, 0x00000000 ),
	};

#define	ASIN_CLASS_TO_ACTION_MAP	((U_WORD const *) ((char *) TABLE_NAME + 0))
#define	ACOS_CLASS_TO_ACTION_MAP	((U_WORD const *) ((char *) TABLE_NAME + 16))
#define	ASIND_CLASS_TO_ACTION_MAP	((U_WORD const *) ((char *) TABLE_NAME + 40))
#define	ACOSD_CLASS_TO_ACTION_MAP	((U_WORD const *) ((char *) TABLE_NAME + 56))
#define	ATAN_CLASS_TO_ACTION_MAP	((U_WORD const *) ((char *) TABLE_NAME + 80))
#define	ATAND_CLASS_TO_ACTION_MAP	((U_WORD const *) ((char *) TABLE_NAME + 96))
#define	ATAN2_CLASS_TO_ACTION_MAP	((U_WORD const *) ((char *) TABLE_NAME + 112))
#define	ATAND2_CLASS_TO_ACTION_MAP	((U_WORD const *) ((char *) TABLE_NAME + 208))
#define	INV_TRIG_CONS_BASE		((UX_FLOAT *) ((char *) TABLE_NAME + 320))
#define	UX_ZERO					((UX_FLOAT *) ((char *) TABLE_NAME + 320))
#define UX_ZERO_INDEX			0
#define UX_PI_OVER_4_INDEX		24
#define UX_PI_OVER_2_INDEX		48
#define UX_THREE_QUARTERS_PI_INDEX	72
#define UX_PI_INDEX			96
#define	UX_ONE				((UX_FLOAT *) ((char *) TABLE_NAME + 440))
#define	UX_RAD_TO_DEG			((UX_FLOAT *) ((char *) TABLE_NAME + 464))
#define	UX_ONE_THIRD			((UX_FLOAT *) ((char *) TABLE_NAME + 488))
#define	ATAN_COEF_ARRAY			((FIXED_128 *) ((char *) TABLE_NAME + 512))
#define	ATAN_COEF_ARRAY_DEGREE		(( signed long long ) 0x000000000000000b )
#define	ASIN_COEF_ARRAY			((FIXED_128 *) ((char *) TABLE_NAME + 912))
#define	ASIN_COEF_ARRAY_DEGREE		(( signed long long ) 0x000000000000000b )
