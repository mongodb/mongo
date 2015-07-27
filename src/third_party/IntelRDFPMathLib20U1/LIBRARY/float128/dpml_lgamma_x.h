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

	/* lgamma class-to-action-mapping */
	/* 000 */ DATA_1x2( 0x00eb9408, 0x1efb0000 ),
	/* 008 */ DATA_1x2( 0x00000088, 0x00000000 ),
	/* 016 */ DATA_1x2( 0x00000089, 0x00000000 ),
	/* 024 */ DATA_1x2( 0x0000008b, 0x00000000 ),

	/* Unpacked values of 1, 1/2, 3, ln2, pi/2, ln(2*pi)/2 and ln(pi/2)/2 */
	/* 032 */ POS, 0001, DATA_2x2( 0x00000000, 0x80000000, 0x00000000, 0x00000000 ),
	/* 056 */ POS, 0000, DATA_2x2( 0x00000000, 0x80000000, 0x00000000, 0x00000000 ),
	/* 080 */ POS, 0002, DATA_2x2( 0x00000000, 0xc0000000, 0x00000000, 0x00000000 ),
	/* 104 */ POS, 0000, DATA_2x2( 0xd1cf79ab, 0xb17217f7, 0x03f2f6af, 0xc9e3b398 ),
	/* 128 */ POS, 0001, DATA_2x2( 0x2168c234, 0xc90fdaa2, 0x80dc1cd1, 0xc4c6628b ),
	/* 152 */ POS, 0000, DATA_2x2( 0x25f5a534, 0xeb3f8e43, 0x44192023, 0x94bc9001 ),
	/* 176 */ NEG, 00-2, DATA_2x2( 0x5098ae23, 0xe735d92d, 0x0098a5d2, 0x2b6371a5 ),

	/* Fixed point coefficients for lgamma on [1,2) */
	/* 200 */ DATA_2x2( 0x00000000, 0x00000000, 0x00000000, 0x00000000 ),
	/* 216 */ DATA_4( 0x60a9b302, 0x251114f8, 0x000a9755, 0x00000000 ),
	/* 232 */ DATA_4( 0x2cc9bd10, 0x4a93d634, 0x10f9922b, 0x00000000 ),
	/* 248 */ DATA_4( 0xee192d70, 0x82f37577, 0xa7292d3c, 0x00000007 ),
	/* 264 */ DATA_4( 0x66c06a08, 0xa21e1b64, 0x4eb14ba2, 0x00000193 ),
	/* 280 */ DATA_4( 0x0be50f9f, 0x49519e8b, 0x7e5a1213, 0x00002e54 ),
	/* 296 */ DATA_4( 0x390dbcb7, 0x01bedab0, 0x79f1e1e9, 0x00034d19 ),
	/* 312 */ DATA_4( 0xe9de96be, 0x8c803c56, 0x8033da1a, 0x0027b25a ),
	/* 328 */ DATA_4( 0xc35545a5, 0x04688452, 0xfac6a931, 0x0145ce49 ),
	/* 344 */ DATA_4( 0x96c7ab7c, 0x52635611, 0xe0e409ba, 0x073eb570 ),
	/* 360 */ DATA_4( 0x21e7042e, 0xc8eb5b37, 0x8c27bb64, 0x1c98e3bf ),
	/* 376 */ DATA_4( 0x7e72db3d, 0x80c9f91f, 0x6f117f09, 0x4ccac2fa ),
	/* 392 */ DATA_4( 0x9081feb0, 0x99782df4, 0x590f0953, 0x85d5f505 ),
	/* 408 */ DATA_4( 0x6c10b7f7, 0x1b6c7514, 0x39f9e37e, 0x88814a09 ),
	/* 424 */ DATA_4( 0x83a6a3c6, 0x3431fac5, 0x2983229a, 0x3dd72b61 ),
	/* 440 */ DATA_1x2( 0x000000-1, 0x00000000 ),
	/* 448 */ DATA_4( 0xc67ddea9, 0x27e51e40, 0x0000731a, 0x00000000 ),
	/* 464 */ DATA_4( 0x5b1c0e74, 0xc97fab18, 0x0115d73c, 0x00000000 ),
	/* 480 */ DATA_4( 0x1d79d56f, 0x6e9d2e49, 0xa6315e07, 0x00000000 ),
	/* 496 */ DATA_4( 0xf4cb1e90, 0x00133167, 0x5e5a31c5, 0x0000002b ),
	/* 512 */ DATA_4( 0x216b5ee3, 0xc14d67db, 0x17b80486, 0x0000062f ),
	/* 528 */ DATA_4( 0x5598dbab, 0x5e4616f4, 0x89d3ba81, 0x00008abf ),
	/* 544 */ DATA_4( 0x04f76b07, 0x2f86a645, 0x77d3affa, 0x000801ae ),
	/* 560 */ DATA_4( 0xf8d34de2, 0x21b7e21e, 0xeeb8ec9f, 0x00512a20 ),
	/* 576 */ DATA_4( 0xb9653bbe, 0xde7adeae, 0xf89914ba, 0x0241d70a ),
	/* 592 */ DATA_4( 0x979ac908, 0x9b14aeee, 0xedde5c80, 0x0b628f84 ),
	/* 608 */ DATA_4( 0xdb6f3010, 0xac217782, 0xf57d41e9, 0x28786839 ),
	/* 624 */ DATA_4( 0xa00e1d50, 0x1c26652e, 0xd536e872, 0x63216f20 ),
	/* 640 */ DATA_4( 0xd4c5cd55, 0x2fe415ce, 0xc53819f2, 0x9f39f1ec ),
	/* 656 */ DATA_4( 0x97207a40, 0x7d9ed1ea, 0x01fb71ed, 0x96f0820b ),
	/* 672 */ DATA_4( 0x00000000, 0x00000000, 0x00000000, 0x40000000 ),
	/* 688 */ DATA_1x2( 0x00000002, 0x00000000 ),

	/* Fixed point coefficients for lgamma(8*x) on [0, 1/16) */
	/* 696 */ DATA_4( 0x943ed058, 0xc3b7bbca, 0xbc3dce5c, 0x00000000 ),
	/* 712 */ DATA_4( 0x0da9290d, 0x2bb58f01, 0x0a447bf9, 0x00000324 ),
	/* 728 */ DATA_4( 0x8ceb762a, 0xaab155aa, 0x4234fdef, 0x0001e4ce ),
	/* 744 */ DATA_4( 0xe9bcf260, 0xb5270ed6, 0x948dbace, 0x00588e2c ),
	/* 760 */ DATA_4( 0x1484eea6, 0x26aa99f0, 0xa5074a12, 0x064912e4 ),
	/* 776 */ DATA_4( 0x229f11bd, 0x77235e20, 0x430459cf, 0x30a1be32 ),
	/* 792 */ DATA_4( 0x257c5853, 0x5544fc19, 0x91178da0, 0x9d3bed71 ),
	/* 808 */ DATA_4( 0xaaaaaaab, 0xaaaaaaaa, 0xaaaaaaaa, 0xaaaaaaaa ),
	/* 824 */ DATA_1x2( 0x000000-6, 0x00000000 ),
	/* 832 */ DATA_4( 0xef3685a9, 0xb0eb5698, 0xbfa75404, 0x00000000 ),
	/* 848 */ DATA_4( 0xad31e3f2, 0xf0e52018, 0x3c335fc5, 0x00000284 ),
	/* 864 */ DATA_4( 0x13d425ca, 0x21bf8b59, 0x77594c75, 0x000173e1 ),
	/* 880 */ DATA_4( 0x679bc1ac, 0x9f5eb7a8, 0x10c65cbd, 0x004306d1 ),
	/* 896 */ DATA_4( 0x15a6dce6, 0x8cea58be, 0xfddab044, 0x04bb9b7a ),
	/* 912 */ DATA_4( 0x4834b491, 0xb1c1f9d3, 0x5d26f16f, 0x2488f69c ),
	/* 928 */ DATA_4( 0xed2e52e1, 0x5104ce23, 0x3de2bb49, 0x75fe0326 ),
	/* 944 */ DATA_4( 0x00000000, 0x00000000, 0x00000000, 0x80000000 ),
	/* 960 */ DATA_1x2( 0x00000001, 0x00000000 ),
	};

#define	LGAMMA_CLASS_TO_ACTION_MAP	((U_WORD const *) ((char *) TABLE_NAME + 0))
#define	UX_ONE				((UX_FLOAT *) ((char *) TABLE_NAME + 32))
#define	UX_HALF				((UX_FLOAT *) ((char *) TABLE_NAME + 56))
#define	UX_THREE				((UX_FLOAT *) ((char *) TABLE_NAME + 80))
#define	UX_LN2				((UX_FLOAT *) ((char *) TABLE_NAME + 104))
#define	UX_PI_OVER_2			((UX_FLOAT *) ((char *) TABLE_NAME + 128))
#define	UX_HALF_LN_TWO_PI		((UX_FLOAT *) ((char *) TABLE_NAME + 152))
#define	UX_HALF_LN_TWO_OVER_PI		((UX_FLOAT *) ((char *) TABLE_NAME + 176))
#define	LGAMMA_P_COEF_ARRAY		((FIXED_128 *) ((char *) TABLE_NAME + 200))
#define	LGAMMA_P_COEF_ARRAY_DEGREE	(( signed __int64 ) 0x000000000000000e )
#define	LGAMMA_PHI_COEF_ARRAY		((FIXED_128 *) ((char *) TABLE_NAME + 696))
#define	LGAMMA_PHI_COEF_ARRAY_DEGREE	(( signed __int64 ) 0x0000000000000007 )
