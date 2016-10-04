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

	/* erf class-to-action-mapping */
	/* 000 */ DATA_1x2( 0x00651408, 0x14100000 ),
	/* 008 */ DATA_1x2( 0x00000001, 0x00000000 ),

	/* erfc class-to-action-mapping */
	/* 016 */ DATA_1x2( 0x004d1408, 0x14924920 ),
	/* 024 */ DATA_1x2( 0x00000000, 0x00000000 ),
	/* 032 */ DATA_1x2( 0x00000001, 0x00000000 ),
	/* 040 */ DATA_1x2( 0x00000002, 0x00000000 ),

	/* unpacked 0 constant */
	/* 048 */ POS, -131072, DATA_2x2( 0x00000000, 0x00000000, 0x00000000, 0x00000000 ),

	/* Fixed point coefficients for erf(x) evaluation */
	/* 072 */ DATA_4( 0x690507d1, 0xeef69e7e, 0x0009a2c0, 0x00000000 ),
	/* 088 */ DATA_4( 0xd62f6ded, 0x5454a9ce, 0x04d23552, 0x00000000 ),
	/* 104 */ DATA_4( 0x651e03c7, 0x75c1acc3, 0x74cae68d, 0x00000002 ),
	/* 120 */ DATA_4( 0x45cd8894, 0x118d2231, 0x7e1a5137, 0x00000043 ),
	/* 136 */ DATA_4( 0x581434c9, 0x446de774, 0xb054a6dc, 0x00000cb0 ),
	/* 152 */ DATA_4( 0x8c034d61, 0x23f4d991, 0xed51c1a6, 0x0000af42 ),
	/* 168 */ DATA_4( 0xaaa84f23, 0x05d22b6b, 0x2d422b00, 0x001193ae ),
	/* 184 */ DATA_4( 0xe5f4276d, 0x6cab325d, 0xf115285e, 0x007ff6a2 ),
	/* 200 */ DATA_4( 0x67f8b051, 0x5aeb9398, 0x443544d8, 0x0721a9a1 ),
	/* 216 */ DATA_4( 0x07c1312a, 0xe0da6e19, 0x751f6dc1, 0x15a28319 ),
	/* 232 */ DATA_4( 0x6bfec344, 0x71d48a7f, 0x14db688d, 0x906eba82 ),
	/* 248 */ DATA_1x2( 0x00000001, 0x00000000 ),
	/* 256 */ DATA_4( 0x5cde0345, 0x90c5a9a4, 0x008bc747, 0x00000000 ),
	/* 272 */ DATA_4( 0x74f5c2f8, 0x701ae2c3, 0x3eefe2f7, 0x00000000 ),
	/* 288 */ DATA_4( 0x6d581ca3, 0xcbcfcd58, 0xe5a7e267, 0x0000000d ),
	/* 304 */ DATA_4( 0x92ce8b13, 0xe89a8870, 0xa4a51881, 0x000001f7 ),
	/* 320 */ DATA_4( 0xd99eb0ef, 0xdf999924, 0xb407b7f2, 0x00003250 ),
	/* 336 */ DATA_4( 0xd77e2406, 0xdb9f0613, 0x68ec5b75, 0x0003affc ),
	/* 352 */ DATA_4( 0xb9d4bb0d, 0x21313bcc, 0xcdf4329f, 0x00332b1d ),
	/* 368 */ DATA_4( 0xc8a7cf9d, 0x5cc64048, 0x002b192c, 0x02048bcb ),
	/* 384 */ DATA_4( 0xe524025a, 0x0060238b, 0xf665cb1c, 0x0e222a9b ),
	/* 400 */ DATA_4( 0x6d9018fe, 0xb4bd5070, 0xbc469270, 0x3dd70b93 ),
	/* 416 */ DATA_4( 0x00000000, 0x00000000, 0x00000000, 0x80000000 ),
	/* 432 */ DATA_1x2( 0x00000001, 0x00000000 ),

	/* Fixed point coefficients for erfc(x) evaluation */
	/* 440 */ DATA_4( 0xed06573b, 0x512796c0, 0x009aba1f, 0x00000000 ),
	/* 456 */ DATA_4( 0xc0316b85, 0xd5ef03cb, 0x8737cadd, 0x00000002 ),
	/* 472 */ DATA_4( 0x84785e50, 0xfea641fd, 0xa89094fc, 0x000001a8 ),
	/* 488 */ DATA_4( 0xa40b5daf, 0x671a4ac7, 0xdf02c17c, 0x0000669e ),
	/* 504 */ DATA_4( 0xe691a849, 0x73fe6ac1, 0xe337f4e7, 0x000bf3cd ),
	/* 520 */ DATA_4( 0x6d5b4479, 0x9e22ec73, 0x7386ca8c, 0x00c251fe ),
	/* 536 */ DATA_4( 0x70d74b54, 0xff37636b, 0xaac6d558, 0x072327fe ),
	/* 552 */ DATA_4( 0xfda3ec67, 0xb10f37fc, 0x2ed79804, 0x2780b9f3 ),
	/* 568 */ DATA_4( 0x6fcba7b4, 0x663ae3dc, 0xabb226b4, 0x7dd1ffd9 ),
	/* 584 */ DATA_4( 0x6ae846ad, 0x61d40831, 0x18de9d28, 0xd396d32d ),
	/* 600 */ DATA_4( 0x6bfec344, 0x71d48a7f, 0x14db688d, 0x906eba82 ),
	/* 616 */ DATA_1x2( 0x000000-3, 0x00000000 ),
	/* 624 */ DATA_4( 0xa522fa40, 0x38900912, 0x02199f19, 0x00000000 ),
	/* 640 */ DATA_4( 0x4c93f48a, 0x5070d6ad, 0xed6686d5, 0x00000003 ),
	/* 656 */ DATA_4( 0x82a6ef78, 0xc9d7c414, 0x37e89742, 0x000001fb ),
	/* 672 */ DATA_4( 0xe05a1b8f, 0x16233622, 0xc0459e24, 0x00006c38 ),
	/* 688 */ DATA_4( 0x34408ea6, 0x539920f1, 0x36c389eb, 0x000bc6aa ),
	/* 704 */ DATA_4( 0xb302fa6d, 0x473e17d5, 0x0191c399, 0x00b7e45a ),
	/* 720 */ DATA_4( 0x7c5a230c, 0x10b9f27f, 0x1a1c27c2, 0x069606ff ),
	/* 736 */ DATA_4( 0xabbaddef, 0x7aff09fc, 0x170844cb, 0x23db7a3b ),
	/* 752 */ DATA_4( 0x91212a77, 0xf0d73ba7, 0xf7b1db1a, 0x70f46698 ),
	/* 768 */ DATA_4( 0x12d9c217, 0xf77fbd27, 0x526b5b36, 0xbc841944 ),
	/* 784 */ DATA_4( 0x00000000, 0x00000000, 0x00000000, 0x80000000 ),
	/* 800 */ DATA_1x2( 0x00000001, 0x00000000 ),

	/* Packed coefficients for mid numerator evaluation */
	/* 808 */ DATA_4( 0xe2a7cbc0, 0xd824dff5, 0x2cf8e12d, 0xcbd29966 ),
	/* 824 */ DATA_4( 0x14a8dcf4, 0x17295b8a, 0x01782242, 0x88e5f042 ),
	/* 840 */ DATA_4( 0x7115fef4, 0x5861def3, 0xe7164162, 0xb66d0a3e ),
	/* 856 */ DATA_4( 0xed898946, 0x010150cf, 0xed17a27e, 0x9f024216 ),
	/* 872 */ DATA_4( 0xdacad206, 0x13319615, 0xf98f246f, 0xca00d3a2 ),
	/* 888 */ DATA_4( 0xee3b6486, 0x06462369, 0x1d6f83c8, 0xc5a1bc70 ),
	/* 904 */ DATA_4( 0x3ed3e2c8, 0xcc0060f7, 0x49be1d68, 0x99a7e90d ),
	/* 920 */ DATA_4( 0xe750aeb8, 0xca9bbf22, 0x2dd2d670, 0xc158e9c4 ),
	/* 936 */ DATA_4( 0xe1f88d88, 0x8cd95e49, 0x4d0fbe75, 0xc6beb986 ),
	/* 952 */ DATA_4( 0x0c71a92a, 0xc67ac67a, 0xf964e260, 0xa75abf3d ),
	/* 968 */ DATA_4( 0xf0b36cea, 0x28de78ed, 0xd4d57310, 0xe62294ca ),
	/* 984 */ DATA_4( 0x26f2d00a, 0x228cf6d2, 0x11d8c668, 0xffe345bc ),
	/* 1000 */ DATA_4( 0x0073676a, 0x43d22116, 0x122673b5, 0xe1e5d119 ),
	/* 1016 */ DATA_4( 0x23fc903c, 0x63895404, 0x69b73a2d, 0x997b13f1 ),
	/* 1032 */ DATA_4( 0x43669c7e, 0xdb71a733, 0x3be837ea, 0x9806802a ),
	/* 1048 */ DATA_4( 0x4f807f2e, 0xc4dfff5e, 0x3e94bd84, 0xc571ee37 ),
	/* 1064 */ DATA_4( 0x7630dbde, 0x67e257e9, 0x00000000, 0x80000000 ),

	/* Packed coefficients for mid denominator evaluation */
	/* 1080 */ DATA_4( 0xd89b76c0, 0x05d4fe61, 0xd97e2ef3, 0xb4a2145d ),
	/* 1096 */ DATA_4( 0x31a6bc30, 0x9d9984a3, 0xb5e650f0, 0xf2a54f04 ),
	/* 1112 */ DATA_4( 0x9e7af042, 0xe97ce9f3, 0x6dc7ed1e, 0xa1c24c5a ),
	/* 1128 */ DATA_4( 0xc4c6fce4, 0x91b02101, 0x4754004e, 0x8d27a4b9 ),
	/* 1144 */ DATA_4( 0x7ec3ea24, 0xa0b199ce, 0x36b707b1, 0xb3a6cb36 ),
	/* 1160 */ DATA_4( 0x8319d0f4, 0x760a21f7, 0xcb561e52, 0xb03e6b4c ),
	/* 1176 */ DATA_4( 0xffcbf966, 0x13ab93a3, 0x54317f40, 0x899010a9 ),
	/* 1192 */ DATA_4( 0xb88b23a6, 0x930021bf, 0x030064e4, 0xae0d59ab ),
	/* 1208 */ DATA_4( 0x5250ffe6, 0x39761e97, 0x5586fbb9, 0xb44d80d5 ),
	/* 1224 */ DATA_4( 0xd048dc48, 0xe580fe89, 0xcd0c4ce7, 0x9980c3e0 ),
	/* 1240 */ DATA_4( 0x675bac48, 0x04253cdb, 0x5175b4d4, 0xd67474e1 ),
	/* 1256 */ DATA_4( 0x64c5aa78, 0x379d4d7d, 0x707f3016, 0xf413c5e7 ),
	/* 1272 */ DATA_4( 0x0c6e9318, 0xb23f0eb3, 0x440397fc, 0xdf3f2022 ),
	/* 1288 */ DATA_4( 0x7590a22a, 0x78a99e1c, 0x070ff8a1, 0xa07972f0 ),
	/* 1304 */ DATA_4( 0x07c619ca, 0x7d0a6907, 0xdae3e255, 0xaee9fe93 ),
	/* 1320 */ DATA_4( 0x163780fc, 0x30c4b60f, 0x2cb78373, 0x88152ee9 ),
	/* 1336 */ DATA_4( 0x63af014e, 0xa8344a43, 0x248138e7, 0x86d4a5bc ),
	/* 1352 */ DATA_4( 0x0000000c, 0x00000000, 0x00000000, 0x80000000 ),
	};

#define	ERF_CLASS_TO_ACTION_MAP		((U_WORD const *) ((char *) TABLE_NAME + 0))
#define	ERFC_CLASS_TO_ACTION_MAP	((U_WORD const *) ((char *) TABLE_NAME + 16))
#define	UX_ZERO				((UX_FLOAT *) ((char *) TABLE_NAME + 48))
#define	ERF_COEF_ARRAY			((FIXED_128 *) ((char *) TABLE_NAME + 72))
#define	ERF_COEF_ARRAY_DEGREE		(( signed __int64 ) 0x000000000000000a )
#define	ERFC_COEF_ARRAY			((FIXED_128 *) ((char *) TABLE_NAME + 440))
#define	ERFC_COEF_ARRAY_DEGREE		(( signed __int64 ) 0x000000000000000a )
#define	MID_NUM_COEF_ARRAY		((FIXED_128 *) ((char *) TABLE_NAME + 808))
#define	MID_NUM_COEF_ARRAY_DEGREE	(( signed __int64 ) 0x0000000000000010 )
#define	MID_NUM_SCALE_BIAS		(( signed __int64 ) 0x0000000000000006 )
#define	MID_NUM_SCALE_MASK		(( signed __int64 ) 0x0000000000000007 )
#define	MID_DEN_COEF_ARRAY		((FIXED_128 *) ((char *) TABLE_NAME + 1080))
#define	MID_DEN_COEF_ARRAY_DEGREE	(( signed __int64 ) 0x0000000000000011 )
#define	MID_DEN_SCALE_BIAS		(( signed __int64 ) 0x0000000000000005 )
#define	MID_DEN_SCALE_MASK		(( signed __int64 ) 0x0000000000000007 )
