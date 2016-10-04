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

#include "bid_internal.h"

BID_UINT64 bid_round_const_table[][19] = {
  {	// RN
   0ull,	// 0 extra digits
   5ull,	// 1 extra digits
   50ull,	// 2 extra digits
   500ull,	// 3 extra digits
   5000ull,	// 4 extra digits
   50000ull,	// 5 extra digits
   500000ull,	// 6 extra digits
   5000000ull,	// 7 extra digits
   50000000ull,	// 8 extra digits
   500000000ull,	// 9 extra digits
   5000000000ull,	// 10 extra digits
   50000000000ull,	// 11 extra digits
   500000000000ull,	// 12 extra digits
   5000000000000ull,	// 13 extra digits
   50000000000000ull,	// 14 extra digits
   500000000000000ull,	// 15 extra digits
   5000000000000000ull,	// 16 extra digits
   50000000000000000ull,	// 17 extra digits
   500000000000000000ull	// 18 extra digits
   }
  ,
  {	// RD
   0ull,	// 0 extra digits
   0ull,	// 1 extra digits
   0ull,	// 2 extra digits
   00ull,	// 3 extra digits
   000ull,	// 4 extra digits
   0000ull,	// 5 extra digits
   00000ull,	// 6 extra digits
   000000ull,	// 7 extra digits
   0000000ull,	// 8 extra digits
   00000000ull,	// 9 extra digits
   000000000ull,	// 10 extra digits
   0000000000ull,	// 11 extra digits
   00000000000ull,	// 12 extra digits
   000000000000ull,	// 13 extra digits
   0000000000000ull,	// 14 extra digits
   00000000000000ull,	// 15 extra digits
   000000000000000ull,	// 16 extra digits
   0000000000000000ull,	// 17 extra digits
   00000000000000000ull	// 18 extra digits
   }
  ,
  {	// round to Inf
   0ull,	// 0 extra digits
   9ull,	// 1 extra digits
   99ull,	// 2 extra digits
   999ull,	// 3 extra digits
   9999ull,	// 4 extra digits
   99999ull,	// 5 extra digits
   999999ull,	// 6 extra digits
   9999999ull,	// 7 extra digits
   99999999ull,	// 8 extra digits
   999999999ull,	// 9 extra digits
   9999999999ull,	// 10 extra digits
   99999999999ull,	// 11 extra digits
   999999999999ull,	// 12 extra digits
   9999999999999ull,	// 13 extra digits
   99999999999999ull,	// 14 extra digits
   999999999999999ull,	// 15 extra digits
   9999999999999999ull,	// 16 extra digits
   99999999999999999ull,	// 17 extra digits
   999999999999999999ull	// 18 extra digits
   }
  ,
  {	// RZ
   0ull,	// 0 extra digits
   0ull,	// 1 extra digits
   0ull,	// 2 extra digits
   00ull,	// 3 extra digits
   000ull,	// 4 extra digits
   0000ull,	// 5 extra digits
   00000ull,	// 6 extra digits
   000000ull,	// 7 extra digits
   0000000ull,	// 8 extra digits
   00000000ull,	// 9 extra digits
   000000000ull,	// 10 extra digits
   0000000000ull,	// 11 extra digits
   00000000000ull,	// 12 extra digits
   000000000000ull,	// 13 extra digits
   0000000000000ull,	// 14 extra digits
   00000000000000ull,	// 15 extra digits
   000000000000000ull,	// 16 extra digits
   0000000000000000ull,	// 17 extra digits
   00000000000000000ull	// 18 extra digits
   }
  ,
  {	// round ties away from 0
   0ull,	// 0 extra digits
   5ull,	// 1 extra digits
   50ull,	// 2 extra digits
   500ull,	// 3 extra digits
   5000ull,	// 4 extra digits
   50000ull,	// 5 extra digits
   500000ull,	// 6 extra digits
   5000000ull,	// 7 extra digits
   50000000ull,	// 8 extra digits
   500000000ull,	// 9 extra digits
   5000000000ull,	// 10 extra digits
   50000000000ull,	// 11 extra digits
   500000000000ull,	// 12 extra digits
   5000000000000ull,	// 13 extra digits
   50000000000000ull,	// 14 extra digits
   500000000000000ull,	// 15 extra digits
   5000000000000000ull,	// 16 extra digits
   50000000000000000ull,	// 17 extra digits
   500000000000000000ull	// 18 extra digits
   }
  ,
};

BID_UINT128 bid_round_const_table_128[][36] = {
  {	//RN
   {{0ull, 0ull}
    }
   ,	// 0 extra digits
   {{5ull, 0ull}
    }
   ,	// 1 extra digits
   {{50ull, 0ull}
    }
   ,	// 2 extra digits
   {{500ull, 0ull}
    }
   ,	// 3 extra digits
   {{5000ull, 0ull}
    }
   ,	// 4 extra digits
   {{50000ull, 0ull}
    }
   ,	// 5 extra digits
   {{500000ull, 0ull}
    }
   ,	// 6 extra digits
   {{5000000ull, 0ull}
    }
   ,	// 7 extra digits
   {{50000000ull, 0ull}
    }
   ,	// 8 extra digits
   {{500000000ull, 0ull}
    }
   ,	// 9 extra digits
   {{5000000000ull, 0ull}
    }
   ,	// 10 extra digits
   {{50000000000ull, 0ull}
    }
   ,	// 11 extra digits
   {{500000000000ull, 0ull}
    }
   ,	// 12 extra digits
   {{5000000000000ull, 0ull}
    }
   ,	// 13 extra digits
   {{50000000000000ull, 0ull}
    }
   ,	// 14 extra digits
   {{500000000000000ull, 0ull}
    }
   ,	// 15 extra digits
   {{5000000000000000ull, 0ull}
    }
   ,	// 16 extra digits
   {{50000000000000000ull, 0ull}
    }
   ,	// 17 extra digits
   {{500000000000000000ull, 0ull}
    }
   ,	// 18 extra digits
   {{5000000000000000000ull, 0ull}
    }
   ,	// 19 extra digits
   {{0xb5e3af16b1880000ull, 2ull}
    }
   ,	//20
   {{0x1ae4d6e2ef500000ull, 27ull}
    }
   ,	//21
   {{0xcf064dd59200000ull, 271ull}
    }
   ,	//22
   {{0x8163f0a57b400000ull, 2710ull}
    }
   ,	//23
   {{0xde76676d0800000ull, 27105ull}
    }
   ,	//24
   {{0x8b0a00a425000000ull, 0x422caull}
    }
   ,	//25
   {{0x6e64066972000000ull, 0x295be9ull}
    }
   ,	//26
   {{0x4fe8401e74000000ull, 0x19d971eull}
    }
   ,	//27
   {{0x1f12813088000000ull, 0x1027e72full}
    }
   ,	//28
   {{0x36b90be550000000ull, 0xa18f07d7ull}
    }
   ,	//29
   {{0x233a76f520000000ull, 0x64f964e68ull}
    }
   ,	//30
   {{0x6048a59340000000ull, 0x3f1bdf1011ull}
    }
   ,	//31
   {{0xc2d677c080000000ull, 0x27716b6a0adull}
    }
   ,	//32
   {{0x9c60ad8500000000ull, 0x18a6e32246c9ull}
    }
   ,	//33
   {{0x1bc6c73200000000ull, 0xf684df56c3e0ull}
    }
   ,	//34
   {{0x15c3c7f400000000ull, 0x9a130b963a6c1ull}
    }
   ,	//35
   }
  ,
  {	//RD
   {{0ull, 0ull}
    }
   ,	// 0 extra digits
   {{0ull, 0ull}
    }
   ,	// 1 extra digits
   {{0ull, 0ull}
    }
   ,	// 2 extra digits
   {{00ull, 0ull}
    }
   ,	// 3 extra digits
   {{000ull, 0ull}
    }
   ,	// 4 extra digits
   {{0000ull, 0ull}
    }
   ,	// 5 extra digits
   {{00000ull, 0ull}
    }
   ,	// 6 extra digits
   {{000000ull, 0ull}
    }
   ,	// 7 extra digits
   {{0000000ull, 0ull}
    }
   ,	// 8 extra digits
   {{00000000ull, 0ull}
    }
   ,	// 9 extra digits
   {{000000000ull, 0ull}
    }
   ,	// 10 extra digits
   {{0000000000ull, 0ull}
    }
   ,	// 11 extra digits
   {{00000000000ull, 0ull}
    }
   ,	// 12 extra digits
   {{000000000000ull, 0ull}
    }
   ,	// 13 extra digits
   {{0000000000000ull, 0ull}
    }
   ,	// 14 extra digits
   {{00000000000000ull, 0ull}
    }
   ,	// 15 extra digits
   {{000000000000000ull, 0ull}
    }
   ,	// 16 extra digits
   {{0000000000000000ull, 0ull}
    }
   ,	// 17 extra digits
   {{00000000000000000ull, 0ull}
    }
   ,	// 18 extra digits
   {{000000000000000000ull, 0ull}
    }
   ,	// 19 extra digits
   {{0ull, 0ull}
    }
   ,	//20
   {{0ull, 0ull}
    }
   ,	//21
   {{0ull, 0ull}
    }
   ,	//22
   {{0ull, 0ull}
    }
   ,	//23
   {{0ull, 0ull}
    }
   ,	//24
   {{0ull, 0ull}
    }
   ,	//25
   {{0ull, 0ull}
    }
   ,	//26
   {{0ull, 0ull}
    }
   ,	//27
   {{0ull, 0ull}
    }
   ,	//28
   {{0ull, 0ull}
    }
   ,	//29
   {{0ull, 0ull}
    }
   ,	//30
   {{0ull, 0ull}
    }
   ,	//31
   {{0ull, 0ull}
    }
   ,	//32
   {{0ull, 0ull}
    }
   ,	//33
   {{0ull, 0ull}
    }
   ,	//34
   {{0ull, 0ull}
    }
   ,	//35
   }
  ,
  {	//RU
   {{0ull, 0ull}
    }
   ,	// 0 extra digits
   {{9ull, 0ull}
    }
   ,	// 1 extra digits
   {{99ull, 0ull}
    }
   ,	// 2 extra digits
   {{999ull, 0ull}
    }
   ,	// 3 extra digits
   {{9999ull, 0ull}
    }
   ,	// 4 extra digits
   {{99999ull, 0ull}
    }
   ,	// 5 extra digits
   {{999999ull, 0ull}
    }
   ,	// 6 extra digits
   {{9999999ull, 0ull}
    }
   ,	// 7 extra digits
   {{99999999ull, 0ull}
    }
   ,	// 8 extra digits
   {{999999999ull, 0ull}
    }
   ,	// 9 extra digits
   {{9999999999ull, 0ull}
    }
   ,	// 10 extra digits
   {{99999999999ull, 0ull}
    }
   ,	// 11 extra digits
   {{999999999999ull, 0ull}
    }
   ,	// 12 extra digits
   {{9999999999999ull, 0ull}
    }
   ,	// 13 extra digits
   {{99999999999999ull, 0ull}
    }
   ,	// 14 extra digits
   {{999999999999999ull, 0ull}
    }
   ,	// 15 extra digits
   {{9999999999999999ull, 0ull}
    }
   ,	// 16 extra digits
   {{99999999999999999ull, 0ull}
    }
   ,	// 17 extra digits
   {{999999999999999999ull, 0ull}
    }
   ,	// 18 extra digits
   {{9999999999999999999ull, 0ull}
    }
   ,	// 19 extra digits
   {{0x6BC75E2D630FFFFFull, 0x5ull}
    }
   ,	//20
   {{0x35C9ADC5DE9FFFFFull, 0x36ull}
    }
   ,	//21
   {{0x19E0C9BAB23FFFFFull, 0x21eull}
    }
   ,	//22
   {{0x2C7E14AF67FFFFFull, 0x152dull}
    }
   ,	//23
   {{0x1BCECCEDA0FFFFFFull, 0xd3c2ull}
    }
   ,	//24
   {{0x1614014849FFFFFFull, 0x84595ull}
    }
   ,	//25
   {{0xDCC80CD2E3FFFFFFull, 0x52b7d2ull}
    }
   ,	//26
   {{0x9FD0803CE7FFFFFFull, 0x33B2E3Cull}
    }
   ,	//27
   {{0x3E2502610FFFFFFFull, 0x204FCE5Eull}
    }
   ,	//28
   {{0x6D7217CA9FFFFFFFull, 0x1431E0FAEull}
    }
   ,	//29
   {{0x4674EDEA3FFFFFFFull, 0xC9F2C9CD0ull}
    }
   ,	//30
   {{0xC0914B267FFFFFFFull, 0x7E37BE2022ull}
    }
   ,	//31
   {{0x85ACEF80FFFFFFFFull, 0x4EE2D6D415Bull}
    }
   ,	//32
   {{0x38c15b09ffffffffull, 0x314dc6448d93ull}
    }
   ,	//33
   {{0x378d8e63ffffffffull, 0x1ed09bead87c0ull}
    }
   ,	//34
   {{0x2b878fe7ffffffffull, 0x13426172c74d82ull}
    }
   ,	//35
   }
  ,
  {	//RZ
   {{0ull, 0ull}
    }
   ,	// 0 extra digits
   {{0ull, 0ull}
    }
   ,	// 1 extra digits
   {{0ull, 0ull}
    }
   ,	// 2 extra digits
   {{00ull, 0ull}
    }
   ,	// 3 extra digits
   {{000ull, 0ull}
    }
   ,	// 4 extra digits
   {{0000ull, 0ull}
    }
   ,	// 5 extra digits
   {{00000ull, 0ull}
    }
   ,	// 6 extra digits
   {{000000ull, 0ull}
    }
   ,	// 7 extra digits
   {{0000000ull, 0ull}
    }
   ,	// 8 extra digits
   {{00000000ull, 0ull}
    }
   ,	// 9 extra digits
   {{000000000ull, 0ull}
    }
   ,	// 10 extra digits
   {{0000000000ull, 0ull}
    }
   ,	// 11 extra digits
   {{00000000000ull, 0ull}
    }
   ,	// 12 extra digits
   {{000000000000ull, 0ull}
    }
   ,	// 13 extra digits
   {{0000000000000ull, 0ull}
    }
   ,	// 14 extra digits
   {{00000000000000ull, 0ull}
    }
   ,	// 15 extra digits
   {{000000000000000ull, 0ull}
    }
   ,	// 16 extra digits
   {{0000000000000000ull, 0ull}
    }
   ,	// 17 extra digits
   {{00000000000000000ull, 0ull}
    }
   ,	// 18 extra digits
   {{000000000000000000ull, 0ull}
    }
   ,	// 19 extra digits
   {{0ull, 0ull}
    }
   ,	//20
   {{0ull, 0ull}
    }
   ,	//21
   {{0ull, 0ull}
    }
   ,	//22
   {{0ull, 0ull}
    }
   ,	//23
   {{0ull, 0ull}
    }
   ,	//24
   {{0ull, 0ull}
    }
   ,	//25
   {{0ull, 0ull}
    }
   ,	//26
   {{0ull, 0ull}
    }
   ,	//27
   {{0ull, 0ull}
    }
   ,	//28
   {{0ull, 0ull}
    }
   ,	//29
   {{0ull, 0ull}
    }
   ,	//30
   {{0ull, 0ull}
    }
   ,	//31
   {{0ull, 0ull}
    }
   ,	//32
   {{0ull, 0ull}
    }
   ,	//33
   {{0ull, 0ull}
    }
   ,	//34
   {{0ull, 0ull}
    }
   ,	//35
   }
  ,
  {	//RN, ties away
   {{0ull, 0ull}
    }
   ,	// 0 extra digits
   {{5ull, 0ull}
    }
   ,	// 1 extra digits
   {{50ull, 0ull}
    }
   ,	// 2 extra digits
   {{500ull, 0ull}
    }
   ,	// 3 extra digits
   {{5000ull, 0ull}
    }
   ,	// 4 extra digits
   {{50000ull, 0ull}
    }
   ,	// 5 extra digits
   {{500000ull, 0ull}
    }
   ,	// 6 extra digits
   {{5000000ull, 0ull}
    }
   ,	// 7 extra digits
   {{50000000ull, 0ull}
    }
   ,	// 8 extra digits
   {{500000000ull, 0ull}
    }
   ,	// 9 extra digits
   {{5000000000ull, 0ull}
    }
   ,	// 10 extra digits
   {{50000000000ull, 0ull}
    }
   ,	// 11 extra digits
   {{500000000000ull, 0ull}
    }
   ,	// 12 extra digits
   {{5000000000000ull, 0ull}
    }
   ,	// 13 extra digits
   {{50000000000000ull, 0ull}
    }
   ,	// 14 extra digits
   {{500000000000000ull, 0ull}
    }
   ,	// 15 extra digits
   {{5000000000000000ull, 0ull}
    }
   ,	// 16 extra digits
   {{50000000000000000ull, 0ull}
    }
   ,	// 17 extra digits
   {{500000000000000000ull, 0ull}
    }
   ,	// 18 extra digits
   {{5000000000000000000ull, 0ull}
    }
   ,	// 19 extra digits
   {{0xb5e3af16b1880000ull, 2ull}
    }
   ,	//20
   {{0x1ae4d6e2ef500000ull, 27ull}
    }
   ,	//21
   {{0xcf064dd59200000ull, 271ull}
    }
   ,	//22
   {{0x8163f0a57b400000ull, 2710ull}
    }
   ,	//23
   {{0xde76676d0800000ull, 27105ull}
    }
   ,	//24
   {{0x8b0a00a425000000ull, 0x422caull}
    }
   ,	//25
   {{0x6e64066972000000ull, 0x295be9ull}
    }
   ,	//26
   {{0x4fe8401e74000000ull, 0x19d971eull}
    }
   ,	//27
   {{0x1f12813088000000ull, 0x1027e72full}
    }
   ,	//28
   {{0x36b90be550000000ull, 0xa18f07d7ull}
    }
   ,	//29
   {{0x233a76f520000000ull, 0x64f964e68ull}
    }
   ,	//30
   {{0x6048a59340000000ull, 0x3f1bdf1011ull}
    }
   ,	//31
   {{0xc2d677c080000000ull, 0x27716b6a0adull}
    }
   ,	//32
   {{0x9c60ad8500000000ull, 0x18a6e32246c9ull}
    }
   ,	//33
   {{0x1bc6c73200000000ull, 0xf684df56c3e0ull}
    }
   ,	//34
   {{0x15c3c7f400000000ull, 0x9a130b963a6c1ull}
    }
   ,	//35
   }
};


BID_UINT128 bid_reciprocals10_128[] = {
  {{0ull, 0ull}
   }
  ,	// 0 extra digits
  {{0x3333333333333334ull, 0x3333333333333333ull}
   }
  ,	// 1 extra digit
  {{0x51eb851eb851eb86ull, 0x051eb851eb851eb8ull}
   }
  ,	// 2 extra digits
  {{0x3b645a1cac083127ull, 0x0083126e978d4fdfull}
   }
  ,	// 3 extra digits
  {{0x4af4f0d844d013aaULL, 0x00346dc5d6388659ULL}
   }
  ,	//  10^(-4) * 2^131
  {{0x08c3f3e0370cdc88ULL, 0x0029f16b11c6d1e1ULL}
   }
  ,	//  10^(-5) * 2^134
  {{0x6d698fe69270b06dULL, 0x00218def416bdb1aULL}
   }
  ,	//  10^(-6) * 2^137
  {{0xaf0f4ca41d811a47ULL, 0x0035afe535795e90ULL}
   }
  ,	//  10^(-7) * 2^141
  {{0xbf3f70834acdaea0ULL, 0x002af31dc4611873ULL}
   }
  ,	//  10^(-8) * 2^144
  {{0x65cc5a02a23e254dULL, 0x00225c17d04dad29ULL}
   }
  ,	//  10^(-9) * 2^147
  {{0x6fad5cd10396a214ULL, 0x0036f9bfb3af7b75ULL}
   }
  ,	// 10^(-10) * 2^151
  {{0xbfbde3da69454e76ULL, 0x002bfaffc2f2c92aULL}
   }
  ,	// 10^(-11) * 2^154
  {{0x32fe4fe1edd10b92ULL, 0x00232f33025bd422ULL}
   }
  ,	// 10^(-12) * 2^157
  {{0x84ca19697c81ac1cULL, 0x00384b84d092ed03ULL}
   }
  ,	// 10^(-13) * 2^161
  {{0x03d4e1213067bce4ULL, 0x002d09370d425736ULL}
   }
  ,	// 10^(-14) * 2^164
  {{0x3643e74dc052fd83ULL, 0x0024075f3dceac2bULL}
   }
  ,	// 10^(-15) * 2^167
  {{0x56d30baf9a1e626bULL, 0x0039a5652fb11378ULL}
   }
  ,	// 10^(-16) * 2^171
  {{0x12426fbfae7eb522ULL, 0x002e1dea8c8da92dULL}
   }
  ,	// 10^(-17) * 2^174
  {{0x41cebfcc8b9890e8ULL, 0x0024e4bba3a48757ULL}
   }
  ,	// 10^(-18) * 2^177
  {{0x694acc7a78f41b0dULL, 0x003b07929f6da558ULL}
   }
  ,	// 10^(-19) * 2^181
  {{0xbaa23d2ec729af3eULL, 0x002f394219248446ULL}
   }
  ,	// 10^(-20) * 2^184
  {{0xfbb4fdbf05baf298ULL, 0x0025c768141d369eULL}
   }
  ,	// 10^(-21) * 2^187
  {{0x2c54c931a2c4b759ULL, 0x003c7240202ebdcbULL}
   }
  ,	// 10^(-22) * 2^191
  {{0x89dd6dc14f03c5e1ULL, 0x00305b66802564a2ULL}
   }
  ,	// 10^(-23) * 2^194
  {{0xd4b1249aa59c9e4eULL, 0x0026af8533511d4eULL}
   }
  ,	// 10^(-24) * 2^197
  {{0x544ea0f76f60fd49ULL, 0x003de5a1ebb4fbb1ULL}
   }
  ,	// 10^(-25) * 2^201
  {{0x76a54d92bf80caa1ULL, 0x00318481895d9627ULL}
   }
  ,	// 10^(-26) * 2^204
  {{0x921dd7a89933d54eULL, 0x00279d346de4781fULL}
   }
  ,	// 10^(-27) * 2^207
  {{0x8362f2a75b862215ULL, 0x003f61ed7ca0c032ULL}
   }
  ,	// 10^(-28) * 2^211
  {{0xcf825bb91604e811ULL, 0x0032b4bdfd4d668eULL}
   }
  ,	// 10^(-29) * 2^214
  {{0x0c684960de6a5341ULL, 0x00289097fdd7853fULL}
   }
  ,	// 10^(-30) * 2^217
  {{0x3d203ab3e521dc34ULL, 0x002073accb12d0ffULL}
   }
  ,	// 10^(-31) * 2^220
  {{0x2e99f7863b696053ULL, 0x0033ec47ab514e65ULL}
   }
  ,	// 10^(-32) * 2^224
  {{0x587b2c6b62bab376ULL, 0x002989d2ef743eb7ULL}
   }
  ,	// 10^(-33) * 2^227
  {{0xad2f56bc4efbc2c5ULL, 0x00213b0f25f69892ULL}
   }
  ,	// 10^(-34) * 2^230
  {{0x0f2abc9d8c9689d1ull, 0x01a95a5b7f87a0efull}
   }
  ,	// 35 extra digits
};


int bid_recip_scale[] = {
  129 - 128,	// 1
  129 - 128,	// 1/10
  129 - 128,	// 1/10^2
  129 - 128,	// 1/10^3
  3,	// 131 - 128
  6,	// 134 - 128
  9,	// 137 - 128
  13,	// 141 - 128
  16,	// 144 - 128
  19,	// 147 - 128
  23,	// 151 - 128
  26,	// 154 - 128
  29,	// 157 - 128
  33,	// 161 - 128
  36,	// 164 - 128
  39,	// 167 - 128
  43,	// 171 - 128
  46,	// 174 - 128
  49,	// 177 - 128
  53,	// 181 - 128
  56,	// 184 - 128
  59,	// 187 - 128
  63,	// 191 - 128

  66,	// 194 - 128
  69,	// 197 - 128
  73,	// 201 - 128
  76,	// 204 - 128
  79,	// 207 - 128
  83,	// 211 - 128
  86,	// 214 - 128
  89,	// 217 - 128
  92,	// 220 - 128
  96,	// 224 - 128
  99,	// 227 - 128
  102,	// 230 - 128
  109,	// 237 - 128, 1/10^35
};


// tables used in computation
int bid_estimate_decimal_digits[129] = {
  1,	//2^0 =1     < 10^0
  1,	//2^1 =2     < 10^1
  1,	//2^2 =4     < 10^1
  1,	//2^3 =8     < 10^1
  2,	//2^4 =16    < 10^2
  2,	//2^5 =32    < 10^2
  2,	//2^6 =64    < 10^2
  3,	//2^7 =128   < 10^3
  3,	//2^8 =256   < 10^3
  3,	//2^9 =512   < 10^3
  4,	//2^10=1024  < 10^4
  4,	//2^11=2048  < 10^4
  4,	//2^12=4096  < 10^4
  4,	//2^13=8192  < 10^4
  5,	//2^14=16384 < 10^5
  5,	//2^15=32768 < 10^5

  5,	//2^16=65536     < 10^5
  6,	//2^17=131072    < 10^6
  6,	//2^18=262144    < 10^6
  6,	//2^19=524288    < 10^6
  7,	//2^20=1048576   < 10^7
  7,	//2^21=2097152   < 10^7
  7,	//2^22=4194304   < 10^7
  7,	//2^23=8388608   < 10^7
  8,	//2^24=16777216  < 10^8
  8,	//2^25=33554432  < 10^8
  8,	//2^26=67108864  < 10^8
  9,	//2^27=134217728 < 10^9
  9,	//2^28=268435456 < 10^9
  9,	//2^29=536870912 < 10^9
  10,	//2^30=1073741824< 10^10
  10,	//2^31=2147483648< 10^10

  10,	//2^32=4294967296     < 10^10
  10,	//2^33=8589934592     < 10^10
  11,	//2^34=17179869184    < 10^11
  11,	//2^35=34359738368    < 10^11
  11,	//2^36=68719476736    < 10^11
  12,	//2^37=137438953472   < 10^12
  12,	//2^38=274877906944   < 10^12
  12,	//2^39=549755813888   < 10^12
  13,	//2^40=1099511627776  < 10^13
  13,	//2^41=2199023255552  < 10^13
  13,	//2^42=4398046511104  < 10^13
  13,	//2^43=8796093022208  < 10^13
  14,	//2^44=17592186044416 < 10^14
  14,	//2^45=35184372088832 < 10^14
  14,	//2^46=70368744177664 < 10^14
  15,	//2^47=140737488355328< 10^15

  15,	//2^48=281474976710656    < 10^15
  15,	//2^49=562949953421312    < 10^15
  16,	//2^50=1125899906842624   < 10^16
  16,	//2^51=2251799813685248   < 10^16
  16,	//2^52=4503599627370496   < 10^16
  16,	//2^53=9007199254740992   < 10^16
  17,	//2^54=18014398509481984  < 10^17
  17,	//2^55=36028797018963968  < 10^17
  17,	//2^56=72057594037927936  < 10^17
  18,	//2^57=144115188075855872 < 10^18
  18,	//2^58=288230376151711744 < 10^18
  18,	//2^59=576460752303423488 < 10^18
  19,	//2^60=1152921504606846976< 10^19
  19,	//2^61=2305843009213693952< 10^19
  19,	//2^62=4611686018427387904< 10^19
  19,	//2^63=9223372036854775808< 10^19

  20,	//2^64=18446744073709551616
  20,	//2^65=36893488147419103232
  20,	//2^66=73786976294838206464
  21,	//2^67=147573952589676412928
  21,	//2^68=295147905179352825856
  21,	//2^69=590295810358705651712
  22,	//2^70=1180591620717411303424
  22,	//2^71=2361183241434822606848
  22,	//2^72=4722366482869645213696
  22,	//2^73=9444732965739290427392
  23,	//2^74=18889465931478580854784
  23,	//2^75=37778931862957161709568
  23,	//2^76=75557863725914323419136
  24,	//2^77=151115727451828646838272
  24,	//2^78=302231454903657293676544
  24,	//2^79=604462909807314587353088

  25,	//2^80=1208925819614629174706176
  25,	//2^81=2417851639229258349412352
  25,	//2^82=4835703278458516698824704
  25,	//2^83=9671406556917033397649408
  26,	//2^84=19342813113834066795298816
  26,	//2^85=38685626227668133590597632
  26,	//2^86=77371252455336267181195264
  27,	//2^87=154742504910672534362390528
  27,	//2^88=309485009821345068724781056
  27,	//2^89=618970019642690137449562112
  28,	//2^90=1237940039285380274899124224
  28,	//2^91=2475880078570760549798248448
  28,	//2^92=4951760157141521099596496896
  28,	//2^93=9903520314283042199192993792
  29,	//2^94=19807040628566084398385987584
  29,	//2^95=39614081257132168796771975168
  29,	//2^96=79228162514264337593543950336

  30,	//2^97=158456325028528675187087900672
  30,	//2^98=316912650057057350374175801344
  30,	//2^99=633825300114114700748351602688
  31,	//2^100=1267650600228229401496703205376
  31,	//2^101=2535301200456458802993406410752
  31,	//2^102=5070602400912917605986812821504
  32,	//2^103=10141204801825835211973625643008
  32,	//2^104=20282409603651670423947251286016
  32,	//2^105=40564819207303340847894502572032
  32,	//2^106=81129638414606681695789005144064
  33,	//2^107=162259276829213363391578010288128
  33,	// 2^108
  33,	// 2^109
  34,	// 2^110
  34,	// 2^111
  34,	// 2^112
  35,	// 2^113
  35,	// 2^114
  35,	// 2^115
  35,	// 2^116
  36,	// 2^117
  36,	// 2^118
  36,	// 2^119
  37,	// 2^120
  37,	// 2^121
  37,	// 2^122
  38,	// 2^123
  38,	// 2^124
  38,	// 2^125
  38,	// 2^126
  39,	// 2^127
  39	// 2^128
};


BID_UINT128 bid_power10_table_128[] = {
  {{0x0000000000000001ull, 0x0000000000000000ull}},	// 10^0
  {{0x000000000000000aull, 0x0000000000000000ull}},	// 10^1
  {{0x0000000000000064ull, 0x0000000000000000ull}},	// 10^2
  {{0x00000000000003e8ull, 0x0000000000000000ull}},	// 10^3
  {{0x0000000000002710ull, 0x0000000000000000ull}},	// 10^4
  {{0x00000000000186a0ull, 0x0000000000000000ull}},	// 10^5
  {{0x00000000000f4240ull, 0x0000000000000000ull}},	// 10^6
  {{0x0000000000989680ull, 0x0000000000000000ull}},	// 10^7
  {{0x0000000005f5e100ull, 0x0000000000000000ull}},	// 10^8
  {{0x000000003b9aca00ull, 0x0000000000000000ull}},	// 10^9
  {{0x00000002540be400ull, 0x0000000000000000ull}},	// 10^10
  {{0x000000174876e800ull, 0x0000000000000000ull}},	// 10^11
  {{0x000000e8d4a51000ull, 0x0000000000000000ull}},	// 10^12
  {{0x000009184e72a000ull, 0x0000000000000000ull}},	// 10^13
  {{0x00005af3107a4000ull, 0x0000000000000000ull}},	// 10^14
  {{0x00038d7ea4c68000ull, 0x0000000000000000ull}},	// 10^15
  {{0x002386f26fc10000ull, 0x0000000000000000ull}},	// 10^16
  {{0x016345785d8a0000ull, 0x0000000000000000ull}},	// 10^17
  {{0x0de0b6b3a7640000ull, 0x0000000000000000ull}},	// 10^18
  {{0x8ac7230489e80000ull, 0x0000000000000000ull}},	// 10^19
  {{0x6bc75e2d63100000ull, 0x0000000000000005ull}},	// 10^20
  {{0x35c9adc5dea00000ull, 0x0000000000000036ull}},	// 10^21
  {{0x19e0c9bab2400000ull, 0x000000000000021eull}},	// 10^22
  {{0x02c7e14af6800000ull, 0x000000000000152dull}},	// 10^23
  {{0x1bcecceda1000000ull, 0x000000000000d3c2ull}},	// 10^24
  {{0x161401484a000000ull, 0x0000000000084595ull}},	// 10^25
  {{0xdcc80cd2e4000000ull, 0x000000000052b7d2ull}},	// 10^26
  {{0x9fd0803ce8000000ull, 0x00000000033b2e3cull}},	// 10^27
  {{0x3e25026110000000ull, 0x00000000204fce5eull}},	// 10^28
  {{0x6d7217caa0000000ull, 0x00000001431e0faeull}},	// 10^29
  {{0x4674edea40000000ull, 0x0000000c9f2c9cd0ull}},	// 10^30
  {{0xc0914b2680000000ull, 0x0000007e37be2022ull}},	// 10^31
  {{0x85acef8100000000ull, 0x000004ee2d6d415bull}},	// 10^32
  {{0x38c15b0a00000000ull, 0x0000314dc6448d93ull}},	// 10^33
  {{0x378d8e6400000000ull, 0x0001ed09bead87c0ull}},	// 10^34
  {{0x2b878fe800000000ull, 0x0013426172c74d82ull}},	// 10^35
  {{0xb34b9f1000000000ull, 0x00c097ce7bc90715ull}},	// 10^36
  {{0x00f436a000000000ull, 0x0785ee10d5da46d9ull}},	// 10^37
  {{0x098a224000000000ull, 0x4b3b4ca85a86c47aull}},	// 10^38
};


int bid_estimate_bin_expon[] = {
  0,	// 10^0
  3,	// 10^1
  6,	// 10^2
  9,	// 10^3
  13,	// 10^4
  16,	// 10^5
  19,	// 10^6
  23,	// 10^7
  26,	// 10^8
  29,	// 10^9
  33,	// 10^10
  36,	// 10^11
  39,	// 10^12
  43,	// 10^13
  46,	// 10^14
  49,	// 10^15
  53	// 10^16
};


BID_UINT64 bid_power10_index_binexp[] = {
  0x000000000000000aull,
  0x000000000000000aull,
  0x000000000000000aull,
  0x000000000000000aull,
  0x0000000000000064ull,
  0x0000000000000064ull,
  0x0000000000000064ull,
  0x00000000000003e8ull,
  0x00000000000003e8ull,
  0x00000000000003e8ull,
  0x0000000000002710ull,
  0x0000000000002710ull,
  0x0000000000002710ull,
  0x0000000000002710ull,
  0x00000000000186a0ull,
  0x00000000000186a0ull,
  0x00000000000186a0ull,
  0x00000000000f4240ull,
  0x00000000000f4240ull,
  0x00000000000f4240ull,
  0x0000000000989680ull,
  0x0000000000989680ull,
  0x0000000000989680ull,
  0x0000000000989680ull,
  0x0000000005f5e100ull,
  0x0000000005f5e100ull,
  0x0000000005f5e100ull,
  0x000000003b9aca00ull,
  0x000000003b9aca00ull,
  0x000000003b9aca00ull,
  0x00000002540be400ull,
  0x00000002540be400ull,
  0x00000002540be400ull,
  0x00000002540be400ull,
  0x000000174876e800ull,
  0x000000174876e800ull,
  0x000000174876e800ull,
  0x000000e8d4a51000ull,
  0x000000e8d4a51000ull,
  0x000000e8d4a51000ull,
  0x000009184e72a000ull,
  0x000009184e72a000ull,
  0x000009184e72a000ull,
  0x000009184e72a000ull,
  0x00005af3107a4000ull,
  0x00005af3107a4000ull,
  0x00005af3107a4000ull,
  0x00038d7ea4c68000ull,
  0x00038d7ea4c68000ull,
  0x00038d7ea4c68000ull,
  0x002386f26fc10000ull,
  0x002386f26fc10000ull,
  0x002386f26fc10000ull,
  0x002386f26fc10000ull,
  0x016345785d8a0000ull,
  0x016345785d8a0000ull,
  0x016345785d8a0000ull,
  0x0de0b6b3a7640000ull,
  0x0de0b6b3a7640000ull,
  0x0de0b6b3a7640000ull,
  0x8ac7230489e80000ull,
  0x8ac7230489e80000ull,
  0x8ac7230489e80000ull,
  0x8ac7230489e80000ull
};


int bid_short_recip_scale[] = {
  1,
  65 - 64,
  69 - 64,
  71 - 64,
  75 - 64,
  78 - 64,
  81 - 64,
  85 - 64,
  88 - 64,
  91 - 64,
  95 - 64,
  98 - 64,
  101 - 64,
  105 - 64,
  108 - 64,
  111 - 64,
  115 - 64,	//114 - 64
  118 - 64
};


BID_UINT64 bid_reciprocals10_64[] = {
  1ull,	// dummy value for 0 extra digits
  0x3333333333333334ull,	// 1 extra digit
  0x51eb851eb851eb86ull,
  0x20c49ba5e353f7cfull,
  0x346dc5d63886594bull,
  0x29f16b11c6d1e109ull,
  0x218def416bdb1a6eull,
  0x35afe535795e90b0ull,
  0x2af31dc4611873c0ull,
  0x225c17d04dad2966ull,
  0x36f9bfb3af7b7570ull,
  0x2bfaffc2f2c92ac0ull,
  0x232f33025bd42233ull,
  0x384b84d092ed0385ull,
  0x2d09370d42573604ull,
  0x24075f3dceac2b37ull,
  0x39a5652fb1137857ull,
  0x2e1dea8c8da92d13ull
};

int bid_bid_bid_recip_scale32 [] = 
{
	1, 
	33-32,
	35-32,
	39-32,
	43-32,
	46-32,
	50-32, 
	53-32,
	57-32
};


BID_UINT64 bid_bid_reciprocals10_32[] = {
	1ull, //dummy,
	0x33333334ull,
	0x147AE148ull,
    0x20C49BA6ull,
	0x346DC5D7ull, //4
	0x29F16B12ull,
	0x431BDE83ull,
	0x35AFE536ull,
	0x55E63B89ull
};



BID_UINT128 bid_power10_index_binexp_128[] = {
  {{0x000000000000000aull, 0x0000000000000000ull}},
  {{0x000000000000000aull, 0x0000000000000000ull}},
  {{0x000000000000000aull, 0x0000000000000000ull}},
  {{0x000000000000000aull, 0x0000000000000000ull}},
  {{0x0000000000000064ull, 0x0000000000000000ull}},
  {{0x0000000000000064ull, 0x0000000000000000ull}},
  {{0x0000000000000064ull, 0x0000000000000000ull}},
  {{0x00000000000003e8ull, 0x0000000000000000ull}},
  {{0x00000000000003e8ull, 0x0000000000000000ull}},
  {{0x00000000000003e8ull, 0x0000000000000000ull}},
  {{0x0000000000002710ull, 0x0000000000000000ull}},
  {{0x0000000000002710ull, 0x0000000000000000ull}},
  {{0x0000000000002710ull, 0x0000000000000000ull}},
  {{0x0000000000002710ull, 0x0000000000000000ull}},
  {{0x00000000000186a0ull, 0x0000000000000000ull}},
  {{0x00000000000186a0ull, 0x0000000000000000ull}},
  {{0x00000000000186a0ull, 0x0000000000000000ull}},
  {{0x00000000000f4240ull, 0x0000000000000000ull}},
  {{0x00000000000f4240ull, 0x0000000000000000ull}},
  {{0x00000000000f4240ull, 0x0000000000000000ull}},
  {{0x0000000000989680ull, 0x0000000000000000ull}},
  {{0x0000000000989680ull, 0x0000000000000000ull}},
  {{0x0000000000989680ull, 0x0000000000000000ull}},
  {{0x0000000000989680ull, 0x0000000000000000ull}},
  {{0x0000000005f5e100ull, 0x0000000000000000ull}},
  {{0x0000000005f5e100ull, 0x0000000000000000ull}},
  {{0x0000000005f5e100ull, 0x0000000000000000ull}},
  {{0x000000003b9aca00ull, 0x0000000000000000ull}},
  {{0x000000003b9aca00ull, 0x0000000000000000ull}},
  {{0x000000003b9aca00ull, 0x0000000000000000ull}},
  {{0x00000002540be400ull, 0x0000000000000000ull}},
  {{0x00000002540be400ull, 0x0000000000000000ull}},
  {{0x00000002540be400ull, 0x0000000000000000ull}},
  {{0x00000002540be400ull, 0x0000000000000000ull}},
  {{0x000000174876e800ull, 0x0000000000000000ull}},
  {{0x000000174876e800ull, 0x0000000000000000ull}},
  {{0x000000174876e800ull, 0x0000000000000000ull}},
  {{0x000000e8d4a51000ull, 0x0000000000000000ull}},
  {{0x000000e8d4a51000ull, 0x0000000000000000ull}},
  {{0x000000e8d4a51000ull, 0x0000000000000000ull}},
  {{0x000009184e72a000ull, 0x0000000000000000ull}},
  {{0x000009184e72a000ull, 0x0000000000000000ull}},
  {{0x000009184e72a000ull, 0x0000000000000000ull}},
  {{0x000009184e72a000ull, 0x0000000000000000ull}},
  {{0x00005af3107a4000ull, 0x0000000000000000ull}},
  {{0x00005af3107a4000ull, 0x0000000000000000ull}},
  {{0x00005af3107a4000ull, 0x0000000000000000ull}},
  {{0x00038d7ea4c68000ull, 0x0000000000000000ull}},
  {{0x00038d7ea4c68000ull, 0x0000000000000000ull}},
  {{0x00038d7ea4c68000ull, 0x0000000000000000ull}},
  {{0x002386f26fc10000ull, 0x0000000000000000ull}},
  {{0x002386f26fc10000ull, 0x0000000000000000ull}},
  {{0x002386f26fc10000ull, 0x0000000000000000ull}},
  {{0x002386f26fc10000ull, 0x0000000000000000ull}},
  {{0x016345785d8a0000ull, 0x0000000000000000ull}},
  {{0x016345785d8a0000ull, 0x0000000000000000ull}},
  {{0x016345785d8a0000ull, 0x0000000000000000ull}},
  {{0x0de0b6b3a7640000ull, 0x0000000000000000ull}},
  {{0x0de0b6b3a7640000ull, 0x0000000000000000ull}},
  {{0x0de0b6b3a7640000ull, 0x0000000000000000ull}},
  {{0x8ac7230489e80000ull, 0x0000000000000000ull}},
  {{0x8ac7230489e80000ull, 0x0000000000000000ull}},
  {{0x8ac7230489e80000ull, 0x0000000000000000ull}},
  {{0x8ac7230489e80000ull, 0x0000000000000000ull}},
  {{0x6bc75e2d63100000ull, 0x0000000000000005ull}},	// 10^20
  {{0x6bc75e2d63100000ull, 0x0000000000000005ull}},	// 10^20
  {{0x6bc75e2d63100000ull, 0x0000000000000005ull}},	// 10^20
  {{0x35c9adc5dea00000ull, 0x0000000000000036ull}},	// 10^21
  {{0x35c9adc5dea00000ull, 0x0000000000000036ull}},	// 10^21
  {{0x35c9adc5dea00000ull, 0x0000000000000036ull}},	// 10^21
  {{0x19e0c9bab2400000ull, 0x000000000000021eull}},	// 10^22
  {{0x19e0c9bab2400000ull, 0x000000000000021eull}},	// 10^22
  {{0x19e0c9bab2400000ull, 0x000000000000021eull}},	// 10^22
  {{0x19e0c9bab2400000ull, 0x000000000000021eull}},	// 10^22
  {{0x02c7e14af6800000ull, 0x000000000000152dull}},	// 10^23
  {{0x02c7e14af6800000ull, 0x000000000000152dull}},	// 10^23
  {{0x02c7e14af6800000ull, 0x000000000000152dull}},	// 10^23
  {{0x1bcecceda1000000ull, 0x000000000000d3c2ull}},	// 10^24
  {{0x1bcecceda1000000ull, 0x000000000000d3c2ull}},	// 10^24
  {{0x1bcecceda1000000ull, 0x000000000000d3c2ull}},	// 10^24
  {{0x161401484a000000ull, 0x0000000000084595ull}},	// 10^25
  {{0x161401484a000000ull, 0x0000000000084595ull}},	// 10^25
  {{0x161401484a000000ull, 0x0000000000084595ull}},	// 10^25
  {{0x161401484a000000ull, 0x0000000000084595ull}},	// 10^25
  {{0xdcc80cd2e4000000ull, 0x000000000052b7d2ull}},	// 10^26
  {{0xdcc80cd2e4000000ull, 0x000000000052b7d2ull}},	// 10^26
  {{0xdcc80cd2e4000000ull, 0x000000000052b7d2ull}},	// 10^26
  {{0x9fd0803ce8000000ull, 0x00000000033b2e3cull}},	// 10^27
  {{0x9fd0803ce8000000ull, 0x00000000033b2e3cull}},	// 10^27
  {{0x9fd0803ce8000000ull, 0x00000000033b2e3cull}},	// 10^27
  {{0x3e25026110000000ull, 0x00000000204fce5eull}},	// 10^28
  {{0x3e25026110000000ull, 0x00000000204fce5eull}},	// 10^28
  {{0x3e25026110000000ull, 0x00000000204fce5eull}},	// 10^28
  {{0x3e25026110000000ull, 0x00000000204fce5eull}},	// 10^28
  {{0x6d7217caa0000000ull, 0x00000001431e0faeull}},	// 10^29
  {{0x6d7217caa0000000ull, 0x00000001431e0faeull}},	// 10^29
  {{0x6d7217caa0000000ull, 0x00000001431e0faeull}},	// 10^29
  {{0x4674edea40000000ull, 0x0000000c9f2c9cd0ull}},	// 10^30
  {{0x4674edea40000000ull, 0x0000000c9f2c9cd0ull}},	// 10^30
  {{0x4674edea40000000ull, 0x0000000c9f2c9cd0ull}},	// 10^30
  {{0xc0914b2680000000ull, 0x0000007e37be2022ull}},	// 10^31
  {{0xc0914b2680000000ull, 0x0000007e37be2022ull}},	// 10^31
  {{0xc0914b2680000000ull, 0x0000007e37be2022ull}},	// 10^31
  {{0x85acef8100000000ull, 0x000004ee2d6d415bull}},	// 10^32
  {{0x85acef8100000000ull, 0x000004ee2d6d415bull}},	// 10^32
  {{0x85acef8100000000ull, 0x000004ee2d6d415bull}},	// 10^32
  {{0x85acef8100000000ull, 0x000004ee2d6d415bull}},	// 10^32
  {{0x38c15b0a00000000ull, 0x0000314dc6448d93ull}},	// 10^33
  {{0x38c15b0a00000000ull, 0x0000314dc6448d93ull}},	// 10^33
  {{0x38c15b0a00000000ull, 0x0000314dc6448d93ull}},	// 10^33, entry 112
  {{0x378d8e6400000000ull, 0x0001ed09bead87c0ull}},	// 10^34
  {{0x378d8e6400000000ull, 0x0001ed09bead87c0ull}},	// 10^34
  {{0x378d8e6400000000ull, 0x0001ed09bead87c0ull}},	// 10^34
  {{0x2b878fe800000000ull, 0x0013426172c74d82ull}},	// 10^35
  {{0x2b878fe800000000ull, 0x0013426172c74d82ull}},	// 10^35
  {{0x2b878fe800000000ull, 0x0013426172c74d82ull}},	// 10^35
  {{0x2b878fe800000000ull, 0x0013426172c74d82ull}},	// 10^35
  {{0xb34b9f1000000000ull, 0x00c097ce7bc90715ull}},	// 10^36
  {{0x00f436a000000000ull, 0x0785ee10d5da46d9ull}},	// 10^37
  {{0x00f436a000000000ull, 0x0785ee10d5da46d9ull}},	// 10^37
  {{0x00f436a000000000ull, 0x0785ee10d5da46d9ull}},	// 10^37
  {{0x098a224000000000ull, 0x4b3b4ca85a86c47aull}},	// 10^38
  {{0x098a224000000000ull, 0x4b3b4ca85a86c47aull}},	// 10^38
  {{0x098a224000000000ull, 0x4b3b4ca85a86c47aull}},	// 10^38
  {{0x098a224000000000ull, 0x4b3b4ca85a86c47aull}},	// 10^38
};
