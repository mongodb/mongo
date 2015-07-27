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




/* Define default table name */

#if !defined(TABLE_NAME)
#   define TABLE_NAME	__pow_t_table
#endif

#include "dpml_private.h"


#if !DEFINE_SYMBOLIC_CONSTANTS

    const unsigned int TABLE_NAME[] = { 

	/* 
	 * Tj = 2^(j/2^POW2_K) and Rj = [2^(j/2^POW2_K) - Tj]/Tj.
	 *
	 * offset                            row
	 */
	/* 0000 */ DATA_1x2( 0x00000000, 0x3ff00000 ), /* 000 */
	/* 0008 */ DATA_1x2( 0x00000000, 0x00000000 ),
	/* 0016 */ DATA_1x2( 0xfa5abcbf, 0x3ff00b1a ), /* 001 */
	/* 0024 */ DATA_1x2( 0xc61851ac, 0xbc84e82f ),
	/* 0032 */ DATA_1x2( 0xa9fb3335, 0x3ff0163d ), /* 002 */
	/* 0040 */ DATA_1x2( 0x1a88bf6d, 0x3c9b3b4f ),
	/* 0048 */ DATA_1x2( 0x143b0281, 0x3ff02168 ), /* 003 */
	/* 0056 */ DATA_1x2( 0xd8521d32, 0xbc82985d ),
	/* 0064 */ DATA_1x2( 0x3e778061, 0x3ff02c9a ), /* 004 */
	/* 0072 */ DATA_1x2( 0x9cd8dc5d, 0xbc716013 ),
	/* 0080 */ DATA_1x2( 0x2e11bbcc, 0x3ff037d4 ), /* 005 */
	/* 0088 */ DATA_1x2( 0x7061bfbd, 0x3c651e61 ),
	/* 0096 */ DATA_1x2( 0xe86e7f85, 0x3ff04315 ), /* 006 */
	/* 0104 */ DATA_1x2( 0x108766d1, 0xbc905e7a ),
	/* 0112 */ DATA_1x2( 0x72f654b1, 0x3ff04e5f ), /* 007 */
	/* 0120 */ DATA_1x2( 0x437fa426, 0x3c845fad ),
	/* 0128 */ DATA_1x2( 0xd3158574, 0x3ff059b0 ), /* 008 */
	/* 0136 */ DATA_1x2( 0x3567f613, 0x3c8cd252 ),
	/* 0144 */ DATA_1x2( 0x0e3c1f89, 0x3ff0650a ), /* 009 */
	/* 0152 */ DATA_1x2( 0x642b232f, 0xbc954529 ),
	/* 0160 */ DATA_1x2( 0x29ddf6de, 0x3ff0706b ), /* 010 */
	/* 0168 */ DATA_1x2( 0x23f98efa, 0xbc8bce80 ),
	/* 0176 */ DATA_1x2( 0x2b72a836, 0x3ff07bd4 ), /* 011 */
	/* 0184 */ DATA_1x2( 0x8ef5c32e, 0x3c829370 ),
	/* 0192 */ DATA_1x2( 0x18759bc8, 0x3ff08745 ), /* 012 */
	/* 0200 */ DATA_1x2( 0x61e6c861, 0x3c60f74e ),
	/* 0208 */ DATA_1x2( 0xf66607e0, 0x3ff092bd ), /* 013 */
	/* 0216 */ DATA_1x2( 0x0905b2a5, 0xbc95b928 ),
	/* 0224 */ DATA_1x2( 0xcac6f383, 0x3ff09e3e ), /* 014 */
	/* 0232 */ DATA_1x2( 0x5b33d398, 0x3c90a3e4 ),
	/* 0240 */ DATA_1x2( 0x9b1f3919, 0x3ff0a9c7 ), /* 015 */
	/* 0248 */ DATA_1x2( 0x32c4b7e7, 0x3c84f31f ),
	/* 0256 */ DATA_1x2( 0x6cf9890f, 0x3ff0b558 ), /* 016 */
	/* 0264 */ DATA_1x2( 0x5d837b6c, 0x3c979aa6 ),
	/* 0272 */ DATA_1x2( 0x45e46c85, 0x3ff0c0f1 ), /* 017 */
	/* 0280 */ DATA_1x2( 0x30d06420, 0x3c9407fb ),
	/* 0288 */ DATA_1x2( 0x2b7247f7, 0x3ff0cc92 ), /* 018 */
	/* 0296 */ DATA_1x2( 0x92fdeffb, 0x3c8eb51a ),
	/* 0304 */ DATA_1x2( 0x23395dec, 0x3ff0d83b ), /* 019 */
	/* 0312 */ DATA_1x2( 0xb3b9911c, 0xbc9a5d04 ),
	/* 0320 */ DATA_1x2( 0x32d3d1a2, 0x3ff0e3ec ), /* 020 */
	/* 0328 */ DATA_1x2( 0x702f9cd1, 0x3c3ebe3d ),
	/* 0336 */ DATA_1x2( 0x5fdfa9c5, 0x3ff0efa5 ), /* 021 */
	/* 0344 */ DATA_1x2( 0xf0739547, 0xbc937a01 ),
	/* 0352 */ DATA_1x2( 0xaffed31b, 0x3ff0fb66 ), /* 022 */
	/* 0360 */ DATA_1x2( 0x89906e0b, 0xbc6a0334 ),
	/* 0368 */ DATA_1x2( 0x28d7233e, 0x3ff10730 ), /* 023 */
	/* 0376 */ DATA_1x2( 0xb04ef0a5, 0x3c8b8268 ),
	/* 0384 */ DATA_1x2( 0xd0125b51, 0x3ff11301 ), /* 024 */
	/* 0392 */ DATA_1x2( 0x2a2fbd0e, 0xbc955652 ),
	/* 0400 */ DATA_1x2( 0xab5e2ab6, 0x3ff11edb ), /* 025 */
	/* 0408 */ DATA_1x2( 0x44a2ebcc, 0xbc9ac46e ),
	/* 0416 */ DATA_1x2( 0xc06c31cc, 0x3ff12abd ), /* 026 */
	/* 0424 */ DATA_1x2( 0x8c4eea55, 0xbc5080ef ),
	/* 0432 */ DATA_1x2( 0x14f204ab, 0x3ff136a8 ), /* 027 */
	/* 0440 */ DATA_1x2( 0x90c9f860, 0xbc65704e ),
	/* 0448 */ DATA_1x2( 0xaea92de0, 0x3ff1429a ), /* 028 */
	/* 0456 */ DATA_1x2( 0xb9d5f416, 0xbc91c923 ),
	/* 0464 */ DATA_1x2( 0x934f312e, 0x3ff14e95 ), /* 029 */
	/* 0472 */ DATA_1x2( 0x57e46280, 0xbc897cea ),
	/* 0480 */ DATA_1x2( 0xc8a58e51, 0x3ff15a98 ), /* 030 */
	/* 0488 */ DATA_1x2( 0xe95c55af, 0x3c80d3e3 ),
	/* 0496 */ DATA_1x2( 0x5471c3c2, 0x3ff166a4 ), /* 031 */
	/* 0504 */ DATA_1x2( 0x29e2b9d2, 0x3c56f014 ),
	/* 0512 */ DATA_1x2( 0x3c7d517b, 0x3ff172b8 ), /* 032 */
	/* 0520 */ DATA_1x2( 0xeaa59348, 0xbc801b15 ),
	/* 0528 */ DATA_1x2( 0x8695bbc0, 0x3ff17ed4 ), /* 033 */
	/* 0536 */ DATA_1x2( 0x2459034b, 0x3c6e653b ),
	/* 0544 */ DATA_1x2( 0x388c8dea, 0x3ff18af9 ), /* 034 */
	/* 0552 */ DATA_1x2( 0x55de323d, 0xbc8f1ff0 ),
	/* 0560 */ DATA_1x2( 0x58375d2f, 0x3ff19726 ), /* 035 */
	/* 0568 */ DATA_1x2( 0xa345b7dc, 0x3c92cc7e ),
	/* 0576 */ DATA_1x2( 0xeb6fcb75, 0x3ff1a35b ), /* 036 */
	/* 0584 */ DATA_1x2( 0x3f1353bf, 0x3c8b898c ),
	/* 0592 */ DATA_1x2( 0xf8138a1c, 0x3ff1af99 ), /* 037 */
	/* 0600 */ DATA_1x2( 0x2876ea9e, 0x3c957bfb ),
	/* 0608 */ DATA_1x2( 0x84045cd4, 0x3ff1bbe0 ), /* 038 */
	/* 0616 */ DATA_1x2( 0x7611eb27, 0xbc96d99c ),
	/* 0624 */ DATA_1x2( 0x95281c6b, 0x3ff1c82f ), /* 039 */
	/* 0632 */ DATA_1x2( 0x73af2154, 0x3c8cdc18 ),
	/* 0640 */ DATA_1x2( 0x3168b9aa, 0x3ff1d487 ), /* 040 */
	/* 0648 */ DATA_1x2( 0x3e3a2f5f, 0x3c9aecf7 ),
	/* 0656 */ DATA_1x2( 0x5eb44027, 0x3ff1e0e7 ), /* 041 */
	/* 0664 */ DATA_1x2( 0x4653a131, 0xbc949368 ),
	/* 0672 */ DATA_1x2( 0x22fcd91d, 0x3ff1ed50 ), /* 042 */
	/* 0680 */ DATA_1x2( 0xcb86389e, 0xbc8fe782 ),
	/* 0688 */ DATA_1x2( 0x8438ce4d, 0x3ff1f9c1 ), /* 043 */
	/* 0696 */ DATA_1x2( 0x9077520a, 0xbc98e289 ),
	/* 0704 */ DATA_1x2( 0x88628cd6, 0x3ff2063b ), /* 044 */
	/* 0712 */ DATA_1x2( 0x44a6c38c, 0x3c8a6f41 ),
	/* 0720 */ DATA_1x2( 0x3578a819, 0x3ff212be ), /* 045 */
	/* 0728 */ DATA_1x2( 0xd4f59273, 0x3c9120fc ),
	/* 0736 */ DATA_1x2( 0x917ddc96, 0x3ff21f49 ), /* 046 */
	/* 0744 */ DATA_1x2( 0xb0e4047d, 0x3c807a05 ),
	/* 0752 */ DATA_1x2( 0xa27912d1, 0x3ff22bdd ), /* 047 */
	/* 0760 */ DATA_1x2( 0xc188c9b8, 0x3c89b788 ),
	/* 0768 */ DATA_1x2( 0x6e756238, 0x3ff2387a ), /* 048 */
	/* 0776 */ DATA_1x2( 0xe3a8a893, 0x3c968efd ),
	/* 0784 */ DATA_1x2( 0xfb82140a, 0x3ff2451f ), /* 049 */
	/* 0792 */ DATA_1x2( 0xca90ef84, 0x3c877afb ),
	/* 0800 */ DATA_1x2( 0x4fb2a63f, 0x3ff251ce ), /* 050 */
	/* 0808 */ DATA_1x2( 0xf274487d, 0x3c875e18 ),
	/* 0816 */ DATA_1x2( 0x711ece75, 0x3ff25e85 ), /* 051 */
	/* 0824 */ DATA_1x2( 0x082876ee, 0x3c91512f ),
	/* 0832 */ DATA_1x2( 0x65e27cdd, 0x3ff26b45 ), /* 052 */
	/* 0840 */ DATA_1x2( 0x981fe7f2, 0x3c80472b ),
	/* 0848 */ DATA_1x2( 0x341ddf29, 0x3ff2780e ), /* 053 */
	/* 0856 */ DATA_1x2( 0xc7d75ec5, 0x3c9a02f0 ),
	/* 0864 */ DATA_1x2( 0xe1f56381, 0x3ff284df ), /* 054 */
	/* 0872 */ DATA_1x2( 0x3f71085e, 0xbc96b87b ),
	/* 0880 */ DATA_1x2( 0x7591bb70, 0x3ff291ba ), /* 055 */
	/* 0888 */ DATA_1x2( 0xe78260bf, 0xbc803297 ),
	/* 0896 */ DATA_1x2( 0xf51fdee1, 0x3ff29e9d ), /* 056 */
	/* 0904 */ DATA_1x2( 0x6d09ab31, 0x3c82f7e1 ),
	/* 0912 */ DATA_1x2( 0x66d10f13, 0x3ff2ab8a ), /* 057 */
	/* 0920 */ DATA_1x2( 0x5ccd9fbf, 0xbc95b77e ),
	/* 0928 */ DATA_1x2( 0xd0dad990, 0x3ff2b87f ), /* 058 */
	/* 0936 */ DATA_1x2( 0x1a6fbffb, 0xbc3d219b ),
	/* 0944 */ DATA_1x2( 0x39771b2f, 0x3ff2c57e ), /* 059 */
	/* 0952 */ DATA_1x2( 0x40b4251e, 0xbc91e75c ),
	/* 0960 */ DATA_1x2( 0xa6e4030b, 0x3ff2d285 ), /* 060 */
	/* 0968 */ DATA_1x2( 0x720c0ab3, 0x3c8b3782 ),
	/* 0976 */ DATA_1x2( 0x1f641589, 0x3ff2df96 ), /* 061 */
	/* 0984 */ DATA_1x2( 0xf1f77859, 0x3c98a911 ),
	/* 0992 */ DATA_1x2( 0xa93e2f56, 0x3ff2ecaf ), /* 062 */
	/* 1000 */ DATA_1x2( 0x89cecb8f, 0x3c6e1492 ),
	/* 1008 */ DATA_1x2( 0x4abd886b, 0x3ff2f9d2 ), /* 063 */
	/* 1016 */ DATA_1x2( 0x98db7dbc, 0xbc61e7c9 ),
	/* 1024 */ DATA_1x2( 0x0a31b715, 0x3ff306fe ), /* 064 */
	/* 1032 */ DATA_1x2( 0x4db0abb6, 0x3c834d75 ),
	/* 1040 */ DATA_1x2( 0xedeeb2fd, 0x3ff31432 ), /* 065 */
	/* 1048 */ DATA_1x2( 0x11faadf4, 0x3c85425c ),
	/* 1056 */ DATA_1x2( 0xfc4cd831, 0x3ff32170 ), /* 066 */
	/* 1064 */ DATA_1x2( 0xe2ac744c, 0x3c864201 ),
	/* 1072 */ DATA_1x2( 0x3ba8ea32, 0x3ff32eb8 ), /* 067 */
	/* 1080 */ DATA_1x2( 0xa03e2848, 0xbc979517 ),
	/* 1088 */ DATA_1x2( 0xb26416ff, 0x3ff33c08 ), /* 068 */
	/* 1096 */ DATA_1x2( 0x5dd3f84a, 0x3c8fdd39 ),
	/* 1104 */ DATA_1x2( 0x66e3fa2d, 0x3ff34962 ), /* 069 */
	/* 1112 */ DATA_1x2( 0x46da4bee, 0xbc800e2a ),
	/* 1120 */ DATA_1x2( 0x5f929ff1, 0x3ff356c5 ), /* 070 */
	/* 1128 */ DATA_1x2( 0x3b8e5b04, 0xbc86a380 ),
	/* 1136 */ DATA_1x2( 0xa2de883b, 0x3ff36431 ), /* 071 */
	/* 1144 */ DATA_1x2( 0x03972b34, 0xbc874308 ),
	/* 1152 */ DATA_1x2( 0x373aa9cb, 0x3ff371a7 ), /* 072 */
	/* 1160 */ DATA_1x2( 0xcc4b5069, 0xbc924aed ),
	/* 1168 */ DATA_1x2( 0x231e754a, 0x3ff37f26 ), /* 073 */
	/* 1176 */ DATA_1x2( 0x0ae02d95, 0xbc954de3 ),
	/* 1184 */ DATA_1x2( 0x6d05d866, 0x3ff38cae ), /* 074 */
	/* 1192 */ DATA_1x2( 0x1b512d8f, 0xbc9907f8 ),
	/* 1200 */ DATA_1x2( 0x1b7140ef, 0x3ff39a40 ), /* 075 */
	/* 1208 */ DATA_1x2( 0x7e1c03ec, 0xbc94f248 ),
	/* 1216 */ DATA_1x2( 0x34e59ff7, 0x3ff3a7db ), /* 076 */
	/* 1224 */ DATA_1x2( 0x3e9436d2, 0xbc71d1e8 ),
	/* 1232 */ DATA_1x2( 0xbfec6cf4, 0x3ff3b57f ), /* 077 */
	/* 1240 */ DATA_1x2( 0x32fcb2f4, 0x3c914a54 ),
	/* 1248 */ DATA_1x2( 0xc313a8e5, 0x3ff3c32d ), /* 078 */
	/* 1256 */ DATA_1x2( 0xb3ce1b15, 0xbc991919 ),
	/* 1264 */ DATA_1x2( 0x44ede173, 0x3ff3d0e5 ), /* 079 */
	/* 1272 */ DATA_1x2( 0xa5562a2f, 0x3c79c3bb ),
	/* 1280 */ DATA_1x2( 0x4c123422, 0x3ff3dea6 ), /* 080 */
	/* 1288 */ DATA_1x2( 0xa72a4c6c, 0x3c859f48 ),
	/* 1296 */ DATA_1x2( 0xdf1c5175, 0x3ff3ec70 ), /* 081 */
	/* 1304 */ DATA_1x2( 0x12e21658, 0xbc85a716 ),
	/* 1312 */ DATA_1x2( 0x04ac801c, 0x3ff3fa45 ), /* 082 */
	/* 1320 */ DATA_1x2( 0x7a28698a, 0xbc931260 ),
	/* 1328 */ DATA_1x2( 0xc367a024, 0x3ff40822 ), /* 083 */
	/* 1336 */ DATA_1x2( 0x6f1d24d6, 0x3c86421f ),
	/* 1344 */ DATA_1x2( 0x21f72e2a, 0x3ff4160a ), /* 084 */
	/* 1352 */ DATA_1x2( 0x4817895b, 0xbc58a78f ),
	/* 1360 */ DATA_1x2( 0x2709468a, 0x3ff423fb ), /* 085 */
	/* 1368 */ DATA_1x2( 0x815fce65, 0xbc9348a6 ),
	/* 1376 */ DATA_1x2( 0xd950a897, 0x3ff431f5 ), /* 086 */
	/* 1384 */ DATA_1x2( 0x67499a1b, 0xbc7c2c9b ),
	/* 1392 */ DATA_1x2( 0x3f84b9d4, 0x3ff43ffa ), /* 087 */
	/* 1400 */ DATA_1x2( 0x984d9871, 0x3c835c43 ),
	/* 1408 */ DATA_1x2( 0x6061892d, 0x3ff44e08 ), /* 088 */
	/* 1416 */ DATA_1x2( 0x60c2ac11, 0x3c4363ed ),
	/* 1424 */ DATA_1x2( 0x42a7d232, 0x3ff45c20 ), /* 089 */
	/* 1432 */ DATA_1x2( 0x8d9473a0, 0xbc632afc ),
	/* 1440 */ DATA_1x2( 0xed1d0057, 0x3ff46a41 ), /* 090 */
	/* 1448 */ DATA_1x2( 0x3b0664ef, 0x3c966609 ),
	/* 1456 */ DATA_1x2( 0x668b3237, 0x3ff4786d ), /* 091 */
	/* 1464 */ DATA_1x2( 0x44de020e, 0xbc95fc5e ),
	/* 1472 */ DATA_1x2( 0xb5c13cd0, 0x3ff486a2 ), /* 092 */
	/* 1480 */ DATA_1x2( 0xdaa10379, 0x3c6ecce1 ),
	/* 1488 */ DATA_1x2( 0xe192aed2, 0x3ff494e1 ), /* 093 */
	/* 1496 */ DATA_1x2( 0x8327c42f, 0xbc7ea014 ),
	/* 1504 */ DATA_1x2( 0xf0d7d3de, 0x3ff4a32a ), /* 094 */
	/* 1512 */ DATA_1x2( 0x3f0f1230, 0x3c93ff8e ),
	/* 1520 */ DATA_1x2( 0xea6db7d7, 0x3ff4b17d ), /* 095 */
	/* 1528 */ DATA_1x2( 0xd1a88022, 0xbc7a843a ),
	/* 1536 */ DATA_1x2( 0xd5362a27, 0x3ff4bfda ), /* 096 */
	/* 1544 */ DATA_1x2( 0xbb7aafb0, 0x3c7690ce ),
	/* 1552 */ DATA_1x2( 0xb817c114, 0x3ff4ce41 ), /* 097 */
	/* 1560 */ DATA_1x2( 0xbf144e62, 0x3c892ca3 ),
	/* 1568 */ DATA_1x2( 0x99fddd0d, 0x3ff4dcb2 ), /* 098 */
	/* 1576 */ DATA_1x2( 0xeb54e077, 0x3c931dbd ),
	/* 1584 */ DATA_1x2( 0x81d8abff, 0x3ff4eb2d ), /* 099 */
	/* 1592 */ DATA_1x2( 0xb04aa8b0, 0xbc902c99 ),
	/* 1600 */ DATA_1x2( 0x769d2ca7, 0x3ff4f9b2 ), /* 100 */
	/* 1608 */ DATA_1x2( 0x0071a38f, 0xbc8f9434 ),
	/* 1616 */ DATA_1x2( 0x7f4531ee, 0x3ff50841 ), /* 101 */
	/* 1624 */ DATA_1x2( 0x67e67117, 0x3c73e34f ),
	/* 1632 */ DATA_1x2( 0xa2cf6642, 0x3ff516da ), /* 102 */
	/* 1640 */ DATA_1x2( 0xdc93a34a, 0xbc87decc ),
	/* 1648 */ DATA_1x2( 0xe83f4eef, 0x3ff5257d ), /* 103 */
	/* 1656 */ DATA_1x2( 0x197ba0f0, 0xbc75a3b1 ),
	/* 1664 */ DATA_1x2( 0x569d4f82, 0x3ff5342b ), /* 104 */
	/* 1672 */ DATA_1x2( 0xbd0f3860, 0xbc78dec6 ),
	/* 1680 */ DATA_1x2( 0xf4f6ad27, 0x3ff542e2 ), /* 105 */
	/* 1688 */ DATA_1x2( 0x88075068, 0x3c81bd28 ),
	/* 1696 */ DATA_1x2( 0xca5d920f, 0x3ff551a4 ), /* 106 */
	/* 1704 */ DATA_1x2( 0xec7b5cf6, 0xbc861246 ),
	/* 1712 */ DATA_1x2( 0xdde910d2, 0x3ff56070 ), /* 107 */
	/* 1720 */ DATA_1x2( 0xae89ef8f, 0xbc896be8 ),
	/* 1728 */ DATA_1x2( 0x36b527da, 0x3ff56f47 ), /* 108 */
	/* 1736 */ DATA_1x2( 0x18fdd78d, 0x3c933505 ),
	/* 1744 */ DATA_1x2( 0xdbe2c4cf, 0x3ff57e27 ), /* 109 */
	/* 1752 */ DATA_1x2( 0x90348602, 0xbc88e6ac ),
	/* 1760 */ DATA_1x2( 0xd497c7fd, 0x3ff58d12 ), /* 110 */
	/* 1768 */ DATA_1x2( 0x2f8a9b05, 0x3c7b98b7 ),
	/* 1776 */ DATA_1x2( 0x27ff07cc, 0x3ff59c08 ), /* 111 */
	/* 1784 */ DATA_1x2( 0x1365c3ac, 0xbc91af7f ),
	/* 1792 */ DATA_1x2( 0xdd485429, 0x3ff5ab07 ), /* 112 */
	/* 1800 */ DATA_1x2( 0xe21c5409, 0x3c9063e1 ),
	/* 1808 */ DATA_1x2( 0xfba87a03, 0x3ff5ba11 ), /* 113 */
	/* 1816 */ DATA_1x2( 0x40d1898a, 0xbc943a35 ),
	/* 1824 */ DATA_1x2( 0x8a5946b7, 0x3ff5c926 ), /* 114 */
	/* 1832 */ DATA_1x2( 0x5019c6ea, 0x3c34c785 ),
	/* 1840 */ DATA_1x2( 0x90998b93, 0x3ff5d845 ), /* 115 */
	/* 1848 */ DATA_1x2( 0xddaa8090, 0xbc951f58 ),
	/* 1856 */ DATA_1x2( 0x15ad2148, 0x3ff5e76f ), /* 116 */
	/* 1864 */ DATA_1x2( 0x2b64c035, 0x3c9432e6 ),
	/* 1872 */ DATA_1x2( 0x20dceb71, 0x3ff5f6a3 ), /* 117 */
	/* 1880 */ DATA_1x2( 0x8e50a17c, 0xbc82e164 ),
	/* 1888 */ DATA_1x2( 0xb976dc09, 0x3ff605e1 ), /* 118 */
	/* 1896 */ DATA_1x2( 0x6199769f, 0xbc8ce44a ),
	/* 1904 */ DATA_1x2( 0xe6cdf6f4, 0x3ff6152a ), /* 119 */
	/* 1912 */ DATA_1x2( 0xda98a574, 0x3c95f30e ),
	/* 1920 */ DATA_1x2( 0xb03a5585, 0x3ff6247e ), /* 120 */
	/* 1928 */ DATA_1x2( 0x3bef4da8, 0xbc8c33c5 ),
	/* 1936 */ DATA_1x2( 0x1d1929fd, 0x3ff633dd ), /* 121 */
	/* 1944 */ DATA_1x2( 0xa8a72158, 0x3c917ecd ),
	/* 1952 */ DATA_1x2( 0x34ccc320, 0x3ff64346 ), /* 122 */
	/* 1960 */ DATA_1x2( 0x892be9ae, 0xbc845378 ),
	/* 1968 */ DATA_1x2( 0xfebc8fb7, 0x3ff652b9 ), /* 123 */
	/* 1976 */ DATA_1x2( 0xcee1ae6e, 0xbc9345f3 ),
	/* 1984 */ DATA_1x2( 0x82552225, 0x3ff66238 ), /* 124 */
	/* 1992 */ DATA_1x2( 0x78565858, 0xbc93cedd ),
	/* 2000 */ DATA_1x2( 0xc70833f6, 0x3ff671c1 ), /* 125 */
	/* 2008 */ DATA_1x2( 0xdf910406, 0xbc85c33f ),
	/* 2016 */ DATA_1x2( 0xd44ca973, 0x3ff68155 ), /* 126 */
	/* 2024 */ DATA_1x2( 0x807e1964, 0x3c5710aa ),
	/* 2032 */ DATA_1x2( 0xb19e9538, 0x3ff690f4 ), /* 127 */
	/* 2040 */ DATA_1x2( 0xb5789604, 0x3c81079a ),
	/* 2048 */ DATA_1x2( 0x667f3bcd, 0x3ff6a09e ), /* 128 */
	/* 2056 */ DATA_1x2( 0xbf5e2229, 0xbc93b3ef ),
	/* 2064 */ DATA_1x2( 0xfa75173e, 0x3ff6b052 ), /* 129 */
	/* 2072 */ DATA_1x2( 0x61cd7778, 0x3c727df1 ),
	/* 2080 */ DATA_1x2( 0x750bdabf, 0x3ff6c012 ), /* 130 */
	/* 2088 */ DATA_1x2( 0x8734b982, 0xbc6a12ad ),
	/* 2096 */ DATA_1x2( 0xddd47645, 0x3ff6cfdc ), /* 131 */
	/* 2104 */ DATA_1x2( 0x4a05b767, 0x3c93f992 ),
	/* 2112 */ DATA_1x2( 0x3c651a2f, 0x3ff6dfb2 ), /* 132 */
	/* 2120 */ DATA_1x2( 0xb86da9ee, 0xbc6367ef ),
	/* 2128 */ DATA_1x2( 0x98593ae5, 0x3ff6ef92 ), /* 133 */
	/* 2136 */ DATA_1x2( 0x39a8b5f0, 0xbc875579 ),
	/* 2144 */ DATA_1x2( 0xf9519484, 0x3ff6ff7d ), /* 134 */
	/* 2152 */ DATA_1x2( 0x54e08851, 0xbc80dc3d ),
	/* 2160 */ DATA_1x2( 0x66f42e87, 0x3ff70f74 ), /* 135 */
	/* 2168 */ DATA_1x2( 0x56fa9d1a, 0x3c51ed2f ),
	/* 2176 */ DATA_1x2( 0xe8ec5f74, 0x3ff71f75 ), /* 136 */
	/* 2184 */ DATA_1x2( 0x7e5a3ecf, 0xbc781f64 ),
	/* 2192 */ DATA_1x2( 0x86ead08a, 0x3ff72f82 ), /* 137 */
	/* 2200 */ DATA_1x2( 0x9006c909, 0xbc88e67a ),
	/* 2208 */ DATA_1x2( 0x48a58174, 0x3ff73f9a ), /* 138 */
	/* 2216 */ DATA_1x2( 0xc08b7db0, 0xbc86ee4a ),
	/* 2224 */ DATA_1x2( 0x35d7cbfd, 0x3ff74fbd ), /* 139 */
	/* 2232 */ DATA_1x2( 0x66977ac8, 0x3c865975 ),
	/* 2240 */ DATA_1x2( 0x564267c9, 0x3ff75feb ), /* 140 */
	/* 2248 */ DATA_1x2( 0x1e55e68a, 0xbc861932 ),
	/* 2256 */ DATA_1x2( 0xb1ab6e09, 0x3ff77024 ), /* 141 */
	/* 2264 */ DATA_1x2( 0x028a5c3a, 0x3c92c0b7 ),
	/* 2272 */ DATA_1x2( 0x4fde5d3f, 0x3ff78069 ), /* 142 */
	/* 2280 */ DATA_1x2( 0x5e09d4d2, 0x3c909ccb ),
	/* 2288 */ DATA_1x2( 0x38ac1cf6, 0x3ff790b9 ), /* 143 */
	/* 2296 */ DATA_1x2( 0xf49cc78b, 0x3c8a30fa ),
	/* 2304 */ DATA_1x2( 0x73eb0187, 0x3ff7a114 ), /* 144 */
	/* 2312 */ DATA_1x2( 0xb94da51d, 0xbc7b32dc ),
	/* 2320 */ DATA_1x2( 0x0976cfdb, 0x3ff7b17b ), /* 145 */
	/* 2328 */ DATA_1x2( 0x519d7b5c, 0xbc92dad3 ),
	/* 2336 */ DATA_1x2( 0x0130c132, 0x3ff7c1ed ), /* 146 */
	/* 2344 */ DATA_1x2( 0x5467c06b, 0x3c94ecfd ),
	/* 2352 */ DATA_1x2( 0x62ff86f0, 0x3ff7d26a ), /* 147 */
	/* 2360 */ DATA_1x2( 0x10fd15c2, 0x3c87d514 ),
	/* 2368 */ DATA_1x2( 0x36cf4e62, 0x3ff7e2f3 ), /* 148 */
	/* 2376 */ DATA_1x2( 0xabd66c55, 0x3c65ebe1 ),
	/* 2384 */ DATA_1x2( 0x8491c491, 0x3ff7f387 ), /* 149 */
	/* 2392 */ DATA_1x2( 0x29969871, 0xbc760a36 ),
	/* 2400 */ DATA_1x2( 0x543e1a12, 0x3ff80427 ), /* 150 */
	/* 2408 */ DATA_1x2( 0x2fb3cf42, 0xbc88a1c5 ),
	/* 2416 */ DATA_1x2( 0xadd106d9, 0x3ff814d2 ), /* 151 */
	/* 2424 */ DATA_1x2( 0xe3fdef5c, 0x3c8b18c6 ),
	/* 2432 */ DATA_1x2( 0x994cce13, 0x3ff82589 ), /* 152 */
	/* 2440 */ DATA_1x2( 0xf13b3734, 0xbc9369b6 ),
	/* 2448 */ DATA_1x2( 0x1eb941f7, 0x3ff8364c ), /* 153 */
	/* 2456 */ DATA_1x2( 0xdcb1390a, 0x3c90ec1d ),
	/* 2464 */ DATA_1x2( 0x4623c7ad, 0x3ff8471a ), /* 154 */
	/* 2472 */ DATA_1x2( 0x3a19ff1e, 0xbc805e84 ),
	/* 2480 */ DATA_1x2( 0x179f5b21, 0x3ff857f4 ), /* 155 */
	/* 2488 */ DATA_1x2( 0x4f3afa1e, 0xbc522cea ),
	/* 2496 */ DATA_1x2( 0x9b4492ed, 0x3ff868d9 ), /* 156 */
	/* 2504 */ DATA_1x2( 0xd872576e, 0xbc94d450 ),
	/* 2512 */ DATA_1x2( 0xd931a436, 0x3ff879ca ), /* 157 */
	/* 2520 */ DATA_1x2( 0x9b958471, 0x3c7c8854 ),
	/* 2528 */ DATA_1x2( 0xd98a6699, 0x3ff88ac7 ), /* 158 */
	/* 2536 */ DATA_1x2( 0x5b0e8a00, 0x3c90ad67 ),
	/* 2544 */ DATA_1x2( 0xa478580f, 0x3ff89bd0 ), /* 159 */
	/* 2552 */ DATA_1x2( 0x962f7877, 0x3c931143 ),
	/* 2560 */ DATA_1x2( 0x422aa0db, 0x3ff8ace5 ), /* 160 */
	/* 2568 */ DATA_1x2( 0xc1f0eab4, 0x3c8db72f ),
	/* 2576 */ DATA_1x2( 0xbad61778, 0x3ff8be05 ), /* 161 */
	/* 2584 */ DATA_1x2( 0x6f112478, 0x3c93e9e9 ),
	/* 2592 */ DATA_1x2( 0x16b5448c, 0x3ff8cf32 ), /* 162 */
	/* 2600 */ DATA_1x2( 0x9cc5e7ff, 0xbc65b660 ),
	/* 2608 */ DATA_1x2( 0x5e0866d9, 0x3ff8e06a ), /* 163 */
	/* 2616 */ DATA_1x2( 0xa4a38df0, 0xbc8dac42 ),
	/* 2624 */ DATA_1x2( 0x99157736, 0x3ff8f1ae ), /* 164 */
	/* 2632 */ DATA_1x2( 0x59f35f44, 0x3c7bf683 ),
	/* 2640 */ DATA_1x2( 0xd0282c8a, 0x3ff902fe ), /* 165 */
	/* 2648 */ DATA_1x2( 0x98b1ed84, 0x3c8b99dd ),
	/* 2656 */ DATA_1x2( 0x0b91ffc6, 0x3ff9145b ), /* 166 */
	/* 2664 */ DATA_1x2( 0xa71e3d83, 0xbc93091f ),
	/* 2672 */ DATA_1x2( 0x53aa2fe2, 0x3ff925c3 ), /* 167 */
	/* 2680 */ DATA_1x2( 0x50cbb750, 0xbc7885ad ),
	/* 2688 */ DATA_1x2( 0xb0cdc5e5, 0x3ff93737 ), /* 168 */
	/* 2696 */ DATA_1x2( 0x8b6c1e29, 0xbc5da9b8 ),
	/* 2704 */ DATA_1x2( 0x2b5f98e5, 0x3ff948b8 ), /* 169 */
	/* 2712 */ DATA_1x2( 0x5f3e0301, 0xbc82d5e8 ),
	/* 2720 */ DATA_1x2( 0xcbc8520f, 0x3ff95a44 ), /* 170 */
	/* 2728 */ DATA_1x2( 0x7c90b959, 0xbc6c23f9 ),
	/* 2736 */ DATA_1x2( 0x9a7670b3, 0x3ff96bdd ), /* 171 */
	/* 2744 */ DATA_1x2( 0x28996971, 0xbc516694 ),
	/* 2752 */ DATA_1x2( 0x9fde4e50, 0x3ff97d82 ), /* 172 */
	/* 2760 */ DATA_1x2( 0x22f4f9aa, 0xbc924343 ),
	/* 2768 */ DATA_1x2( 0xe47a22a2, 0x3ff98f33 ), /* 173 */
	/* 2776 */ DATA_1x2( 0xc1c4c014, 0x3c71f2b2 ),
	/* 2784 */ DATA_1x2( 0x70ca07ba, 0x3ff9a0f1 ), /* 174 */
	/* 2792 */ DATA_1x2( 0xd7668e4b, 0xbc85ca6c ),
	/* 2800 */ DATA_1x2( 0x4d53fe0d, 0x3ff9b2bb ), /* 175 */
	/* 2808 */ DATA_1x2( 0x04f166b6, 0xbc9294f3 ),
	/* 2816 */ DATA_1x2( 0x82a3f090, 0x3ff9c491 ), /* 176 */
	/* 2824 */ DATA_1x2( 0x2b91ce27, 0x3c71affc ),
	/* 2832 */ DATA_1x2( 0x194bb8d5, 0x3ff9d674 ), /* 177 */
	/* 2840 */ DATA_1x2( 0x414c07d3, 0xbc8a1e58 ),
	/* 2848 */ DATA_1x2( 0x19e32323, 0x3ff9e863 ), /* 178 */
	/* 2856 */ DATA_1x2( 0xe10a73bb, 0x3c6dd235 ),
	/* 2864 */ DATA_1x2( 0x8d07f29e, 0x3ff9fa5e ), /* 179 */
	/* 2872 */ DATA_1x2( 0x58a20091, 0xbc79740b ),
	/* 2880 */ DATA_1x2( 0x7b5de565, 0x3ffa0c66 ), /* 180 */
	/* 2888 */ DATA_1x2( 0x22622263, 0xbc87c504 ),
	/* 2896 */ DATA_1x2( 0xed8eb8bb, 0x3ffa1e7a ), /* 181 */
	/* 2904 */ DATA_1x2( 0x0a2b96c2, 0x3c916583 ),
	/* 2912 */ DATA_1x2( 0xec4a2d33, 0x3ffa309b ), /* 182 */
	/* 2920 */ DATA_1x2( 0xe3e231d5, 0x3c8b1c86 ),
	/* 2928 */ DATA_1x2( 0x80460ad8, 0x3ffa42c9 ), /* 183 */
	/* 2936 */ DATA_1x2( 0xbe27874b, 0xbc903d5c ),
	/* 2944 */ DATA_1x2( 0xb23e255d, 0x3ffa5503 ), /* 184 */
	/* 2952 */ DATA_1x2( 0xd3bcbb15, 0xbc91bbd1 ),
	/* 2960 */ DATA_1x2( 0x8af46052, 0x3ffa674a ), /* 185 */
	/* 2968 */ DATA_1x2( 0x8980fce0, 0x3c598617 ),
	/* 2976 */ DATA_1x2( 0x1330b358, 0x3ffa799e ), /* 186 */
	/* 2984 */ DATA_1x2( 0x9cee31d2, 0x3c90cc31 ),
	/* 2992 */ DATA_1x2( 0x53c12e59, 0x3ffa8bfe ), /* 187 */
	/* 3000 */ DATA_1x2( 0x75b1f2a6, 0xbc894729 ),
	/* 3008 */ DATA_1x2( 0x5579fdbf, 0x3ffa9e6b ), /* 188 */
	/* 3016 */ DATA_1x2( 0x6e735ab3, 0x3c846984 ),
	/* 3024 */ DATA_1x2( 0x21356eba, 0x3ffab0e5 ), /* 189 */
	/* 3032 */ DATA_1x2( 0xa34b7e7f, 0x3c7d8157 ),
	/* 3040 */ DATA_1x2( 0xbfd3f37a, 0x3ffac36b ), /* 190 */
	/* 3048 */ DATA_1x2( 0x978e9db4, 0xbc82dfcd ),
	/* 3056 */ DATA_1x2( 0x3a3c2774, 0x3ffad5ff ), /* 191 */
	/* 3064 */ DATA_1x2( 0x231ebb7d, 0x3c8c8a4e ),
	/* 3072 */ DATA_1x2( 0x995ad3ad, 0x3ffae89f ), /* 192 */
	/* 3080 */ DATA_1x2( 0x92cb3386, 0x3c8c1a77 ),
	/* 3088 */ DATA_1x2( 0xe622f2ff, 0x3ffafb4c ), /* 193 */
	/* 3096 */ DATA_1x2( 0x11a142e5, 0xbc888c8d ),
	/* 3104 */ DATA_1x2( 0x298db666, 0x3ffb0e07 ), /* 194 */
	/* 3112 */ DATA_1x2( 0x4ad1d9fa, 0xbc907b8f ),
	/* 3120 */ DATA_1x2( 0x6c9a8952, 0x3ffb20ce ), /* 195 */
	/* 3128 */ DATA_1x2( 0xa41433c7, 0x3c889c2e ),
	/* 3136 */ DATA_1x2( 0xb84f15fb, 0x3ffb33a2 ), /* 196 */
	/* 3144 */ DATA_1x2( 0x56dcaeba, 0xbc55c3d9 ),
	/* 3152 */ DATA_1x2( 0x15b749b1, 0x3ffb4684 ), /* 197 */
	/* 3160 */ DATA_1x2( 0xdac8ff80, 0xbc7274ae ),
	/* 3168 */ DATA_1x2( 0x8de5593a, 0x3ffb5972 ), /* 198 */
	/* 3176 */ DATA_1x2( 0x3da6f640, 0xbc90a40e ),
	/* 3184 */ DATA_1x2( 0x29f1c52a, 0x3ffb6c6e ), /* 199 */
	/* 3192 */ DATA_1x2( 0xce76df06, 0x3c85c620 ),
	/* 3200 */ DATA_1x2( 0xf2fb5e47, 0x3ffb7f76 ), /* 200 */
	/* 3208 */ DATA_1x2( 0x38ad9334, 0xbc68d6f4 ),
	/* 3216 */ DATA_1x2( 0xf22749e4, 0x3ffb928c ), /* 201 */
	/* 3224 */ DATA_1x2( 0xe1b51e41, 0xbc8fda52 ),
	/* 3232 */ DATA_1x2( 0x30a1064a, 0x3ffba5b0 ), /* 202 */
	/* 3240 */ DATA_1x2( 0x6b588a36, 0xbc91eee2 ),
	/* 3248 */ DATA_1x2( 0xb79a6f1f, 0x3ffbb8e0 ), /* 203 */
	/* 3256 */ DATA_1x2( 0x7b3e2cd8, 0xbc32141a ),
	/* 3264 */ DATA_1x2( 0x904bc1d2, 0x3ffbcc1e ), /* 204 */
	/* 3272 */ DATA_1x2( 0x0a5fddcd, 0x3c74ffd7 ),
	/* 3280 */ DATA_1x2( 0xc3f3a207, 0x3ffbdf69 ), /* 205 */
	/* 3288 */ DATA_1x2( 0x507554e5, 0xbc302899 ),
	/* 3296 */ DATA_1x2( 0x5bd71e09, 0x3ffbf2c2 ), /* 206 */
	/* 3304 */ DATA_1x2( 0xfa9298ad, 0xbc91bdfb ),
	/* 3312 */ DATA_1x2( 0x6141b33d, 0x3ffc0628 ), /* 207 */
	/* 3320 */ DATA_1x2( 0xd4c0010c, 0xbc80dda2 ),
	/* 3328 */ DATA_1x2( 0xdd85529c, 0x3ffc199b ), /* 208 */
	/* 3336 */ DATA_1x2( 0x30af0cb3, 0x3c736eae ),
	/* 3344 */ DATA_1x2( 0xd9fa652c, 0x3ffc2d1c ), /* 209 */
	/* 3352 */ DATA_1x2( 0xaadf8d68, 0xbc8a007d ),
	/* 3360 */ DATA_1x2( 0x5fffd07a, 0x3ffc40ab ), /* 210 */
	/* 3368 */ DATA_1x2( 0x5c9ffd93, 0x3c8ee332 ),
	/* 3376 */ DATA_1x2( 0x78fafb22, 0x3ffc5447 ), /* 211 */
	/* 3384 */ DATA_1x2( 0x391181d3, 0x3c836909 ),
	/* 3392 */ DATA_1x2( 0x2e57d14b, 0x3ffc67f1 ), /* 212 */
	/* 3400 */ DATA_1x2( 0xd10959ac, 0x3c84e08f ),
	/* 3408 */ DATA_1x2( 0x8988c933, 0x3ffc7ba8 ), /* 213 */
	/* 3416 */ DATA_1x2( 0xdbdf9547, 0xbc811cd7 ),
	/* 3424 */ DATA_1x2( 0x9406e7b5, 0x3ffc8f6d ), /* 214 */
	/* 3432 */ DATA_1x2( 0x384e1a67, 0x3c63cdaf ),
	/* 3440 */ DATA_1x2( 0x5751c4db, 0x3ffca340 ), /* 215 */
	/* 3448 */ DATA_1x2( 0x7bef6622, 0xbc7ac28b ),
	/* 3456 */ DATA_1x2( 0xdcef9069, 0x3ffcb720 ), /* 216 */
	/* 3464 */ DATA_1x2( 0x6c921968, 0x3c676b2c ),
	/* 3472 */ DATA_1x2( 0x2e6d1675, 0x3ffccb0f ), /* 217 */
	/* 3480 */ DATA_1x2( 0x7207b9e1, 0xbc703058 ),
	/* 3488 */ DATA_1x2( 0x555dc3fa, 0x3ffcdf0b ), /* 218 */
	/* 3496 */ DATA_1x2( 0x83ccb5d2, 0xbc808a18 ),
	/* 3504 */ DATA_1x2( 0x5b5bab74, 0x3ffcf315 ), /* 219 */
	/* 3512 */ DATA_1x2( 0x592af7fc, 0xbc8cc734 ),
	/* 3520 */ DATA_1x2( 0x4a07897c, 0x3ffd072d ), /* 220 */
	/* 3528 */ DATA_1x2( 0x3ffffa6f, 0xbc8fad5d ),
	/* 3536 */ DATA_1x2( 0x2b08c968, 0x3ffd1b53 ), /* 221 */
	/* 3544 */ DATA_1x2( 0x44f587e8, 0x3c87752a ),
	/* 3552 */ DATA_1x2( 0x080d89f2, 0x3ffd2f87 ), /* 222 */
	/* 3560 */ DATA_1x2( 0x3875a949, 0xbc900dae ),
	/* 3568 */ DATA_1x2( 0xeacaa1d6, 0x3ffd43c8 ), /* 223 */
	/* 3576 */ DATA_1x2( 0xefeef52d, 0x3c85b66f ),
	/* 3584 */ DATA_1x2( 0xdcfba487, 0x3ffd5818 ), /* 224 */
	/* 3592 */ DATA_1x2( 0xa63d07a7, 0x3c74a385 ),
	/* 3600 */ DATA_1x2( 0xe862e6d3, 0x3ffd6c76 ), /* 225 */
	/* 3608 */ DATA_1x2( 0xd908a96e, 0x3c5159d9 ),
	/* 3616 */ DATA_1x2( 0x16c98398, 0x3ffd80e3 ), /* 226 */
	/* 3624 */ DATA_1x2( 0x2040220f, 0xbc82919e ),
	/* 3632 */ DATA_1x2( 0x71ff6075, 0x3ffd955d ), /* 227 */
	/* 3640 */ DATA_1x2( 0x16117a68, 0x3c8c254d ),
	/* 3648 */ DATA_1x2( 0x03db3285, 0x3ffda9e6 ), /* 228 */
	/* 3656 */ DATA_1x2( 0xd5c192ac, 0x3c8e5a50 ),
	/* 3664 */ DATA_1x2( 0xd63a8315, 0x3ffdbe7c ), /* 229 */
	/* 3672 */ DATA_1x2( 0x9fbd0e04, 0xbc8d8c32 ),
	/* 3680 */ DATA_1x2( 0xf301b460, 0x3ffdd321 ), /* 230 */
	/* 3688 */ DATA_1x2( 0xac016b4b, 0x3c843a59 ),
	/* 3696 */ DATA_1x2( 0x641c0658, 0x3ffde7d5 ), /* 231 */
	/* 3704 */ DATA_1x2( 0xfbd5f2a6, 0xbc8ea6e6 ),
	/* 3712 */ DATA_1x2( 0x337b9b5f, 0x3ffdfc97 ), /* 232 */
	/* 3720 */ DATA_1x2( 0x07b43e1f, 0xbc82d521 ),
	/* 3728 */ DATA_1x2( 0x6b197d17, 0x3ffe1167 ), /* 233 */
	/* 3736 */ DATA_1x2( 0xeab2cbb4, 0xbc63e8e3 ),
	/* 3744 */ DATA_1x2( 0x14f5a129, 0x3ffe2646 ), /* 234 */
	/* 3752 */ DATA_1x2( 0x3b470dc9, 0xbc892ab9 ),
	/* 3760 */ DATA_1x2( 0x3b16ee12, 0x3ffe3b33 ), /* 235 */
	/* 3768 */ DATA_1x2( 0xcd0d2cda, 0xbc8b7966 ),
	/* 3776 */ DATA_1x2( 0xe78b3ff6, 0x3ffe502e ), /* 236 */
	/* 3784 */ DATA_1x2( 0x603a88d3, 0x3c74b604 ),
	/* 3792 */ DATA_1x2( 0x24676d76, 0x3ffe6539 ), /* 237 */
	/* 3800 */ DATA_1x2( 0x4c2ff1cf, 0xbc776caa ),
	/* 3808 */ DATA_1x2( 0xfbc74c83, 0x3ffe7a51 ), /* 238 */
	/* 3816 */ DATA_1x2( 0x519d7271, 0x3c83c5ec ),
	/* 3824 */ DATA_1x2( 0x77cdb740, 0x3ffe8f79 ), /* 239 */
	/* 3832 */ DATA_1x2( 0x525d9940, 0xbc81d5fc ),
	/* 3840 */ DATA_1x2( 0xa2a490da, 0x3ffea4af ), /* 240 */
	/* 3848 */ DATA_1x2( 0x8fd391f1, 0xbc8ff712 ),
	/* 3856 */ DATA_1x2( 0x867cca6e, 0x3ffeb9f4 ), /* 241 */
	/* 3864 */ DATA_1x2( 0xaaea3d21, 0x3c855cd8 ),
	/* 3872 */ DATA_1x2( 0x2d8e67f1, 0x3ffecf48 ), /* 242 */
	/* 3880 */ DATA_1x2( 0xe223747d, 0xbc8dae98 ),
	/* 3888 */ DATA_1x2( 0xa2188510, 0x3ffee4aa ), /* 243 */
	/* 3896 */ DATA_1x2( 0x7c2bed49, 0x3c826994 ),
	/* 3904 */ DATA_1x2( 0xee615a27, 0x3ffefa1b ), /* 244 */
	/* 3912 */ DATA_1x2( 0x41aa2008, 0x3c8ec3bc ),
	/* 3920 */ DATA_1x2( 0x1cb6412a, 0x3fff0f9c ), /* 245 */
	/* 3928 */ DATA_1x2( 0x7e9afe9e, 0xbc83b613 ),
	/* 3936 */ DATA_1x2( 0x376bba97, 0x3fff252b ), /* 246 */
	/* 3944 */ DATA_1x2( 0xc3a9eb32, 0x3c842b94 ),
	/* 3952 */ DATA_1x2( 0x48dd7274, 0x3fff3ac9 ), /* 247 */
	/* 3960 */ DATA_1x2( 0x878ba7c7, 0xbc69fa74 ),
	/* 3968 */ DATA_1x2( 0x5b6e4540, 0x3fff5076 ), /* 248 */
	/* 3976 */ DATA_1x2( 0x31d185ed, 0x3c8a64a9 ),
	/* 3984 */ DATA_1x2( 0x798844f8, 0x3fff6632 ), /* 249 */
	/* 3992 */ DATA_1x2( 0x75ee0efd, 0x3c901f3a ),
	/* 4000 */ DATA_1x2( 0xad9cbe14, 0x3fff7bfd ), /* 250 */
	/* 4008 */ DATA_1x2( 0xe43be3ed, 0xbc8e37ba ),
	/* 4016 */ DATA_1x2( 0x02243c89, 0x3fff91d8 ), /* 251 */
	/* 4024 */ DATA_1x2( 0xe6ed84fa, 0xbc516a9c ),
	/* 4032 */ DATA_1x2( 0x819e90d8, 0x3fffa7c1 ), /* 252 */
	/* 4040 */ DATA_1x2( 0x4d91cd9c, 0x3c77893b ),
	/* 4048 */ DATA_1x2( 0x3692d514, 0x3fffbdba ), /* 253 */
	/* 4056 */ DATA_1x2( 0xb2effc76, 0xbc699c7d ),
	/* 4064 */ DATA_1x2( 0x2b8f71f1, 0x3fffd3c2 ), /* 254 */
	/* 4072 */ DATA_1x2( 0x4160cc89, 0x3c5305c1 ),
	/* 4080 */ DATA_1x2( 0x6b2a23d9, 0x3fffe9d9 ), /* 255 */
	/* 4088 */ DATA_1x2( 0x677f983f, 0x3c64b458 ),

	/* F_PRECISION acc pow2 result range check */
	/* 4096 */ DATA_1x2( 0xfa5abcbf, 0x03e00b1a ),
	/* 4104 */ DATA_1x2( 0x70cf671a, 0x7c0fdebe ),
	/* 4112 */ DATA_1x2( 0x6b2a23d9, 0x7fefe9d9 ),

	/* R_PRECISION acc pow2 result range check */
	/* 4120 */ DATA_1x2( 0xfa5abcbf, 0x3a100b1a ),
	/* 4128 */ DATA_1x2( 0x70cf671a, 0x0ddfdebe ),
	/* 4136 */ DATA_1x2( 0x6b2a23d9, 0x47efe9d9 ),

	/* 'big' for fast pow/exp rint computation */
	/* 4144 */ DATA_1x2( 0x00000000, 0x42080000 ),

	/* 2^-F_EXP_WIDTH/log(2) in full, hi, lo */
	/* 4152 */ DATA_1x2( 0x652b82fe, 0x3f471547 ),
	/* 4160 */ DATA_1x2( 0x00000000, 0x3f471548 ),
	/* 4168 */ DATA_1x2( 0xa03d1106, 0xbdf35a8f ),

	/* Fast exp F_PRECISION arg range check */
	/* 4176 */ DATA_1x2( 0xfefa39f0, 0x40862e42 ),

	/* Fast exp R_PRECISION arg range check */
	/* 4184 */ DATA_1x2( 0xfefa39f0, 0x40562e42 ),

	/* F_PRECISION fast pow2 poly coeffs */
	/* 4192 */ DATA_1x2( 0x00000000, 0x3ff00000 ),
	/* 4200 */ DATA_1x2( 0xfefa39e3, 0x40962e42 ),
	/* 4208 */ DATA_1x2( 0xff82c5b7, 0x412ebfbd ),
	/* 4216 */ DATA_1x2( 0xf141f5f9, 0x41bc6b08 ),
	/* 4224 */ DATA_1x2( 0x639a3d4f, 0x4243b2ab ),

	/* R_PRECISION fast pow2 poly coeffs */
	/* 4232 */ DATA_1x2( 0x0000006d, 0x3ff00000 ),
	/* 4240 */ DATA_1x2( 0x27f0298e, 0x3fe62e43 ),
	/* 4248 */ DATA_1x2( 0xe31e7970, 0x3fcebfbd ),

	/* Power of 2 to scale down y: 2^-F_EXP_WIDTH */
	/* 4256 */ DATA_1x2( 0x00000000, 0x3f400000 ),

	/* ln2 in hi/lo */
	/* 4264 */ DATA_1x2( 0x00000000, 0x3fe62e43 ),
	/* 4272 */ DATA_1x2( 0x0ca86c39, 0xbe205c61 ),

	/* ln2/ln10 in hi/lo */
	/* 4280 */ DATA_1x2( 0x60000000, 0x3fd34413 ),
	/* 4288 */ DATA_1x2( 0x0219dc1e, 0xbe4ec10c ),

	/* F_PRECISION acc pow2 poly coeffs */
	/* 4296 */ DATA_1x2( 0xfefa39ef, 0x3fe62e42 ),
	/* 4304 */ DATA_1x2( 0xff82c589, 0x3fcebfbd ),
	/* 4312 */ DATA_1x2( 0xd704a0c6, 0x3fac6b08 ),
	/* 4320 */ DATA_1x2( 0x7bda5fa2, 0x3f83b2ab ),
	/* 4328 */ DATA_1x2( 0xe78a6731, 0x3f55d87f ),

	/* R_PRECISION acc pow2 poly coeffs */
	/* 4336 */ DATA_1x2( 0x0000006d, 0x3ff00000 ),
	/* 4344 */ DATA_1x2( 0x27f0298e, 0x3fe62e43 ),
	/* 4352 */ DATA_1x2( 0xe31e7970, 0x3fcebfbd ),

	/* 'big' for accurate pow/exp rint computation */
	/* 4360 */ DATA_1x2( 0x00000000, 0x42b80000 ),

	/* F_PRECISION argument and result sreening values */
	/* 4368 */ DATA_1x2( 0x00000000, 0x3c900000 ),
	/* 4376 */ DATA_1x2( 0xd52d3052, 0x03f74910 ),

	/* R_PRECISION argument and result sreening values */
	/* 4384 */ DATA_1x2( 0x33000000, 0x00000000 ),
	/* 4392 */ DATA_1x2( 0x0fcff1b5, 0x00000000 ),

	/* F_PRECISION argument screening values for 2^x */
	/* 4400 */ DATA_1x2( 0x00000000, 0x0400cc00 ),

	/* R_PRECISION argument and result sreening values */
	/* 4408 */ DATA_1x2( 0x10160000, 0x00000000 ),

	/* F_PRECISION argument and result sreening values for 10^x */
	/* 4416 */ DATA_1x2( 0x00000000, 0x3c900000 ),
	/* 4424 */ DATA_1x2( 0x46e36b53, 0x03e439b7 ),

	/* R_PRECISION argument and result sreening values for 10^x */
	/* 4432 */ DATA_1x2( 0x33000000, 0x00000000 ),
	/* 4440 */ DATA_1x2( 0x0f349e36, 0x00000000 ),

	/* F_PRECISION acc exp poly coeffs */
	/* 4448 */ DATA_1x2( 0x00000000, 0x3ff00000 ),
	/* 4456 */ DATA_1x2( 0xffffff8e, 0x3fdfffff ),
	/* 4464 */ DATA_1x2( 0x555555b7, 0x3fc55555 ),
	/* 4472 */ DATA_1x2( 0x8e38e382, 0x3fa55555 ),
	/* 4480 */ DATA_1x2( 0x1111110a, 0x3f811111 ),

	/* F_PRECISION acc exp poly coeffs */
	/* 4488 */ DATA_1x2( 0xbbb55516, 0x40026bb1 ),
	/* 4496 */ DATA_1x2( 0xc73cea69, 0x40053524 ),
	/* 4504 */ DATA_1x2( 0x91de27bd, 0x40004705 ),
	/* 4512 */ DATA_1x2( 0x09fd9f5a, 0x3ff2bd76 ),
	/* 4520 */ DATA_1x2( 0xce48f476, 0x3fe142a0 ),
	/* 4528 */ DATA_1x2( 0x0847c7c4, 0x3fca7ed7 ),

	/* F_PRECISION expm1 initial screening constants */
	/* 4536 */ DATA_1x2( 0x00000000, 0x3f800000 ),
	/* 4544 */ DATA_1x2( 0xfefa39f0, 0x40862e42 ),
	/* 4552 */ DATA_1x2( 0x872320e2, 0x4042b708 ),

	/* R_PRECISION expm1 initial screening constants */
	/* 4560 */ DATA_1x2( 0x3c000000, 0x00000000 ),
	/* 4568 */ DATA_1x2( 0x42b17218, 0x00000000 ),
	/* 4576 */ DATA_1x2( 0x418aa122, 0x00000000 ),

	/* F_PRECISION expm1 poly range poly coeffs */
	/* 4584 */ DATA_1x2( 0x00000000, 0x3fe00000 ),
	/* 4592 */ DATA_1x2( 0x55555555, 0x3fc55555 ),
	/* 4600 */ DATA_1x2( 0x55553814, 0x3fa55555 ),
	/* 4608 */ DATA_1x2( 0x111120f6, 0x3f811111 ),
	/* 4616 */ DATA_1x2( 0x86e87992, 0x3f56c16e ),
	/* 4624 */ DATA_1x2( 0xd2f0b5c7, 0x3f2a01a0 ),

	/* F_PRECISION expm1 reduce range poly coeffs */
	/* 4632 */ DATA_1x2( 0x00000000, 0x3fe00000 ),
	/* 4640 */ DATA_1x2( 0x55555555, 0x3fc55555 ),
	/* 4648 */ DATA_1x2( 0x55553814, 0x3fa55555 ),
	/* 4656 */ DATA_1x2( 0x111120f6, 0x3f811111 ),
	/* 4664 */ DATA_1x2( 0x86e87992, 0x3f56c16e ),
	/* 4672 */ DATA_1x2( 0xd2f0b5c7, 0x3f2a01a0 ),

	/* R_PRECISION expm1 poly range poly coeffs */
	/* 4680 */ DATA_1x2( 0xffff7778, 0x3fefffff ),
	/* 4688 */ DATA_1x2( 0x000071c7, 0x3fe00000 ),
	/* 4696 */ DATA_1x2( 0x99998843, 0x3fc55559 ),
	/* 4704 */ DATA_1x2( 0x555549c6, 0x3fa55555 ),

	/* R_PRECISION expm1 reduce range poly coeffs */
	/* 4712 */ DATA_1x2( 0xfefa2417, 0x3fe62e42 ),
	/* 4720 */ DATA_1x2( 0xff82f808, 0x3fcebfbd ),
	/* 4728 */ DATA_1x2( 0x9214985c, 0x3fac6b0b ),
	/* 4736 */ DATA_1x2( 0x6fba4c01, 0x3f83b2ab ),

	/* F_PRECISION sinh/cosh argument screening constants */
	/* 4744 */ DATA_1x2( 0x8fb9f87e, 0x408633ce ),
	/* 4752 */ DATA_1x2( 0x293abcb2, 0x00bf9330 ),
	/* 4760 */ DATA_1x2( 0x667f3bcc, 0x3fc6a09e ),

	/* R_PRECISION sinh/cosh argument screening constants */
	/* 4768 */ DATA_1x2( 0x42b2d4fd, 0x00000000 ),
	/* 4776 */ DATA_1x2( 0x047dd00a, 0x00000000 ),
	/* 4784 */ DATA_1x2( 0x3e3504f3, 0x00000000 ),

	/* F_PRECISION sinh poly range poly coeffs */
	/* 4792 */ DATA_1x2( 0x55555555, 0x3fc55555 ),
	/* 4800 */ DATA_1x2( 0x11111108, 0x3f811111 ),
	/* 4808 */ DATA_1x2( 0x1a03c7ed, 0x3f2a01a0 ),
	/* 4816 */ DATA_1x2( 0x750cdc28, 0x3ec71de3 ),
	/* 4824 */ DATA_1x2( 0x69964294, 0x3e5ae9b8 ),

	/* F_PRECISION cosh poly range poly coeffs */
	/* 4832 */ DATA_1x2( 0x00000000, 0x3fe00000 ),
	/* 4840 */ DATA_1x2( 0x55555539, 0x3fa55555 ),
	/* 4848 */ DATA_1x2( 0x16c4ecab, 0x3f56c16c ),
	/* 4856 */ DATA_1x2( 0xcb8adc6c, 0x3efa019f ),
	/* 4864 */ DATA_1x2( 0x264635c5, 0x3e92811d ),

	/* R_PRECISION sinh poly range poly coeffs */
	/* 4872 */ DATA_1x2( 0x000cfb0b, 0x3ff00000 ),
	/* 4880 */ DATA_1x2( 0x6b60aac4, 0x3fc55554 ),
	/* 4888 */ DATA_1x2( 0x7c0f16b2, 0x3f8115f1 ),

	/* R_PRECISION cosh poly range poly coeffs */
	/* 4896 */ DATA_1x2( 0xfffff98b, 0x3fefffff ),
	/* 4904 */ DATA_1x2( 0x0019e9bb, 0x3fe00000 ),
	/* 4912 */ DATA_1x2( 0x51b349b1, 0x3fa55554 ),
	/* 4920 */ DATA_1x2( 0xb5354813, 0x3f56c7eb ),

	/* B_PRECISION .5, 1.0 and 2.0 */
	/* 4928 */ DATA_1x2( 0x00000000, 0x3fe00000 ),
	/* 4936 */ DATA_1x2( 0x00000000, 0x3ff00000 ),
	/* 4944 */ DATA_1x2( 0x00000000, 0x40000000 ),

	/* B_PRECISION max float */
	/* 4952 */ DATA_1x2( 0xffffffff, 0x7fefffff ),

	/* 1/ln2 in B_PRECISION */
	/* 4960 */ DATA_1x2( 0x652b82fe, 0x3ff71547 ),

	/* ln10/ln2 in B_PRECISION */
	/* 4968 */ DATA_1x2( 0x0979a371, 0x400a934f ),

	/* ln2/2 in F_PRECISION */
	/* 4976 */ DATA_1x2( 0xfefa39ef, 0x3fd62e42 ),

	/* F_PRECISION acc log2 poly coeffs */
	/* 4984 */ DATA_1x2( 0xffac83b5, 0x3fc47fd3 ),
	/* 4992 */ DATA_1x2( 0xa1437886, 0xbfb55046 ),
	/* 5000 */ DATA_1x2( 0x1fa518cf, 0x3fa7a334 ),
	/* 5008 */ DATA_1x2( 0xe1cdc887, 0xbf9b4e9f ),
	/* 5016 */ DATA_1x2( 0xdfbf11c7, 0x3f903962 ),
	/* 5024 */ DATA_1x2( 0x93bf219c, 0xbf83adb0 ),

	/* R_PRECISION acc log2 poly coeffs */
	/* 5032 */ DATA_1x2( 0xffff3663, 0x3fefffff ),
	/* 5040 */ DATA_1x2( 0xff09b1bc, 0xbfd62e42 ),
	/* 5048 */ DATA_1x2( 0x33b3b950, 0x3fc47fe0 ),
	/* 5056 */ DATA_1x2( 0xe00972df, 0xbfb55013 ),

	/* F_PRECISION fast log2 poly coeffs */
	/* 5064 */ DATA_1x2( 0xfefa39ef, 0xbfd62e42 ),
	/* 5072 */ DATA_1x2( 0xffac83ac, 0x3fc47fd3 ),
	/* 5080 */ DATA_1x2( 0xa13d920a, 0xbfb55046 ),
	/* 5088 */ DATA_1x2( 0x1ffa5722, 0x3fa7a334 ),
	/* 5096 */ DATA_1x2( 0x5bcb09e0, 0xbf9b4ebe ),
	/* 5104 */ DATA_1x2( 0xeddab426, 0x3f90390f ),

	/* 
	 * Fj, Rj = 1/(Fj*ln2) and Lj = log2(Fj).  Lj and Rj are
	 * given in hi and low parts.  Fj and the hi part or Lj are
	 * in reduced precision; Rj, lo(Rj) and lo(Lj) in standard
	 * precision with hi(Rj) = Rj - lo(Rj)
	 *
	 * offset                             row
	 */
	/* 5112 */ 0x3f800000, 0x00000000, /* 000 */
	/* 5120 */ DATA_1x2( 0x652b82fe, 0x3ff71547 ),
	/* 5128 */ DATA_1x2( 0x2b82fe17, 0x3f754765 ),
	/* 5136 */ DATA_1x2( 0x00000000, 0x00000000 ),
	/* 5144 */ 0x3f810000, 0x3c37f286, /* 001 */
	/* 5152 */ DATA_1x2( 0x7442fd04, 0x3ff6e778 ),
	/* 5160 */ DATA_1x2( 0xbd02fbf1, 0xbf78878b ),
	/* 5168 */ DATA_1x2( 0x5d00e391, 0xbdf221ef ),
	/* 5176 */ 0x3f820000, 0x3cb73cb4, /* 002 */
	/* 5184 */ DATA_1x2( 0xed75ac4d, 0x3ff6ba5d ),
	/* 5192 */ DATA_1x2( 0x294ecc70, 0xbf56884a ),
	/* 5200 */ DATA_1x2( 0xa629b8a0, 0x3df70b48 ),
	/* 5208 */ 0x3f830000, 0x3d08e68f, /* 003 */
	/* 5216 */ DATA_1x2( 0xaf111c54, 0x3ff68df3 ),
	/* 5224 */ DATA_1x2( 0x2238a83d, 0x3f6be75e ),
	/* 5232 */ DATA_1x2( 0xda24fd76, 0xbe15d997 ),
	/* 5240 */ 0x3f840000, 0x3d35d69c, /* 004 */
	/* 5248 */ DATA_1x2( 0xb77002e7, 0x3ff66235 ),
	/* 5256 */ DATA_1x2( 0x8ffd1920, 0xbf7dca48 ),
	/* 5264 */ DATA_1x2( 0xf19d93f2, 0xbe14e204 ),
	/* 5272 */ 0x3f850000, 0x3d626fd6, /* 005 */
	/* 5280 */ DATA_1x2( 0x23c5c923, 0x3ff63720 ),
	/* 5288 */ DATA_1x2( 0x746dba88, 0xbf61bfb8 ),
	/* 5296 */ DATA_1x2( 0x842c1c1e, 0xbe0bd552 ),
	/* 5304 */ 0x3f860000, 0x3d8759c5, /* 006 */
	/* 5312 */ DATA_1x2( 0x2ef7e44b, 0x3ff60caf ),
	/* 5320 */ DATA_1x2( 0xefc89531, 0x3f695e5d ),
	/* 5328 */ DATA_1x2( 0x530c22d1, 0xbdd75819 ),
	/* 5336 */ 0x3f870000, 0x3d9d517f, /* 007 */
	/* 5344 */ DATA_1x2( 0x3084471b, 0x3ff5e2df ),
	/* 5352 */ DATA_1x2( 0x7bb8e55d, 0xbf7d20cf ),
	/* 5360 */ DATA_1x2( 0xe93fd977, 0xbe06c071 ),
	/* 5368 */ 0x3f880000, 0x3db31fb8, /* 008 */
	/* 5376 */ DATA_1x2( 0x9b743f0d, 0x3ff5b9ac ),
	/* 5384 */ DATA_1x2( 0x2f03caf3, 0xbf594d92 ),
	/* 5392 */ DATA_1x2( 0xa60cceb2, 0xbe14dbb3 ),
	/* 5400 */ 0x3f890000, 0x3dc8c50b, /* 009 */
	/* 5408 */ DATA_1x2( 0xfd5b1b17, 0x3ff59113 ),
	/* 5416 */ DATA_1x2( 0x5b1b1682, 0x3f7113fd ),
	/* 5424 */ DATA_1x2( 0xb55ce465, 0x3e2c8c66 ),
	/* 5432 */ 0x3f8a0000, 0x3dde4212, /* 010 */
	/* 5440 */ DATA_1x2( 0xfd6002c7, 0x3ff56911 ),
	/* 5448 */ DATA_1x2( 0x9ffd396c, 0xbf76ee02 ),
	/* 5456 */ DATA_1x2( 0x4c7658b5, 0x3de5b577 ),
	/* 5464 */ 0x3f8b0000, 0x3df39761, /* 011 */
	/* 5472 */ DATA_1x2( 0x5b526d93, 0x3ff541a3 ),
	/* 5480 */ DATA_1x2( 0x26d936c3, 0x3f3a35b5 ),
	/* 5488 */ DATA_1x2( 0x9bc68494, 0xbe2d00b4 ),
	/* 5496 */ 0x3f8c0000, 0x3e0462c4, /* 012 */
	/* 5504 */ DATA_1x2( 0xeec8b247, 0x3ff51ac4 ),
	/* 5512 */ DATA_1x2( 0xc8b24766, 0x3f7ac4ee ),
	/* 5520 */ DATA_1x2( 0xc72c4f79, 0x3e39b4f3 ),
	/* 5528 */ 0x3f8d0000, 0x3e0ee68d, /* 013 */
	/* 5536 */ DATA_1x2( 0xa6482e4b, 0x3ff4f473 ),
	/* 5544 */ DATA_1x2( 0x6fa36af4, 0xbf6718b3 ),
	/* 5552 */ DATA_1x2( 0x0942b758, 0xbe3155a9 ),
	/* 5560 */ 0x3f8e0000, 0x3e19574f, /* 014 */
	/* 5568 */ DATA_1x2( 0x86768bb6, 0x3ff4ceac ),
	/* 5576 */ DATA_1x2( 0xed176c56, 0x3f6d590c ),
	/* 5584 */ DATA_1x2( 0xd0fa8f96, 0x3e13c570 ),
	/* 5592 */ 0x3f8f0000, 0x3e23b550, /* 015 */
	/* 5600 */ DATA_1x2( 0xa953b3e9, 0x3ff4a96c ),
	/* 5608 */ DATA_1x2( 0xac4c1731, 0xbf769356 ),
	/* 5616 */ DATA_1x2( 0xcb3b1c5b, 0xbe29f022 ),
	/* 5624 */ 0x3f900000, 0x3e2e00d2, /* 016 */
	/* 5632 */ DATA_1x2( 0x3d7c02a9, 0x3ff484b1 ),
	/* 5640 */ DATA_1x2( 0xf00aa3e2, 0x3f52c4f5 ),
	/* 5648 */ DATA_1x2( 0xe1817fd4, 0xbe2810a5 ),
	/* 5656 */ 0x3f910000, 0x3e383a17, /* 017 */
	/* 5664 */ DATA_1x2( 0x857253db, 0x3ff46077 ),
	/* 5672 */ DATA_1x2( 0x8dac24ff, 0xbf7f887a ),
	/* 5680 */ DATA_1x2( 0x01ac7fc6, 0xbe3621fb ),
	/* 5688 */ 0x3f920000, 0x3e42615f, /* 018 */
	/* 5696 */ DATA_1x2( 0xd6f18b64, 0x3ff43cbc ),
	/* 5704 */ DATA_1x2( 0x73a4dfcc, 0xbf4a1948 ),
	/* 5712 */ DATA_1x2( 0x9a045bb7, 0xbe3fa1f8 ),
	/* 5720 */ 0x3f930000, 0x3e4c76e8, /* 019 */
	/* 5728 */ DATA_1x2( 0x9a453c13, 0x3ff4197e ),
	/* 5736 */ DATA_1x2( 0x453c133c, 0x3f797e9a ),
	/* 5744 */ DATA_1x2( 0x7c0beb3a, 0x3e2df9c3 ),
	/* 5752 */ 0x3f940000, 0x3e567af1, /* 020 */
	/* 5760 */ DATA_1x2( 0x49a91758, 0x3ff3f6ba ),
	/* 5768 */ DATA_1x2( 0xadd14f69, 0xbf628b6c ),
	/* 5776 */ DATA_1x2( 0x27dfc23e, 0x3e3b69d9 ),
	/* 5784 */ 0x3f950000, 0x3e606db6, /* 021 */
	/* 5792 */ DATA_1x2( 0x70aed42e, 0x3ff3d46d ),
	/* 5800 */ DATA_1x2( 0xaed42e78, 0x3f746d70 ),
	/* 5808 */ DATA_1x2( 0xdc81c4db, 0x3e3bb29b ),
	/* 5816 */ 0x3f960000, 0x3e6a4f72, /* 022 */
	/* 5824 */ DATA_1x2( 0xabaa3ffe, 0x3ff3b295 ),
	/* 5832 */ DATA_1x2( 0xab800342, 0xbf6ad4a8 ),
	/* 5840 */ DATA_1x2( 0xdf91e96b, 0x3e3864b2 ),
	/* 5848 */ 0x3f970000, 0x3e74205f, /* 023 */
	/* 5856 */ DATA_1x2( 0xa7233050, 0x3ff39130 ),
	/* 5864 */ DATA_1x2( 0x23304fc3, 0x3f7130a7 ),
	/* 5872 */ DATA_1x2( 0x331f27cf, 0x3e2d39a9 ),
	/* 5880 */ 0x3f980000, 0x3e7de0b6, /* 024 */
	/* 5888 */ DATA_1x2( 0x1f4d0ffe, 0x3ff3703c ),
	/* 5896 */ DATA_1x2( 0x65e00337, 0xbf6f87c1 ),
	/* 5904 */ DATA_1x2( 0xd7715c9a, 0xbe2bf65f ),
	/* 5912 */ 0x3f990000, 0x3e83c857, /* 025 */
	/* 5920 */ DATA_1x2( 0xdf83c645, 0x3ff34fb5 ),
	/* 5928 */ DATA_1x2( 0x078c895b, 0x3f6f6bbf ),
	/* 5936 */ DATA_1x2( 0xda43f396, 0xbe313f3f ),
	/* 5944 */ 0x3f9a0000, 0x3e88983f, /* 026 */
	/* 5952 */ DATA_1x2( 0xc1cdb958, 0x3ff32f9b ),
	/* 5960 */ DATA_1x2( 0x3246a7d2, 0xbf70643e ),
	/* 5968 */ DATA_1x2( 0x28d3da3f, 0xbe34b3d2 ),
	/* 5976 */ 0x3f9b0000, 0x3e8d602e, /* 027 */
	/* 5984 */ DATA_1x2( 0xae62b18b, 0x3ff30feb ),
	/* 5992 */ DATA_1x2( 0xc563159f, 0x3f6fd75c ),
	/* 6000 */ DATA_1x2( 0x1f5ae719, 0xbe4adc1f ),
	/* 6008 */ 0x3f9c0000, 0x3e92203d, /* 028 */
	/* 6016 */ DATA_1x2( 0x9b3764eb, 0x3ff2f0a3 ),
	/* 6024 */ DATA_1x2( 0x91362a84, 0xbf6eb8c9 ),
	/* 6032 */ DATA_1x2( 0x73048b73, 0x3e461c0e ),
	/* 6040 */ 0x3f9d0000, 0x3e96d888, /* 029 */
	/* 6048 */ DATA_1x2( 0x8b8d7636, 0x3ff2d1c1 ),
	/* 6056 */ DATA_1x2( 0x8d7635e2, 0x3f71c18b ),
	/* 6064 */ DATA_1x2( 0x8487642e, 0xbe2d932a ),
	/* 6072 */ 0x3f9e0000, 0x3e9b8926, /* 030 */
	/* 6080 */ DATA_1x2( 0x8f87b4a7, 0x3ff2b343 ),
	/* 6088 */ DATA_1x2( 0xf096b214, 0xbf6978e0 ),
	/* 6096 */ DATA_1x2( 0xd9b32267, 0x3e4d499b ),
	/* 6104 */ 0x3f9f0000, 0x3ea03232, /* 031 */
	/* 6112 */ DATA_1x2( 0xc3c26cac, 0x3ff29527 ),
	/* 6120 */ DATA_1x2( 0xc26cac5a, 0x3f7527c3 ),
	/* 6128 */ DATA_1x2( 0xda293433, 0xbe3393ee ),
	/* 6136 */ 0x3fa00000, 0x3ea4d3c2, /* 032 */
	/* 6144 */ DATA_1x2( 0x50ef9bfe, 0x3ff2776c ),
	/* 6152 */ DATA_1x2( 0x20c8030e, 0xbf61275e ),
	/* 6160 */ DATA_1x2( 0x15fc9258, 0x3e479a37 ),
	/* 6168 */ 0x3fa10000, 0x3ea96df0, /* 033 */
	/* 6176 */ DATA_1x2( 0x6b76ddcf, 0x3ff25a0f ),
	/* 6184 */ DATA_1x2( 0x76ddcec8, 0x3f7a0f6b ),
	/* 6192 */ DATA_1x2( 0x641184f9, 0xbe2d9b4a ),
	/* 6200 */ 0x3fa20000, 0x3eae00d2, /* 034 */
	/* 6208 */ DATA_1x2( 0x5318e5ec, 0x3ff23d0f ),
	/* 6216 */ DATA_1x2( 0x38d0a3c4, 0xbf478567 ),
	/* 6224 */ DATA_1x2( 0xe1817fd4, 0xbe3810a5 ),
	/* 6232 */ 0x3fa30000, 0x3eb28c7f, /* 035 */
	/* 6240 */ DATA_1x2( 0x529663b9, 0x3ff2206a ),
	/* 6248 */ DATA_1x2( 0x699c469a, 0xbf7f95ad ),
	/* 6256 */ DATA_1x2( 0x9b9489e4, 0x3e319dee ),
	/* 6264 */ 0x3fa40000, 0x3eb7110e, /* 036 */
	/* 6272 */ DATA_1x2( 0xbf5a27cd, 0x3ff2041e ),
	/* 6280 */ DATA_1x2( 0x689f323d, 0x3f507afd ),
	/* 6288 */ DATA_1x2( 0xbcaf1aa4, 0x3e4b3a19 ),
	/* 6296 */ 0x3fa50000, 0x3ebb8e96, /* 037 */
	/* 6304 */ DATA_1x2( 0xf92668b9, 0x3ff1e82a ),
	/* 6312 */ DATA_1x2( 0xd997474c, 0xbf77d506 ),
	/* 6320 */ DATA_1x2( 0x106e404d, 0xbe360413 ),
	/* 6328 */ 0x3fa60000, 0x3ec0052b, /* 038 */
	/* 6336 */ DATA_1x2( 0x69c50564, 0x3ff1cc8d ),
	/* 6344 */ DATA_1x2( 0x8a0ac8a0, 0x3f691ad3 ),
	/* 6352 */ DATA_1x2( 0xa19575b0, 0x3e28b0e2 ),
	/* 6360 */ 0x3fa70000, 0x3ec474e4, /* 039 */
	/* 6368 */ DATA_1x2( 0x84baa4c9, 0x3ff1b144 ),
	/* 6376 */ DATA_1x2( 0x8ab66e3b, 0xbf6d76f6 ),
	/* 6384 */ DATA_1x2( 0xb4b69337, 0xbe4a3e9b ),
	/* 6392 */ 0x3fa80000, 0x3ec8ddd4, /* 040 */
	/* 6400 */ DATA_1x2( 0xc6fc9491, 0x3ff1964e ),
	/* 6408 */ DATA_1x2( 0xfc9490d5, 0x3f764ec6 ),
	/* 6416 */ DATA_1x2( 0x1169656a, 0x3e423e2e ),
	/* 6424 */ 0x3fa90000, 0x3ecd4012, /* 041 */
	/* 6432 */ DATA_1x2( 0xb6a94976, 0x3ff17baa ),
	/* 6440 */ DATA_1x2( 0x5ada271b, 0xbf515525 ),
	/* 6448 */ DATA_1x2( 0x432d124c, 0xbe3b8773 ),
	/* 6456 */ 0x3faa0000, 0x3ed19bb0, /* 042 */
	/* 6464 */ DATA_1x2( 0xe2c365a4, 0x3ff16156 ),
	/* 6472 */ DATA_1x2( 0x3c9a5bca, 0xbf7ea91d ),
	/* 6480 */ DATA_1x2( 0xa13af882, 0x3e44fec0 ),
	/* 6488 */ 0x3fab0000, 0x3ed5f0c4, /* 043 */
	/* 6496 */ DATA_1x2( 0xe2ef2aa9, 0x3ff14751 ),
	/* 6504 */ DATA_1x2( 0xbcaaa4f4, 0x3f5d478b ),
	/* 6512 */ DATA_1x2( 0xdc796e37, 0xbe3a0382 ),
	/* 6520 */ 0x3fac0000, 0x3eda3f60, /* 044 */
	/* 6528 */ DATA_1x2( 0x57323dc3, 0x3ff12d9a ),
	/* 6536 */ DATA_1x2( 0xcdc23cf4, 0xbf7265a8 ),
	/* 6544 */ DATA_1x2( 0xbeb7d722, 0xbe418efa ),
	/* 6552 */ 0x3fad0000, 0x3ede8797, /* 045 */
	/* 6560 */ DATA_1x2( 0xe7b5a678, 0x3ff1142e ),
	/* 6568 */ DATA_1x2( 0xb5a677ee, 0x3f742ee7 ),
	/* 6576 */ DATA_1x2( 0x6156885a, 0x3e43cf20 ),
	/* 6584 */ 0x3fae0000, 0x3ee2c97d, /* 046 */
	/* 6592 */ DATA_1x2( 0x4489f08c, 0x3ff0fb0e ),
	/* 6600 */ DATA_1x2( 0xd83dd0a6, 0xbf53c6ed ),
	/* 6608 */ DATA_1x2( 0xacfcfdcb, 0x3e4a52b6 ),
	/* 6616 */ 0x3faf0000, 0x3ee70525, /* 047 */
	/* 6624 */ DATA_1x2( 0x256d5b6c, 0x3ff0e237 ),
	/* 6632 */ DATA_1x2( 0x92a493ae, 0xbf7dc8da ),
	/* 6640 */ DATA_1x2( 0x066d45ec, 0xbe4b8b4f ),
	/* 6648 */ 0x3fb00000, 0x3eeb3a9f, /* 048 */
	/* 6656 */ DATA_1x2( 0x4994022d, 0x3ff0c9a8 ),
	/* 6664 */ DATA_1x2( 0x28045a51, 0x3f635093 ),
	/* 6672 */ DATA_1x2( 0x7f1f5f0d, 0x3de97507 ),
	/* 6680 */ 0x3fb10000, 0x3eef69ff, /* 049 */
	/* 6688 */ DATA_1x2( 0x7771e821, 0x3ff0b160 ),
	/* 6696 */ DATA_1x2( 0x1c2fbd56, 0xbf6d3f11 ),
	/* 6704 */ DATA_1x2( 0x676289cd, 0xbe477b93 ),
	/* 6712 */ 0x3fb20000, 0x3ef39355, /* 050 */
	/* 6720 */ DATA_1x2( 0x7c86d702, 0x3ff0995e ),
	/* 6728 */ DATA_1x2( 0x86d70181, 0x3f795e7c ),
	/* 6736 */ DATA_1x2( 0x20c519e1, 0x3e1e754d ),
	/* 6744 */ 0x3fb30000, 0x3ef7b6b4, /* 051 */
	/* 6752 */ DATA_1x2( 0x2d2bfc6b, 0x3ff081a1 ),
	/* 6760 */ DATA_1x2( 0xbfc6b540, 0x3f3a12d2 ),
	/* 6768 */ DATA_1x2( 0xbf05c3fd, 0xbe49ae3b ),
	/* 6776 */ 0x3fb40000, 0x3efbd42b, /* 052 */
	/* 6784 */ DATA_1x2( 0x64633554, 0x3ff06a27 ),
	/* 6792 */ DATA_1x2( 0x9ccaac06, 0xbf75d89b ),
	/* 6800 */ DATA_1x2( 0x9d9c3263, 0x3e41960d ),
	/* 6808 */ 0x3fb50000, 0x3effebcd, /* 053 */
	/* 6816 */ DATA_1x2( 0x03a7f6cd, 0x3ff052f0 ),
	/* 6824 */ DATA_1x2( 0xa7f6cd26, 0x3f72f003 ),
	/* 6832 */ DATA_1x2( 0x9518ce03, 0xbe35f001 ),
	/* 6840 */ 0x3fb60000, 0x3f01fed4, /* 054 */
	/* 6848 */ DATA_1x2( 0xf2c1c437, 0x3ff03bf9 ),
	/* 6856 */ DATA_1x2( 0xf8ef2450, 0xbf501834 ),
	/* 6864 */ DATA_1x2( 0xfe672869, 0x3e572f32 ),
	/* 6872 */ 0x3fb70000, 0x3f0404e8, /* 055 */
	/* 6880 */ DATA_1x2( 0x1f9823ab, 0x3ff02544 ),
	/* 6888 */ DATA_1x2( 0x67dc5545, 0xbf7abbe0 ),
	/* 6896 */ DATA_1x2( 0x855b4988, 0xbe5b011f ),
	/* 6904 */ 0x3fb80000, 0x3f060828, /* 056 */
	/* 6912 */ DATA_1x2( 0x7e080215, 0x3ff00ecd ),
	/* 6920 */ DATA_1x2( 0x100429de, 0x3f6d9afc ),
	/* 6928 */ DATA_1x2( 0xcb104aea, 0x3e1ac754 ),
	/* 6936 */ 0x3fb90000, 0x3f08089e, /* 057 */
	/* 6944 */ DATA_1x2( 0x0f74f227, 0x3feff12a ),
	/* 6952 */ DATA_1x2( 0x161bb241, 0xbf5dabe1 ),
	/* 6960 */ DATA_1x2( 0x2b09c645, 0xbe5d586e ),
	/* 6968 */ 0x3fba0000, 0x3f0a0650, /* 058 */
	/* 6976 */ DATA_1x2( 0x77f9d292, 0x3fefc533 ),
	/* 6984 */ DATA_1x2( 0xe74a4813, 0x3f44cddf ),
	/* 6992 */ DATA_1x2( 0xf187a96b, 0xbe45786a ),
	/* 7000 */ 0x3fbb0000, 0x3f0c0146, /* 059 */
	/* 7008 */ DATA_1x2( 0x3f34b8cd, 0x3fef99b5 ),
	/* 7016 */ DATA_1x2( 0x34b8cd79, 0x3f69b53f ),
	/* 7024 */ DATA_1x2( 0xd49d71d3, 0x3e5ee52e ),
	/* 7032 */ 0x3fbc0000, 0x3f0df989, /* 060 */
	/* 7040 */ DATA_1x2( 0x796c4570, 0x3fef6ead ),
	/* 7048 */ DATA_1x2( 0x93ba9037, 0xbf615286 ),
	/* 7056 */ DATA_1x2( 0x21c4aec5, 0xbe26a2ff ),
	/* 7064 */ 0x3fbd0000, 0x3f0fef1f, /* 061 */
	/* 7072 */ DATA_1x2( 0x454f4101, 0x3fef441a ),
	/* 7080 */ DATA_1x2( 0x3d0405ea, 0x3f406915 ),
	/* 7088 */ DATA_1x2( 0xb37b7d45, 0xbe59e2fd ),
	/* 7096 */ 0x3fbe0000, 0x3f11e20f, /* 062 */
	/* 7104 */ DATA_1x2( 0xcbae7ffd, 0x3fef19f9 ),
	/* 7112 */ DATA_1x2( 0xae7ffd6e, 0x3f69f9cb ),
	/* 7120 */ DATA_1x2( 0x6fefe267, 0xbe57b1b0 ),
	/* 7128 */ 0x3fbf0000, 0x3f13d260, /* 063 */
	/* 7136 */ DATA_1x2( 0x3f38faa1, 0x3feef04a ),
	/* 7144 */ DATA_1x2( 0x8e0abe14, 0xbf5f6b81 ),
	/* 7152 */ DATA_1x2( 0xe015e13c, 0x3e46172f ),
	/* 7160 */ 0x3fc00000, 0x3f15c01a, /* 064 */
	/* 7168 */ DATA_1x2( 0xdc3a03fd, 0x3feec709 ),
	/* 7176 */ DATA_1x2( 0xe80ff5d2, 0x3f4c2770 ),
	/* 7184 */ DATA_1x2( 0x43cfd006, 0x3e4cfdeb ),
	/* 7192 */ 0x3fc10000, 0x3f17ab44, /* 065 */
	/* 7200 */ DATA_1x2( 0xe8598c97, 0x3fee9e36 ),
	/* 7208 */ DATA_1x2( 0x598c9756, 0x3f6e36e8 ),
	/* 7216 */ DATA_1x2( 0xb3d7b0e6, 0xbe542981 ),
	/* 7224 */ 0x3fc20000, 0x3f1993e3, /* 066 */
	/* 7232 */ DATA_1x2( 0xb25e5dae, 0x3fee75cf ),
	/* 7240 */ DATA_1x2( 0x4344a363, 0xbf54609b ),
	/* 7248 */ DATA_1x2( 0x4d90d724, 0x3e556939 ),
	/* 7256 */ 0x3fc30000, 0x3f1b7a00, /* 067 */
	/* 7264 */ DATA_1x2( 0x91f23b11, 0x3fee4dd2 ),
	/* 7272 */ DATA_1x2( 0xe4762260, 0x3f5ba523 ),
	/* 7280 */ DATA_1x2( 0x76fee235, 0xbe4249ba ),
	/* 7288 */ 0x3fc40000, 0x3f1d5da0, /* 068 */
	/* 7296 */ DATA_1x2( 0xe767da1d, 0x3fee263d ),
	/* 7304 */ DATA_1x2( 0x9825e325, 0xbf69c218 ),
	/* 7312 */ DATA_1x2( 0x64ccd537, 0xbe457f7a ),
	/* 7320 */ 0x3fc50000, 0x3f1f3eca, /* 069 */
	/* 7328 */ DATA_1x2( 0x1b829d3b, 0x3fedff10 ),
	/* 7336 */ DATA_1x2( 0xac58aceb, 0xbf1dfc8f ),
	/* 7344 */ DATA_1x2( 0xd32a3ab1, 0xbe50c11f ),
	/* 7352 */ 0x3fc60000, 0x3f211d84, /* 070 */
	/* 7360 */ DATA_1x2( 0x9f4003df, 0x3fedd847 ),
	/* 7368 */ DATA_1x2( 0x4003de81, 0x3f68479f ),
	/* 7376 */ DATA_1x2( 0x698f89e3, 0xbe267955 ),
	/* 7384 */ 0x3fc70000, 0x3f22f9d5, /* 071 */
	/* 7392 */ DATA_1x2( 0xeba2bfab, 0x3fedb1e2 ),
	/* 7400 */ DATA_1x2( 0xba80a993, 0xbf5c3a28 ),
	/* 7408 */ DATA_1x2( 0x1d6ca000, 0xbe4d77e3 ),
	/* 7416 */ 0x3fc80000, 0x3f24d3c2, /* 072 */
	/* 7424 */ DATA_1x2( 0x817f5ffe, 0x3fed8be0 ),
	/* 7432 */ DATA_1x2( 0xfebffb1d, 0x3f57c102 ),
	/* 7440 */ DATA_1x2( 0x15fc9258, 0x3e579a37 ),
	/* 7448 */ 0x3fc90000, 0x3f26ab53, /* 073 */
	/* 7456 */ DATA_1x2( 0xe94a85b9, 0x3fed663e ),
	/* 7464 */ DATA_1x2( 0xb57a4735, 0xbf69c116 ),
	/* 7472 */ DATA_1x2( 0xeed64840, 0xbe4330c4 ),
	/* 7480 */ 0x3fca0000, 0x3f28808c, /* 074 */
	/* 7488 */ DATA_1x2( 0xb2e891bc, 0x3fed40fc ),
	/* 7496 */ DATA_1x2( 0x123775c7, 0x3f1f965d ),
	/* 7504 */ DATA_1x2( 0xe377a525, 0x3e4c22a3 ),
	/* 7512 */ 0x3fcb0000, 0x3f2a5374, /* 075 */
	/* 7520 */ DATA_1x2( 0x757ec0f0, 0x3fed1c18 ),
	/* 7528 */ DATA_1x2( 0x7ec0efb9, 0x3f6c1875 ),
	/* 7536 */ DATA_1x2( 0x1b636195, 0x3e594a87 ),
	/* 7544 */ 0x3fcc0000, 0x3f2c2411, /* 076 */
	/* 7552 */ DATA_1x2( 0xcf45a967, 0x3fecf790 ),
	/* 7560 */ DATA_1x2( 0x74ad31f7, 0xbf50de61 ),
	/* 7568 */ DATA_1x2( 0xcf0e362f, 0x3e4a6274 ),
	/* 7576 */ 0x3fcd0000, 0x3f2df268, /* 077 */
	/* 7584 */ DATA_1x2( 0x655d0c7a, 0x3fecd364 ),
	/* 7592 */ DATA_1x2( 0x5d0c7a7f, 0x3f636465 ),
	/* 7600 */ DATA_1x2( 0x6955d67e, 0x3e596a28 ),
	/* 7608 */ 0x3fce0000, 0x3f2fbe80, /* 078 */
	/* 7616 */ DATA_1x2( 0xe3a0f252, 0x3fecaf91 ),
	/* 7624 */ DATA_1x2( 0x5f0dadde, 0xbf606e1c ),
	/* 7632 */ DATA_1x2( 0xa28e69ca, 0xbe57c3ec ),
	/* 7640 */ 0x3fcf0000, 0x3f31885c, /* 079 */
	/* 7648 */ DATA_1x2( 0xfc8003b3, 0x3fec8c17 ),
	/* 7656 */ DATA_1x2( 0x000766e0, 0x3f582ff9 ),
	/* 7664 */ DATA_1x2( 0x9080d4b4, 0x3e5eaa60 ),
	/* 7672 */ 0x3fd00000, 0x3f335004, /* 080 */
	/* 7680 */ DATA_1x2( 0x68d31760, 0x3fec68f5 ),
	/* 7688 */ DATA_1x2( 0x2ce89fe3, 0xbf670a97 ),
	/* 7696 */ DATA_1x2( 0x979a5db7, 0x3e5c8f11 ),
	/* 7704 */ 0x3fd10000, 0x3f35157d, /* 081 */
	/* 7712 */ DATA_1x2( 0xe7b5e8b8, 0x3fec4628 ),
	/* 7720 */ DATA_1x2( 0xd7a2df61, 0x3f48a39e ),
	/* 7728 */ DATA_1x2( 0x5f7f66a9, 0xbe2a5f0f ),
	/* 7736 */ 0x3fd20000, 0x3f36d8cb, /* 082 */
	/* 7744 */ DATA_1x2( 0x3e60edb5, 0x3fec23b1 ),
	/* 7752 */ DATA_1x2( 0x9f124b78, 0xbf6c4ec1 ),
	/* 7760 */ DATA_1x2( 0x93b2fbe1, 0x3e54ec32 ),
	/* 7768 */ 0x3fd30000, 0x3f3899f5, /* 083 */
	/* 7776 */ DATA_1x2( 0x380442b9, 0x3fec018d ),
	/* 7784 */ DATA_1x2( 0x442b8874, 0x3f28d380 ),
	/* 7792 */ DATA_1x2( 0x106ba601, 0xbe43aa4e ),
	/* 7800 */ 0x3fd40000, 0x3f3a58ff, /* 084 */
	/* 7808 */ DATA_1x2( 0xa5a3a303, 0x3febdfbb ),
	/* 7816 */ DATA_1x2( 0xa3a30287, 0x3f6fbba5 ),
	/* 7824 */ DATA_1x2( 0x58723510, 0xbe5363f1 ),
	/* 7832 */ 0x3fd50000, 0x3f3c15ee, /* 085 */
	/* 7840 */ DATA_1x2( 0x5df364f3, 0x3febbe3b ),
	/* 7848 */ DATA_1x2( 0xc9b0d1c5, 0xbf2c4a20 ),
	/* 7856 */ DATA_1x2( 0x421bc0f7, 0xbdf12cd4 ),
	/* 7864 */ 0x3fd60000, 0x3f3dd0c8, /* 086 */
	/* 7872 */ DATA_1x2( 0x3d3671a3, 0x3feb9d0b ),
	/* 7880 */ DATA_1x2( 0x3671a2cd, 0x3f6d0b3d ),
	/* 7888 */ DATA_1x2( 0x6fd85522, 0xbe4b2bf4 ),
	/* 7896 */ 0x3fd70000, 0x3f3f8991, /* 087 */
	/* 7904 */ DATA_1x2( 0x251d2f9e, 0x3feb7c2a ),
	/* 7912 */ DATA_1x2( 0x16830c39, 0xbf3eaed7 ),
	/* 7920 */ DATA_1x2( 0x5d12ecd7, 0x3e282cf1 ),
	/* 7928 */ 0x3fd80000, 0x3f41404f, /* 088 */
	/* 7936 */ DATA_1x2( 0xfca558e1, 0x3feb5b96 ),
	/* 7944 */ DATA_1x2( 0xa558e14b, 0x3f6b96fc ),
	/* 7952 */ DATA_1x2( 0x1a4847f8, 0xbe54831f ),
	/* 7960 */ 0x3fd90000, 0x3f42f506, /* 089 */
	/* 7968 */ DATA_1x2( 0xaffab47d, 0x3feb3b50 ),
	/* 7976 */ DATA_1x2( 0x152e0b5d, 0xbf42bd40 ),
	/* 7984 */ DATA_1x2( 0xb3def206, 0xbe5e9b09 ),
	/* 7992 */ 0x3fda0000, 0x3f44a7ba, /* 090 */
	/* 8000 */ DATA_1x2( 0x3058ac9d, 0x3feb1b56 ),
	/* 8008 */ DATA_1x2( 0x58ac9d77, 0x3f6b5630 ),
	/* 8016 */ DATA_1x2( 0x1680dd46, 0x3e560ddf ),
	/* 8024 */ 0x3fdb0000, 0x3f465872, /* 091 */
	/* 8032 */ DATA_1x2( 0x73ecb9db, 0x3feafba6 ),
	/* 8040 */ DATA_1x2( 0x4d189532, 0xbf416630 ),
	/* 8048 */ DATA_1x2( 0x09325dd6, 0xbe42d311 ),
	/* 8056 */ 0x3fdc0000, 0x3f480731, /* 092 */
	/* 8064 */ DATA_1x2( 0x75b99d15, 0x3feadc40 ),
	/* 8072 */ DATA_1x2( 0xb99d150d, 0x3f6c4075 ),
	/* 8080 */ DATA_1x2( 0x66037816, 0xbe53fffa ),
	/* 8088 */ 0x3fdd0000, 0x3f49b3fb, /* 093 */
	/* 8096 */ DATA_1x2( 0x357b614b, 0x3feabd23 ),
	/* 8104 */ DATA_1x2( 0x24f5a4cc, 0xbf36e654 ),
	/* 8112 */ DATA_1x2( 0x5d3990cb, 0x3e5b4156 ),
	/* 8120 */ 0x3fde0000, 0x3f4b5ed7, /* 094 */
	/* 8128 */ DATA_1x2( 0xb78c1f20, 0x3fea9e4d ),
	/* 8136 */ DATA_1x2( 0x8c1f2065, 0x3f6e4db7 ),
	/* 8144 */ DATA_1x2( 0x1420276e, 0xbe5aa694 ),
	/* 8152 */ 0x3fdf0000, 0x3f4d07c7, /* 095 */
	/* 8160 */ DATA_1x2( 0x04c97bf9, 0x3fea7fbf ),
	/* 8168 */ DATA_1x2( 0xa101b217, 0xbf003ecd ),
	/* 8176 */ DATA_1x2( 0x6042d519, 0xbe589e3f ),
	/* 8184 */ 0x3fe00000, 0x3f4eaed0, /* 096 */
	/* 8192 */ DATA_1x2( 0x2a7aded9, 0x3fea6176 ),
	/* 8200 */ DATA_1x2( 0x852126c1, 0xbf6e89d5 ),
	/* 8208 */ DATA_1x2( 0x64ccd537, 0xbe357f7a ),
	/* 8216 */ 0x3fe10000, 0x3f5053f7, /* 097 */
	/* 8224 */ DATA_1x2( 0x3a385553, 0x3fea4372 ),
	/* 8232 */ DATA_1x2( 0xc2aa994c, 0x3f3b91d1 ),
	/* 8240 */ DATA_1x2( 0x4c673b45, 0xbe46cfbb ),
	/* 8248 */ 0x3fe20000, 0x3f51f740, /* 098 */
	/* 8256 */ DATA_1x2( 0x49d2231b, 0x3fea25b2 ),
	/* 8264 */ DATA_1x2( 0x2ddce4b6, 0xbf6a4db6 ),
	/* 8272 */ DATA_1x2( 0xc25f0ce6, 0xbe58e3cf ),
	/* 8280 */ 0x3fe30000, 0x3f5398af, /* 099 */
	/* 8288 */ DATA_1x2( 0x7338f6f8, 0x3fea0835 ),
	/* 8296 */ DATA_1x2( 0x71edf06b, 0x3f506ae6 ),
	/* 8304 */ DATA_1x2( 0x708553ec, 0xbe5fa1be ),
	/* 8312 */ 0x3fe40000, 0x3f553848, /* 100 */
	/* 8320 */ DATA_1x2( 0xd466bffe, 0x3fe9eafa ),
	/* 8328 */ DATA_1x2( 0x99400225, 0xbf65052b ),
	/* 8336 */ DATA_1x2( 0x59064390, 0xbe54ffd6 ),
	/* 8344 */ 0x3fe50000, 0x3f56d60f, /* 101 */
	/* 8352 */ DATA_1x2( 0x8f481e2d, 0x3fe9ce01 ),
	/* 8360 */ DATA_1x2( 0x903c59a3, 0x3f5c031e ),
	/* 8368 */ DATA_1x2( 0xcb4558b6, 0x3e4c4720 ),
	/* 8376 */ 0x3fe60000, 0x3f587209, /* 102 */
	/* 8384 */ DATA_1x2( 0xc9a669bb, 0x3fe9b148 ),
	/* 8392 */ DATA_1x2( 0xb32c89d0, 0xbf5d6e6c ),
	/* 8400 */ DATA_1x2( 0xaf5e9bb5, 0x3e4af321 ),
	/* 8408 */ 0x3fe70000, 0x3f5a0c3a, /* 103 */
	/* 8416 */ DATA_1x2( 0xad124c76, 0x3fe994cf ),
	/* 8424 */ DATA_1x2( 0x124c7593, 0x3f64cfad ),
	/* 8432 */ DATA_1x2( 0xe84d0e8d, 0xbe56adfe ),
	/* 8440 */ 0x3fe80000, 0x3f5ba4a4, /* 104 */
	/* 8448 */ DATA_1x2( 0x66cee8d2, 0x3fe97895 ),
	/* 8456 */ DATA_1x2( 0xc45cb8fa, 0xbf4daa64 ),
	/* 8464 */ DATA_1x2( 0xb49696e3, 0x3e5eaa65 ),
	/* 8472 */ 0x3fe90000, 0x3f5d3b4e, /* 105 */
	/* 8480 */ DATA_1x2( 0x27bd8a6e, 0x3fe95c99 ),
	/* 8488 */ DATA_1x2( 0xbd8a6df9, 0x3f6c9927 ),
	/* 8496 */ DATA_1x2( 0x37e20f02, 0xbe58c36d ),
	/* 8504 */ 0x3fea0000, 0x3f5ed039, /* 106 */
	/* 8512 */ DATA_1x2( 0x2449dbe4, 0x3fe940da ),
	/* 8520 */ DATA_1x2( 0x3b7c74fd, 0x3f1b4489 ),
	/* 8528 */ DATA_1x2( 0x92574910, 0xbe39cc0c ),
	/* 8536 */ 0x3feb0000, 0x3f60636a, /* 107 */
	/* 8544 */ DATA_1x2( 0x94569df3, 0x3fe92557 ),
	/* 8552 */ DATA_1x2( 0xa9620cf9, 0xbf6aa86b ),
	/* 8560 */ DATA_1x2( 0x4d8b66a7, 0x3e41f177 ),
	/* 8568 */ 0x3fec0000, 0x3f61f4e5, /* 108 */
	/* 8576 */ DATA_1x2( 0xb32adc32, 0x3fe90a10 ),
	/* 8584 */ DATA_1x2( 0x55b863ff, 0x3f542166 ),
	/* 8592 */ DATA_1x2( 0xa99b4c5a, 0x3e370d02 ),
	/* 8600 */ 0x3fed0000, 0x3f6384ad, /* 109 */
	/* 8608 */ DATA_1x2( 0xbf5f9b89, 0x3fe8ef04 ),
	/* 8616 */ DATA_1x2( 0xa06476b8, 0xbf60fb40 ),
	/* 8624 */ DATA_1x2( 0x8ec17936, 0x3e5d23c3 ),
	/* 8632 */ 0x3fee0000, 0x3f6512c7, /* 110 */
	/* 8640 */ DATA_1x2( 0xfacdfeeb, 0x3fe8d432 ),
	/* 8648 */ DATA_1x2( 0xcdfeea96, 0x3f6432fa ),
	/* 8656 */ DATA_1x2( 0x4e5008e3, 0xbe3ab667 ),
	/* 8664 */ 0x3fef0000, 0x3f669f35, /* 111 */
	/* 8672 */ DATA_1x2( 0xaa7ddec9, 0x3fe8b99a ),
	/* 8680 */ DATA_1x2( 0x0884da1e, 0xbf499556 ),
	/* 8688 */ DATA_1x2( 0xd8486e92, 0x3e195120 ),
	/* 8696 */ 0x3ff00000, 0x3f6829fb, /* 112 */
	/* 8704 */ DATA_1x2( 0x1694cffe, 0x3fe89f3b ),
	/* 8712 */ DATA_1x2( 0x94cffdf7, 0x3f6f3b16 ),
	/* 8720 */ DATA_1x2( 0x2ce6312f, 0x3e5a4c11 ),
	/* 8728 */ 0x3ff10000, 0x3f69b31e, /* 113 */
	/* 8736 */ DATA_1x2( 0x8a4596d5, 0x3fe88513 ),
	/* 8744 */ DATA_1x2( 0x165b522f, 0x3f444e29 ),
	/* 8752 */ DATA_1x2( 0x55ad0f91, 0xbe5b019c ),
	/* 8760 */ 0x3ff20000, 0x3f6b3a9f, /* 114 */
	/* 8768 */ DATA_1x2( 0x53c0032a, 0x3fe86b23 ),
	/* 8776 */ DATA_1x2( 0x3ffcd597, 0xbf64dcac ),
	/* 8784 */ DATA_1x2( 0x7f1f5f0d, 0x3df97507 ),
	/* 8792 */ 0x3ff30000, 0x3f6cc083, /* 115 */
	/* 8800 */ DATA_1x2( 0xc421328f, 0x3fe85169 ),
	/* 8808 */ DATA_1x2( 0x21328f5f, 0x3f6169c4 ),
	/* 8816 */ DATA_1x2( 0x530f101c, 0x3e40f598 ),
	/* 8824 */ 0x3ff40000, 0x3f6e44cd, /* 116 */
	/* 8832 */ DATA_1x2( 0x2f643580, 0x3fe837e6 ),
	/* 8840 */ DATA_1x2( 0x3794ffcf, 0xbf5033a1 ),
	/* 8848 */ DATA_1x2( 0xd8bcce75, 0x3e567fea ),
	/* 8856 */ 0x3ff50000, 0x3f6fc781, /* 117 */
	/* 8864 */ DATA_1x2( 0xec5314e4, 0x3fe81e97 ),
	/* 8872 */ DATA_1x2( 0x5314e3e2, 0x3f6e97ec ),
	/* 8880 */ DATA_1x2( 0x897de908, 0x3e10d5e5 ),
	/* 8888 */ 0x3ff60000, 0x3f7148a1, /* 118 */
	/* 8896 */ DATA_1x2( 0x54783511, 0x3fe8057e ),
	/* 8904 */ DATA_1x2( 0xe0d442fc, 0x3f45f951 ),
	/* 8912 */ DATA_1x2( 0x803f7555, 0x3e5c1c02 ),
	/* 8920 */ 0x3ff70000, 0x3f72c832, /* 119 */
	/* 8928 */ DATA_1x2( 0xc41013af, 0x3fe7ec98 ),
	/* 8936 */ DATA_1x2( 0xefec50bf, 0xbf63673b ),
	/* 8944 */ DATA_1x2( 0x8d4f3773, 0xbe3bbee9 ),
	/* 8952 */ 0x3ff80000, 0x3f744636, /* 120 */
	/* 8960 */ DATA_1x2( 0x99fb5dee, 0x3fe7d3e6 ),
	/* 8968 */ DATA_1x2( 0xfb5ded84, 0x3f63e699 ),
	/* 8976 */ DATA_1x2( 0x1aabbcb8, 0xbe593b2b ),
	/* 8984 */ 0x3ff90000, 0x3f75c2b0, /* 121 */
	/* 8992 */ DATA_1x2( 0x37b15c86, 0x3fe7bb67 ),
	/* 9000 */ DATA_1x2( 0x3a8de901, 0xbf426321 ),
	/* 9008 */ DATA_1x2( 0x13cad28e, 0xbe4cd5dc ),
	/* 9016 */ 0x3ffa0000, 0x3f773da4, /* 122 */
	/* 9024 */ DATA_1x2( 0x0132b331, 0x3fe7a31a ),
	/* 9032 */ DATA_1x2( 0xcd4ccec1, 0xbf6ce5fe ),
	/* 9040 */ DATA_1x2( 0x5f05247c, 0xbe5c98ad ),
	/* 9048 */ 0x3ffb0000, 0x3f78b714, /* 123 */
	/* 9056 */ DATA_1x2( 0x5cfc7134, 0x3fe78afe ),
	/* 9064 */ DATA_1x2( 0xf8e26838, 0x3f55fcb9 ),
	/* 9072 */ DATA_1x2( 0x03b21929, 0x3e2db763 ),
	/* 9080 */ 0x3ffc0000, 0x3f7a2f04, /* 124 */
	/* 9088 */ DATA_1x2( 0xb3fb70c1, 0x3fe77313 ),
	/* 9096 */ DATA_1x2( 0x091e7dc8, 0xbf59d898 ),
	/* 9104 */ DATA_1x2( 0xaa9c9ab8, 0x3e579e0c ),
	/* 9112 */ 0x3ffd0000, 0x3f7ba578, /* 125 */
	/* 9120 */ DATA_1x2( 0x71800307, 0x3fe75b59 ),
	/* 9128 */ DATA_1x2( 0x8003072d, 0x3f6b5971 ),
	/* 9136 */ DATA_1x2( 0xa450bc93, 0xbe5e20a0 ),
	/* 9144 */ 0x3ffe0000, 0x3f7d1a71, /* 126 */
	/* 9152 */ DATA_1x2( 0x0331e6cc, 0x3fe743cf ),
	/* 9160 */ DATA_1x2( 0x8f365d77, 0x3f3e7819 ),
	/* 9168 */ DATA_1x2( 0x993adae9, 0xbe5d107b ),
	/* 9176 */ 0x3fff0000, 0x3f7e8df2, /* 127 */
	/* 9184 */ DATA_1x2( 0xd9048786, 0x3fe72c73 ),
	/* 9192 */ DATA_1x2( 0xfb787a63, 0xbf638c26 ),
	/* 9200 */ DATA_1x2( 0xf2856444, 0x3e58fe55 ),
	/* 9208 */ 0x40000000, 0x3f800000, /* 128 */
	/* 9216 */ DATA_1x2( 0x652b82fe, 0x3fe71547 ),
	/* 9224 */ DATA_1x2( 0x2b82fe17, 0x3f654765 ),
	/* 9232 */ DATA_1x2( 0x00000000, 0x00000000 ),
	};

#else

    extern const double TABLE_NAME[1154]; 

#endif

#define POW2_HI(j)		*((double *) ((char *) TABLE_NAME + 0 + (j)))
#define POW2_LO_OV_POW2_HI(j)	*((double *) ((char *) TABLE_NAME + 8 + (j)))
#define IPOW2(j)		*((signed __int64 *) ((char *) TABLE_NAME + 0 + (j)))
#define POW2_INDEX_POS		4 
#define	POW2_LO_CHECK_F		*((unsigned __int64 *) ((char *) TABLE_NAME + 4096))
#define	POW2_HI_CHECK_F		*((unsigned __int64 *) ((char *) TABLE_NAME + 4104))
#define	POW2_MAX_SCALE_F		*((unsigned __int64 *) ((char *) TABLE_NAME + 4112))
#define	POW2_LO_CHECK_R		*((unsigned __int64 *) ((char *) TABLE_NAME + 4120))
#define	POW2_HI_CHECK_R		*((unsigned __int64 *) ((char *) TABLE_NAME + 4128))
#define	POW2_MAX_SCALE_R		*((unsigned __int64 *) ((char *) TABLE_NAME + 4136))
#define SCALE_DOWN_EXP	11 
#define	FAST_BIG		*((double *) ((char *) TABLE_NAME + 4144))
#define	SCALE_DOWN_OV_LN2	*((double *) ((char *) TABLE_NAME + 4152))
#define	SCALE_DOWN_OV_LN2_HI	*((double *) ((char *) TABLE_NAME + 4160))
#define	SCALE_DOWN_OV_LN2_LO	*((double *) ((char *) TABLE_NAME + 4168))
#define	FAST_EXP_RANGE_CHECK_F	*((unsigned __int64 *) ((char *) TABLE_NAME + 4176))
#define	FAST_EXP_RANGE_CHECK_R	*((unsigned __int64 *) ((char *) TABLE_NAME + 4184))
#define	FAST_POW2_F		((double *) ((char *) TABLE_NAME + 4192))
#define	FAST_POW2_R		((double *) ((char *) TABLE_NAME + 4232))
#define	SCALE_DOWN		*((double *) ((char *) TABLE_NAME + 4256))
#define ACC_BIG_HI_32		0x42b80001 
#define FAST_BIG_HI_32		0x42080001 
#define	LN2_HI			*((double *) ((char *) TABLE_NAME + 4264))
#define	LN2_LO			*((double *) ((char *) TABLE_NAME + 4272))
#define	LN2_OV_LN10_HI			*((double *) ((char *) TABLE_NAME + 4280))
#define	LN2_OV_LN10_LO			*((double *) ((char *) TABLE_NAME + 4288))
#define	ACC_POW2_F		((double *) ((char *) TABLE_NAME + 4296))
#define	ACC_POW2_R		((double *) ((char *) TABLE_NAME + 4336))
#define	ACC_BIG			*((double *) ((char *) TABLE_NAME + 4360))
#define	EXP_LO_CHECK_F		*((unsigned __int64 *) ((char *) TABLE_NAME + 4368))
#define	EXP_HI_CHECK_F		*((unsigned __int64 *) ((char *) TABLE_NAME + 4376))
#define	EXP_LO_CHECK_R		*((unsigned __int64 *) ((char *) TABLE_NAME + 4384))
#define	EXP_HI_CHECK_R		*((unsigned __int64 *) ((char *) TABLE_NAME + 4392))
#define	EXP2_HI_CHECK_F		*((unsigned __int64 *) ((char *) TABLE_NAME + 4400))
#define	EXP2_HI_CHECK_R		*((unsigned __int64 *) ((char *) TABLE_NAME + 4408))
#define	EXP10_LO_CHECK_F		*((unsigned __int64 *) ((char *) TABLE_NAME + 4416))
#define	EXP10_HI_CHECK_F		*((unsigned __int64 *) ((char *) TABLE_NAME + 4424))
#define	EXP10_LO_CHECK_R		*((unsigned __int64 *) ((char *) TABLE_NAME + 4432))
#define	EXP10_HI_CHECK_R		*((unsigned __int64 *) ((char *) TABLE_NAME + 4440))
#define	ACC_EXP_F		((double *) ((char *) TABLE_NAME + 4448))
#define	ACC_EXP10_F		((double *) ((char *) TABLE_NAME + 4488))
#define	EXPM1_POLY_CHECK_F	*((unsigned __int64 *) ((char *) TABLE_NAME + 4536))
#define	EXPM1_HI_CHECK_F	*((unsigned __int64 *) ((char *) TABLE_NAME + 4544))
#define	EXPM1_LO_CHECK_F	*((unsigned __int64 *) ((char *) TABLE_NAME + 4552))
#define	EXPM1_POLY_CHECK_R	*((unsigned __int64 *) ((char *) TABLE_NAME + 4560))
#define	EXPM1_HI_CHECK_R	*((unsigned __int64 *) ((char *) TABLE_NAME + 4568))
#define	EXPM1_LO_CHECK_R	*((unsigned __int64 *) ((char *) TABLE_NAME + 4576))
#define	EXPM1_F			((double *) ((char *) TABLE_NAME + 4584))
#define	EXPM1_RED_F		((double *) ((char *) TABLE_NAME + 4632))
#define	EXPM1_R			((double *) ((char *) TABLE_NAME + 4680))
#define	EXPM1_RED_R		((double *) ((char *) TABLE_NAME + 4712))
#define	SINHCOSH_OVERFLOW_CHECK_F	*((unsigned __int64 *) ((char *) TABLE_NAME + 4744))
#define	SINHCOSH_BIG_CHECK_F	*((unsigned __int64 *) ((char *) TABLE_NAME + 4752))
#define	SINHCOSH_POLY_CHECK_F	*((unsigned __int64 *) ((char *) TABLE_NAME + 4760))
#define	SINHCOSH_OVERFLOW_CHECK_R	*((unsigned __int64 *) ((char *) TABLE_NAME + 4768))
#define	SINHCOSH_BIG_CHECK_R	*((unsigned __int64 *) ((char *) TABLE_NAME + 4776))
#define	SINHCOSH_POLY_CHECK_R	*((unsigned __int64 *) ((char *) TABLE_NAME + 4784))
#define	SINH_F			((double *) ((char *) TABLE_NAME + 4792))
#define	COSH_F			((double *) ((char *) TABLE_NAME + 4832))
#define	SINH_R			((double *) ((char *) TABLE_NAME + 4872))
#define	COSH_R			((double *) ((char *) TABLE_NAME + 4896))
#define LOG2_K			7
#define POW2_K			8
#define NO_FAST			0
#define NO_ACC			0
#define USE_DIVIDE		0
#define	HALF			*((double *) ((char *) TABLE_NAME + 4928))
#define	ONE			*((double *) ((char *) TABLE_NAME + 4936))
#define	TWO			*((double *) ((char *) TABLE_NAME + 4944))
#define	MAX_FLOAT		*((double *) ((char *) TABLE_NAME + 4952))
#define	RECIP_LN2		*((double *) ((char *) TABLE_NAME + 4960))
#define	LN10_OV_LN2		*((double *) ((char *) TABLE_NAME + 4968))
#define	LN2_OVER_TWO		*((double *) ((char *) TABLE_NAME + 4976))
#define	ACC_LOG2_F		((double *) ((char *) TABLE_NAME + 4984))
#define	ACC_LOG2_R		((double *) ((char *) TABLE_NAME + 5032))
#define	FAST_LOG2_F		((double *) ((char *) TABLE_NAME + 5064))
#define GET_F(j)		*((float *) ((char *) TABLE_NAME + 5112 + (j)))
#define LOG_F_HI(j)		*((float *) ((char *) TABLE_NAME + 5116 + (j)))
#define RECIP_F(j)		*((double *) ((char *) TABLE_NAME + 5120 + (j)))
#define RECIP_F_LO(j)		*((double *) ((char *) TABLE_NAME + 5128 + (j)))
#define LOG_F_LO(j)		*((double *) ((char *) TABLE_NAME + 5136 + (j)))
#define LOG_INDEX_BASE_POS	5 
#define LOG_INDEX_SCALE		1 

# define FAST_POW2_POLY_F_M(x) (((FAST_POW2_F[0]+x*FAST_POW2_F[1])+(x*x)*FAST_POW2_F[2])+(x*(x*x))*(FAST_POW2_F[3] \
	+x*FAST_POW2_F[4]))
# define FAST_POW2_POLY_F_C(x) (FAST_POW2_F[0]+x*(FAST_POW2_F[1]+x*(FAST_POW2_F[2]+x*(FAST_POW2_F[3] \
	+x*FAST_POW2_F[4]))))
# define FAST_POW2_POLY_F SELECT_POLY(FAST_POW2_POLY_F_)

# define FAST_POW2_POLY_R_M(x) ((FAST_POW2_R[0]+x*FAST_POW2_R[1])+(x*x)*FAST_POW2_R[2])
# define FAST_POW2_POLY_R_C(x) (FAST_POW2_R[0]+x*(FAST_POW2_R[1]+x*FAST_POW2_R[2]))
# define FAST_POW2_POLY_R SELECT_POLY(FAST_POW2_POLY_R_)

# define ACC_POW2_POLY_F_M(t,x) (((t+x*ACC_POW2_F[0])+(x*x)*ACC_POW2_F[1])+(x*(x*x))*((ACC_POW2_F[2] \
	+x*ACC_POW2_F[3])+(x*x)*ACC_POW2_F[4]))
# define ACC_POW2_POLY_F_C(t,x) (t+x*(ACC_POW2_F[0]+x*(ACC_POW2_F[1]+x*(ACC_POW2_F[2] \
	+x*(ACC_POW2_F[3]+x*ACC_POW2_F[4])))))
# define ACC_POW2_POLY_F SELECT_POLY(ACC_POW2_POLY_F_)

# define ACC_POW2_POLY_R_M(x) ((ACC_POW2_R[0]+x*ACC_POW2_R[1])+(x*x)*ACC_POW2_R[2])
# define ACC_POW2_POLY_R_C(x) (ACC_POW2_R[0]+x*(ACC_POW2_R[1]+x*ACC_POW2_R[2]))
# define ACC_POW2_POLY_R SELECT_POLY(ACC_POW2_POLY_R_)

# define ACC_EXP_POLY_F_M(t,x) (((t+x*ACC_EXP_F[0])+(x*x)*ACC_EXP_F[1])+(x*(x*x))*((ACC_EXP_F[2] \
	+x*ACC_EXP_F[3])+(x*x)*ACC_EXP_F[4]))
# define ACC_EXP_POLY_F_C(t,x) (t+x*(ACC_EXP_F[0]+x*(ACC_EXP_F[1]+x*(ACC_EXP_F[2] \
	+x*(ACC_EXP_F[3]+x*ACC_EXP_F[4])))))
# define ACC_EXP_POLY_F SELECT_POLY(ACC_EXP_POLY_F_)

# define ACC_EXP10_POLY_F_M(t,x) (((t+x*ACC_EXP10_F[0])+(x*x)*(ACC_EXP10_F[1]+x*ACC_EXP10_F[2])) \
	+((x*x)*(x*x))*((ACC_EXP10_F[3]+x*ACC_EXP10_F[4])+(x*x)*ACC_EXP10_F[5]))
# define ACC_EXP10_POLY_F_C(t,x) (t+x*(ACC_EXP10_F[0]+x*(ACC_EXP10_F[1]+x*(ACC_EXP10_F[2] \
	+x*(ACC_EXP10_F[3]+x*(ACC_EXP10_F[4]+x*ACC_EXP10_F[5]))))))
# define ACC_EXP10_POLY_F SELECT_POLY(ACC_EXP10_POLY_F_)

# define EXPM1_POLY_F_M(x) (x) + (((x*x)*((EXPM1_F[0]+x*EXPM1_F[1]) \
	+(x*x)*EXPM1_F[2]))+((x*x)*(x*(x*x)))*((EXPM1_F[3]+x*EXPM1_F[4])+(x*x)*EXPM1_F[5]))
# define EXPM1_POLY_F_C(x) (x) + ((x*x)*(EXPM1_F[0]+x*(EXPM1_F[1] \
	+x*(EXPM1_F[2]+x*(EXPM1_F[3]+x*(EXPM1_F[4]+x*EXPM1_F[5]))))))
# define EXPM1_POLY_F SELECT_POLY(EXPM1_POLY_F_)

# define EXPM1_RED_POLY_F_M(t,x) (((t+(x*x)*EXPM1_RED_F[0])+(x*(x*x))*(EXPM1_RED_F[1] \
	+x*EXPM1_RED_F[2]))+((x*x)*(x*(x*x)))*((EXPM1_RED_F[3]+x*EXPM1_RED_F[4])+(x*x)*EXPM1_RED_F[5]))
# define EXPM1_RED_POLY_F_C(t,x) (t+(x*x)*(EXPM1_RED_F[0]+x*(EXPM1_RED_F[1] \
	+x*(EXPM1_RED_F[2]+x*(EXPM1_RED_F[3]+x*(EXPM1_RED_F[4]+x*EXPM1_RED_F[5]))))))
# define EXPM1_RED_POLY_F SELECT_POLY(EXPM1_RED_POLY_F_)

# define EXPM1_POLY_R_M(x) ((x*(EXPM1_R[0]+x*EXPM1_R[1]))+(x*(x*x))*(EXPM1_R[2] \
	+x*EXPM1_R[3]))
# define EXPM1_POLY_R_C(x) (x*(EXPM1_R[0]+x*(EXPM1_R[1]+x*(EXPM1_R[2] \
	+x*EXPM1_R[3]))))
# define EXPM1_POLY_R SELECT_POLY(EXPM1_POLY_R_)

# define EXPM1_RED_POLY_R_M(x) ((x*(EXPM1_RED_R[0]+x*EXPM1_RED_R[1]))+(x*(x*x))*(EXPM1_RED_R[2] \
	+x*EXPM1_RED_R[3]))
# define EXPM1_RED_POLY_R_C(x) (x*(EXPM1_RED_R[0]+x*(EXPM1_RED_R[1]+x*(EXPM1_RED_R[2] \
	+x*EXPM1_RED_R[3]))))
# define EXPM1_RED_POLY_R SELECT_POLY(EXPM1_RED_POLY_R_)

# define SINH_POLY_F_M(x) (x) + ((x*(x*x))*(((SINH_F[0]+(x*x)*SINH_F[1])+((x*x)*(x*x))*SINH_F[2])+((x*x)*((x*x)*(x*x)))*(SINH_F[3]+(x*x)*SINH_F[4])))
# define SINH_POLY_F_C(x) (x) + ((x*(x*x))*(SINH_F[0]+(x*x)*(SINH_F[1]+(x*x)*(SINH_F[2]+(x*x)*(SINH_F[3]+(x*x)*SINH_F[4])))))
# define SINH_POLY_F SELECT_POLY(SINH_POLY_F_)

# define COSH_POLY_F_M(x) ONE + (((x*x)*(COSH_F[0] \
	+(x*x)*COSH_F[1]))+((x*x)*((x*x)*(x*x)))*((COSH_F[2] \
	+(x*x)*COSH_F[3])+((x*x)*(x*x))*COSH_F[4]))
# define COSH_POLY_F_C(x) ONE + ((x*x)*(COSH_F[0] \
	+(x*x)*(COSH_F[1]+(x*x)*(COSH_F[2] \
	+(x*x)*(COSH_F[3]+(x*x)*COSH_F[4])))))
# define COSH_POLY_F SELECT_POLY(COSH_POLY_F_)

# define SINHCOSH_ODD_POLY_F_M(x) (((x*ACC_EXP_F[0])+(x*x)*(x*ACC_EXP_F[2])) \
	+((x*x)*(x*x))*(x*ACC_EXP_F[4]))
# define SINHCOSH_ODD_POLY_F_C(x) (x*(ACC_EXP_F[0]+(x*x)*(ACC_EXP_F[2]+(x*x)*ACC_EXP_F[4])))
# define SINHCOSH_ODD_POLY_F SELECT_POLY(SINHCOSH_ODD_POLY_F_)

# define SINHCOSH_EVEN_POLY_F_M(x) ((x*x)*(ACC_EXP_F[1] \
	+(x*x)*ACC_EXP_F[3]))
# define SINHCOSH_EVEN_POLY_F_C(x) ((x*x)*(ACC_EXP_F[1] \
	+(x*x)*ACC_EXP_F[3]))
# define SINHCOSH_EVEN_POLY_F SELECT_POLY(SINHCOSH_EVEN_POLY_F_)

# define SINH_POLY_R_M(x) (((x*SINH_R[0])+(x*x)*(x*SINH_R[1])) \
	+((x*x)*(x*x))*(x*SINH_R[2]))
# define SINH_POLY_R_C(x) (x*(SINH_R[0]+(x*x)*(SINH_R[1]+(x*x)*SINH_R[2])))
# define SINH_POLY_R SELECT_POLY(SINH_POLY_R_)

# define COSH_POLY_R_M(x) ((COSH_R[0]+(x*x)*COSH_R[1]) \
	+((x*x)*(x*x))*(COSH_R[2]+(x*x)*COSH_R[3]))
# define COSH_POLY_R_C(x) (COSH_R[0]+(x*x)*(COSH_R[1] \
	+(x*x)*(COSH_R[2]+(x*x)*COSH_R[3])))
# define COSH_POLY_R SELECT_POLY(COSH_POLY_R_)

# define SINHCOSH_ODD_POLY_R_M(x) (x*ACC_POW2_R[1])
# define SINHCOSH_ODD_POLY_R_C(x) (x*ACC_POW2_R[1])
# define SINHCOSH_ODD_POLY_R SELECT_POLY(SINHCOSH_ODD_POLY_R_)

# define SINHCOSH_EVEN_POLY_R_M(x) (ACC_POW2_R[0]+(x*x)*ACC_POW2_R[2])
# define SINHCOSH_EVEN_POLY_R_C(x) (ACC_POW2_R[0]+(x*x)*ACC_POW2_R[2])
# define SINHCOSH_EVEN_POLY_R SELECT_POLY(SINHCOSH_EVEN_POLY_R_)

# define ACC_LOG2_POLY_F_M(t,x) (((t+(x*x)*(x*ACC_LOG2_F[0])) \
	+((x*x)*(x*x))*(ACC_LOG2_F[1]+x*ACC_LOG2_F[2]))+((x*x)*((x*x)*(x*x)))*((ACC_LOG2_F[3]+x*ACC_LOG2_F[4]) \
	+(x*x)*ACC_LOG2_F[5]))
# define ACC_LOG2_POLY_F_C(t,x) (t+(x*(x*x))*(ACC_LOG2_F[0] \
	+x*(ACC_LOG2_F[1]+x*(ACC_LOG2_F[2]+x*(ACC_LOG2_F[3]+x*(ACC_LOG2_F[4] \
	+x*ACC_LOG2_F[5]))))))
# define ACC_LOG2_POLY_F SELECT_POLY(ACC_LOG2_POLY_F_)

# define ACC_LOG2_POLY_R_M(t,x) (((t+x*ACC_LOG2_R[0])+(x*x)*ACC_LOG2_R[1])+(x*(x*x))*(ACC_LOG2_R[2] \
	+x*ACC_LOG2_R[3]))
# define ACC_LOG2_POLY_R_C(t,x) (t+x*(ACC_LOG2_R[0]+x*(ACC_LOG2_R[1]+x*(ACC_LOG2_R[2] \
	+x*ACC_LOG2_R[3]))))
# define ACC_LOG2_POLY_R SELECT_POLY(ACC_LOG2_POLY_R_)

# define FAST_LOG2_POLY_F_M(t,x) ((t+(x*(x*x))*(FAST_LOG2_F[1] \
	+x*FAST_LOG2_F[2]))+((x*x)*(x*(x*x)))*((FAST_LOG2_F[3]+x*FAST_LOG2_F[4])+(x*x)*FAST_LOG2_F[5]))
# define FAST_LOG2_POLY_F_C(t,x) (t+(x*(x*x))*(FAST_LOG2_F[1] \
	+x*(FAST_LOG2_F[2]+x*(FAST_LOG2_F[3]+x*(FAST_LOG2_F[4]+x*FAST_LOG2_F[5])))))
# define FAST_LOG2_POLY_F SELECT_POLY(FAST_LOG2_POLY_F_)

# define FAST_LOG2_POLY_R_M(t,x) (t+(x*(x*x))*(ACC_LOG2_R[2] \
	+x*ACC_LOG2_R[3]))
# define FAST_LOG2_POLY_R_C(t,x) (t+(x*(x*x))*(ACC_LOG2_R[2] \
	+x*ACC_LOG2_R[3]))
# define FAST_LOG2_POLY_R SELECT_POLY(FAST_LOG2_POLY_R_)

