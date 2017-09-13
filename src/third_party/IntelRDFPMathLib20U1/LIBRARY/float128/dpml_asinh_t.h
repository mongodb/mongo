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


#define NUM_ASYM_TERM               9
#define EVALUATE_ASYM_RANGE_POLYNOMIAL(x,c,y) \
                         POLY_9(x,c,y)
static const TABLE_UNION asym_range_coef[] = { 
	DATA_1x2( 0x00000000, 0x3fd00000 ),
	DATA_1x2( 0xffffff22, 0xbfb7ffff ),
	DATA_1x2( 0xaaa7ee0d, 0x3faaaaaa ),
	DATA_1x2( 0xfe550a8e, 0xbfa17fff ),
	DATA_1x2( 0x2e10fdeb, 0x3f993332 ),
	DATA_1x2( 0x0379386c, 0xbf933fd3 ),
	DATA_1x2( 0x9bd2ab2a, 0x3f8e9b6b ),
	DATA_1x2( 0x1f9ae66a, 0xbf8896b7 ),
	DATA_1x2( 0xbb3092b8, 0x3f8074fb ),
}; 

static const TABLE_UNION asinh_tab[] = { 
	DATA_3x2( 0x6d3d527b, 0x3fdd6820, 0xa1f04407, 0x3fdc75a1, 0x5b923501, 0x3ff19bb0 ),
	DATA_3x2( 0xd9e832cb, 0x3fde636b, 0xaeffc643, 0x3fdd594e, 0x34068fdc, 0x3ff1b647 ),
	DATA_3x2( 0x90d5d4a4, 0x3fdf6824, 0x2477c478, 0x3fde4419, 0x8cbcb119, 0x3ff1d29d ),
	DATA_3x2( 0xf65f8a34, 0x3fe03b5e, 0xed9b82af, 0x3fdf3639, 0xd8c901a6, 0x3ff1f0d1 ),
	DATA_3x2( 0x74e10493, 0x3fe0c7d9, 0xde842f51, 0x3fe017f5, 0xbfcb3824, 0x3ff21104 ),
	DATA_3x2( 0x6691168f, 0x3fe159c3, 0x8d7850cd, 0x3fe098b5, 0x4b26026c, 0x3ff23359 ),
	DATA_3x2( 0xeee50f8f, 0x3fe1f162, 0x39e41362, 0x3fe11d7b, 0x175d0cda, 0x3ff257f5 ),
	DATA_3x2( 0x17b33550, 0x3fe28f03, 0x13b333f1, 0x3fe1a667, 0x8a0a6bfe, 0x3ff27f00 ),
	DATA_3x2( 0x4114951c, 0x3fe332f4, 0x4c50cdf2, 0x3fe2339a, 0x0cea1f8b, 0x3ff2a8a7 ),
	DATA_3x2( 0x9cc30cb5, 0x3fe3dd8c, 0x1eb35409, 0x3fe2c537, 0x4e89ed79, 0x3ff2d517 ),
	DATA_3x2( 0xb63f8378, 0x3fe48f28, 0xd7a8eea6, 0x3fe35b60, 0x893d7d21, 0x3ff30483 ),
	DATA_3x2( 0x093525ee, 0x3fe5482c, 0xde663617, 0x3fe3f63b, 0xd10aa2f8, 0x3ff33721 ),
	DATA_3x2( 0xa7bd14a9, 0x3fe60901, 0xbd5967c7, 0x3fe495ed, 0x6958abd3, 0x3ff36d2c ),
	DATA_3x2( 0xf25abec0, 0x3fe6d21c, 0x2b443301, 0x3fe53a9d, 0x23477890, 0x3ff3a6e2 ),
	DATA_3x2( 0x63c55e3c, 0x3fe7a3fa, 0x149e5498, 0x3fe5e472, 0xc5b0f30a, 0x3ff3e486 ),
	DATA_3x2( 0x72d7ac52, 0x3fe87f20, 0xa543474d, 0x3fe69395, 0x7ff956b9, 0x3ff42663 ),
	DATA_3x2( 0x8d4d443a, 0x3fe96420, 0x526d617b, 0x3fe74832, 0x68f99efb, 0x3ff46cc7 ),
	DATA_3x2( 0x2e4ece6a, 0x3fea5398, 0xe500cc87, 0x3fe80273, 0x0b7bedbc, 0x3ff4b808 ),
	DATA_3x2( 0x1434b227, 0x3feb4e32, 0x8428d2ef, 0x3fe8c287, 0x01f4c9d2, 0x3ff50882 ),
	DATA_3x2( 0x995dd13f, 0x3fec54a7, 0xc04a1959, 0x3fe9889b, 0xa35ebc97, 0x3ff55e99 ),
	DATA_3x2( 0x347f19d5, 0x3fed67c2, 0x9e4c6a4a, 0x3fea54e0, 0xc3614f4d, 0x3ff5babb ),
	DATA_3x2( 0x256c6268, 0x3fee885d, 0xa33ecda3, 0x3feb2787, 0x883a238f, 0x3ff61d5e ),
	DATA_3x2( 0x540e7e6a, 0x3fefb767, 0xe058c40f, 0x3fec00c3, 0x59389640, 0x3ff68702 ),
	DATA_3x2( 0xb404c831, 0x3ff07af2, 0xff5b8a40, 0x3fece0c9, 0xe903f972, 0x3ff6f832 ),
	DATA_3x2( 0x1043194f, 0x3ff1227a, 0x4f566682, 0x3fedc7d0, 0x5f5c6257, 0x3ff77188 ),
	DATA_3x2( 0x7a52c2e6, 0x3ff1d2e5, 0xd1d1198d, 0x3feeb60e, 0xa691079a, 0x3ff7f3a8 ),
	DATA_3x2( 0x833513b2, 0x3ff28cdf, 0x485fa280, 0x3fefabbf, 0xe188885d, 0x3ff87f48 ),
	DATA_3x2( 0x39ab2616, 0x3ff35123, 0xa1514fc9, 0x3ff0548e, 0x0ff314ac, 0x3ff9152f ),
	DATA_3x2( 0x07d98244, 0x3ff4207e, 0x165bda4c, 0x3ff0d733, 0xe718fb1e, 0x3ff9b633 ),
	DATA_3x2( 0xcdb8788e, 0x3ff4fbd1, 0xaf0eb91b, 0x3ff15dec, 0xe6b70f89, 0x3ffa6344 ),
	DATA_3x2( 0x40f26127, 0x3ff5e417, 0x14872ee9, 0x3ff1e8dc, 0xb2845d4e, 0x3ffb1d66 ),
	DATA_3x2( 0x9c2ca6cc, 0x3ff6da60, 0xf52b6861, 0x3ff27822, 0xba5972e7, 0x3ffbe5b7 ),
	DATA_3x2( 0xa9506a90, 0x3ff7dfdc, 0x0cd4c3a5, 0x3ff30be4, 0x3c8aca91, 0x3ffcbd73 ),
	DATA_3x2( 0x3446ec2a, 0x3ff8f5da, 0x2d3b69ba, 0x3ff3a443, 0xafe8adc6, 0x3ffda5f4 ),
	DATA_3x2( 0xf5d25d84, 0x3ffa1dcb, 0x46a54508, 0x3ff44165, 0xa50d1794, 0x3ffea0bb ),
	DATA_3x2( 0x08c5c47b, 0x3ffb594d, 0x70da6f3b, 0x3ff4e370, 0x313bf030, 0x3fffaf70 ),
	DATA_3x2( 0xfef1e980, 0x3ffcaa25, 0xf46142ab, 0x3ff58a8b, 0xfc17b03e, 0x400069f3 ),
	DATA_3x2( 0xaec20c74, 0x3ffe1252, 0x54044cc2, 0x3ff636e0, 0xf6e87622, 0x40010815 ),
	DATA_3x2( 0xd6d6561d, 0x3fff9408, 0x56a46f2c, 0x3ff6e897, 0x769acff2, 0x4001b33f ),
	DATA_3x2( 0xdd079ca5, 0x400098df, 0x115992a6, 0x3ff79fdc, 0xa3adb0d1, 0x40026cb2 ),
	DATA_3x2( 0x6e4faa8f, 0x4001771c, 0xf1e45ef6, 0x3ff85cda, 0xb925404f, 0x400335d6 ),
	DATA_3x2( 0x0906e429, 0x40026645, 0xc9738237, 0x3ff91fc1, 0x044ae03f, 0x4004103d ),
	DATA_3x2( 0x8a6ce21f, 0x40036814, 0xd7bf1e43, 0x3ff9e8bf, 0xa8fe6fa6, 0x4004fda6 ),
	DATA_3x2( 0x757c9f4b, 0x40047e7c, 0xd67d1859, 0x3ffab805, 0x4c40c585, 0x4006000b ),
	DATA_3x2( 0xb38c721d, 0x4005abac, 0x0530ffee, 0x3ffb8dc6, 0xccf6fa91, 0x400719a0 ),
	DATA_3x2( 0x92e650f3, 0x4006f21c, 0x355a87ef, 0x3ffc6a34, 0x3a8c2179, 0x40084ce4 ),
	DATA_3x2( 0x3c2ca1d3, 0x40085495, 0xd7055c2c, 0x3ffd4d85, 0x4255146a, 0x40099ca4 ),
	DATA_3x2( 0xe29252a1, 0x4009d63d, 0x05bd870d, 0x3ffe37f2, 0x57cdcccf, 0x400b0c0d ),
	DATA_3x2( 0x0086b713, 0x400b7aaa, 0x95eb7345, 0x3fff29b1, 0xe962c171, 0x400c9eb7 ),
	DATA_3x2( 0x0307617c, 0x400d45ea, 0x914d6772, 0x4000117f, 0x040b9ce4, 0x400e58b9 ),
	DATA_3x2( 0x90439eff, 0x400ea898, 0x5dc3c4bb, 0x40006cff, 0xa64bf44b, 0x400faf65 ),
}; 

#define TABLE_ENTRY_SIZE   51 
#define K   4 
#define OFFSET_IND   16348 
#define MAX_SMALL_INDEX -394 
#define MAX_POLY_INDEX 0 
#define MAX_REDUCE_INDEX 51 
#define MAX_ASYM_INDEX 456 
#define HALF_HUGE_INDEX 16387 
static const U_INT_8 asinh_index_table[] = { 
	0,  1,  2,  3,  4,  6,  8,  9,  
	11,  12,  13,  14,  16,  17,  18,  19,  
	20,  21,  21,  22,  24,  25,  27,  28,  
	29,  30,  31,  32,  33,  34,  35,  36,  
	36,  37,  38,  38,  39,  41,  42,  43,  
	43,  44,  45,  46,  46,  47,  48,  48,  
	49,  49,  50,  0,  0,  0,  0,  0,  
}; 

#define EVALUATE_POLY_RANGE_POLYNOMIAL(x,c,y) \
                         ODD_POLY_25_U(x,c,y)
static const TABLE_UNION poly_range_coef[] = { 
	DATA_1x2( 0x55555555, 0xbfc55555 ),
	DATA_1x2( 0x333332b4, 0x3fb33333 ),
	DATA_1x2( 0xb6da83f1, 0xbfa6db6d ),
	DATA_1x2( 0xc673bde2, 0x3f9f1c71 ),
	DATA_1x2( 0x0f051c74, 0xbf96e8ba ),
	DATA_1x2( 0xd1e50a63, 0x3f91c4e8 ),
	DATA_1x2( 0xbb354095, 0xbf8c991b ),
	DATA_1x2( 0xf4225046, 0x3f87a295 ),
	DATA_1x2( 0xc7bd384c, 0xbf83ce52 ),
	DATA_1x2( 0xe54f617d, 0x3f802b59 ),
	DATA_1x2( 0x92c741ae, 0xbf769f6d ),
	DATA_1x2( 0x6f70f30f, 0x3f6318f8 ),
}; 

#define EVALUATE_REDUCE_RANGE_POLYNOMIAL(x,c,y) \
                         ODD_POLY_13_U(x,c,y)
static const TABLE_UNION reduce_range_coef[] = { 
	DATA_1x2( 0x55555555, 0xbfc55555 ),
	DATA_1x2( 0x33333317, 0x3fb33333 ),
	DATA_1x2( 0xb6d8c295, 0xbfa6db6d ),
	DATA_1x2( 0xafd69af5, 0x3f9f1c71 ),
	DATA_1x2( 0x4a7f48fd, 0xbf96e88c ),
	DATA_1x2( 0x7d131bd8, 0x3f919b2c ),
}; 

static const TABLE_UNION log_2[] = {
        DATA_1x2( 0xfefa39ef, 0x3fe62e42 ) 
};

