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

#include "bid_trans.h"

// Extra macros

#define CLZ64_MASK32 0xFFFFFFFF00000000ull
#define CLZ64_MASK16 0xFFFF0000FFFF0000ull
#define CLZ64_MASK8  0xFF00FF00FF00FF00ull
#define CLZ64_MASK4  0xF0F0F0F0F0F0F0F0ull
#define CLZ64_MASK2  0xCCCCCCCCCCCCCCCCull
#define CLZ64_MASK1  0xAAAAAAAAAAAAAAAAull

#define clz64_nz(n)                                             \
 (((((n) & CLZ64_MASK32) <= ((n) & ~CLZ64_MASK32)) ? 32 : 0) +  \
  ((((n) & CLZ64_MASK16) <= ((n) & ~CLZ64_MASK16)) ? 16 : 0) +  \
  ((((n) & CLZ64_MASK8) <= ((n) & ~CLZ64_MASK8)) ? 8 : 0) +     \
  ((((n) & CLZ64_MASK4) <= ((n) & ~CLZ64_MASK4)) ? 4 : 0) +     \
  ((((n) & CLZ64_MASK2) <= ((n) & ~CLZ64_MASK2)) ? 2 : 0) +     \
  ((((n) & CLZ64_MASK1) <= ((n) & ~CLZ64_MASK1)) ? 1 : 0))      \

#define sll128_short(hi,lo,c)                                   \
  ((hi) = ((hi) << (c)) + ((lo)>>(64-(c))),                     \
   (lo) = (lo) << (c)                                           \
  )

#define sll192_short(hi,med,lo,c)                               \
  ((hi) = ((hi) << (c)) + ((med)>>(64-(c))),                    \
   (med) = ((med) << (c)) + ((lo)>>(64-(c))),                   \
   (lo) = (lo) << (c)                                           \
  )

#define BID64_NAN 0x7c00000000000000ull

// Values of (10^a / 2 pi) mod 1 for -17 <= a <= 369
// Each one is a 192-bit binary fraction

static BID_UINT192 bid_decimal64_moduli[] =
{ {{ 0x82d9e5c60f747619ull, 0x5be1334254ee2dfaull, 0x000000000000001dull }},
  {{ 0x1c82f9bc9a8c9cf5ull, 0x96cc0097514dcbc9ull, 0x0000000000000125ull }},
  {{ 0x1d1dc15e097e2197ull, 0xe3f805e92d09f5dbull, 0x0000000000000b77ull }},
  {{ 0x23298dac5eed4fe5ull, 0xe7b03b1bc2639a8full, 0x00000000000072aeull }},
  {{ 0x5f9f88bbb5451ef6ull, 0x0ce24f1597e40997ull, 0x0000000000047ad5ull }},
  {{ 0xbc3b575514b3359bull, 0x80d716d7eee85fe9ull, 0x00000000002ccc52ull }},
  {{ 0x5a516952cf00180bull, 0x0866e46f5513bf21ull, 0x0000000001bffb39ull }},
  {{ 0x872e1d3c1600f06full, 0x5404ec5952c5774dull, 0x00000000117fd03aull }},
  {{ 0x47cd2458dc096459ull, 0x48313b7d3bb6a907ull, 0x00000000aefe2247ull }},
  {{ 0xce036b78985deb7bull, 0xd1ec52e455229a48ull, 0x00000006d5ed56c8ull }},
  {{ 0x0c2232b5f3ab32cdull, 0x333b3ceb535a06d8ull, 0x000000445b4563d8ull }},
  {{ 0x7955fb1b84affc02ull, 0x0050613141844470ull, 0x000002ab90b5e672ull }},
  {{ 0xbd5bcf132edfd811ull, 0x0323cbec8f2aac64ull, 0x00001ab3a71b0074ull }},
  {{ 0x659616bfd4be70aaull, 0x1f65f73d97aabbefull, 0x00010b04870e0488ull }},
  {{ 0xf7dce37e4f7066a1ull, 0x39fba867ecab5759ull, 0x000a6e2d468c2d51ull }},
  {{ 0xaea0e2ef1a640248ull, 0x43d4940f3eb16983ull, 0x00684dc4c179c52cull }},
  {{ 0xd248dd5707e816ceull, 0xa64dc89872ee1f24ull, 0x041309af8ec1b3baull }},
  {{ 0x36d8a5664f10e410ull, 0x7f09d5f47d4d3770ull, 0x28be60db9391054aull }},
  {{ 0x247675ff16a8e8a5ull, 0xf6625b8ce5042a62ull, 0x976fc893c3aa34e8ull }},
  {{ 0x6ca09bf6e2991672ull, 0x9fd79380f229a7d5ull, 0xea5dd5c5a4a61119ull }},
  {{ 0x3e4617a4d9fae072ull, 0x3e6bc30975a08e56ull, 0x27aa59b86e7cab00ull }},
  {{ 0x6ebcec7083ccc478ull, 0x70359e5e98458f5eull, 0x8ca7813450deae02ull }},
  {{ 0x53613c6525ffacacull, 0x62182fb1f2b799b0ull, 0x7e8b0c0b28b2cc18ull }},
  {{ 0x41cc5bf37bfcbeb5ull, 0xd4f1dcf37b2c00e3ull, 0xf16e786f96fbf8f3ull }},
  {{ 0x91fb9782d7df7316ull, 0x5172a182cfb808e0ull, 0x6e50b45be5d7b986ull }},
  {{ 0xb3d3eb1c6eba7ed7ull, 0x2e7a4f1c1d3058c5ull, 0x4f270b96fa6d3f3full }},
  {{ 0x06472f1c5348f467ull, 0xd0c7171923e377b9ull, 0x178673e5c8447877ull }},
  {{ 0x3ec7d71b40d98c03ull, 0x27c6e6fb66e2ad3aull, 0xeb4086f9d2acb4aeull }},
  {{ 0x73ce6710887f781eull, 0x8dc505d204dac446ull, 0x308545c23abf0ecdull }},
  {{ 0x861006a554fab12aull, 0x89b23a34308baac0ull, 0xe534b9964b769407ull }},
  {{ 0x3ca0427551caeba0ull, 0x60f64609e574ab85ull, 0xf40f3fdef2a1c84bull }},
  {{ 0x5e42989531ed3443ull, 0xc99ebc62f68eb334ull, 0x88987eb57a51d2f1ull }},
  {{ 0xae99f5d3f3440a9cull, 0xe0335bdda193000bull, 0x55f4f316c7323d71ull }},
  {{ 0xd2039a4780a86a14ull, 0xc20196a84fbe0074ull, 0x5b917ee3c7f66672ull }},
  {{ 0x342406cb069424c4ull, 0x940fe2931d6c0490ull, 0x93aef4e5cfa0007bull }},
  {{ 0x096843ee41c96faaull, 0xc89ed9bf26382da2ull, 0xc4d590fa1c4004d3ull }},
  {{ 0x5e12a74e91de5ca6ull, 0xd63481777e31c854ull, 0xb057a9c51a803045ull }},
  {{ 0xacba8911b2af9e80ull, 0x5e0d0eaaedf1d34bull, 0xe36ca1b30901e2baull }},
  {{ 0xbf495ab0fadc30ffull, 0xac8292ad4b7240f4ull, 0xe23e50fe5a12db47ull }},
  {{ 0x78dd8ae9cc99e9f7ull, 0xbd19bac4f276898full, 0xd66f29ef84bc90ccull }},
  {{ 0xb8a76d21fe0323a8ull, 0x63014bb178a15f9aull, 0x6057a35b2f5da7ffull }},
  {{ 0x368a4353ec1f648dull, 0xde0cf4eeb64dbc0bull, 0xc36c618fd9a88ff9ull }},
  {{ 0x2166a1473939ed82ull, 0xac8191531f095870ull, 0xa23bcf9e80959fc2ull }},
  {{ 0x4e024cc83c434711ull, 0xbd0fad3f365d7461ull, 0x56561c3105d83d9aull }},
  {{ 0x0c16ffd25aa0c6a8ull, 0x629cc4781fa68bcdull, 0x5f5d19ea3a72680bull }},
  {{ 0x78e5fe378a47c294ull, 0xda1facb13c817602ull, 0xb9a3032648781071ull }},
  {{ 0xb8fbee2b66cd99c5ull, 0x853cbeec5d0e9c18ull, 0x405e1f7ed4b0a472ull }},
  {{ 0x39d74db2040801aeull, 0x345f753ba29218f7ull, 0x83ad3af44ee66c79ull }},
  {{ 0x426908f4285010d0ull, 0x0bba945459b4f9a8ull, 0x24c44d8b15003cbcull }},
  {{ 0x981a59899320a825ull, 0x7549cb4b8111c092ull, 0x6fab076ed2025f58ull }},
  {{ 0xf1077f5fbf469170ull, 0x94e1f0f30ab185b9ull, 0x5cae4a543417b974ull }},
  {{ 0x6a4af9bd78c1ae63ull, 0xd0d3697e6aef3943ull, 0x9ecee74a08ed3e8dull }},
  {{ 0x26edc166b790cfdaull, 0x28421ef02d583ca2ull, 0x341508e45944718aull }},
  {{ 0x85498e032ba81e83ull, 0x92953561c5725e55ull, 0x08d258eb7cac6f65ull }},
  {{ 0x34df8c1fb4913120ull, 0xb9d415d1b677af57ull, 0x58377932debc59f7ull }},
  {{ 0x10bb793d0dabeb3dull, 0x4248da3120acd968ull, 0x722abbfcb35b83adull }},
  {{ 0xa752bc6288b7305full, 0x96d885eb46c07e10ull, 0x75ab57df019324c4ull }},
  {{ 0x893b5bd95727e3b2ull, 0xe4753b30c384eca6ull, 0x98b16eb60fbf6fadull }},
  {{ 0x5c51967d678ee4f8ull, 0xec944fe7a3313e81ull, 0xf6ee531c9d7a5ccaull }},
  {{ 0x9b2fe0e60b94f1b0ull, 0x3dcb1f0c5fec710dull, 0xa54f3f1e26c79fedull }},
  {{ 0x0fdec8fc73d170e4ull, 0x69ef367bbf3c6a88ull, 0x7518772d83cc3f44ull }},
  {{ 0x9eb3d9dc862e68e9ull, 0x235820d5785c2950ull, 0x92f4a7c725fa78acull }},
  {{ 0x3306829d3dd01918ull, 0x61714856b3999d26ull, 0xbd8e8dc77bc8b6b9ull }},
  {{ 0xfe411a246a20faf1ull, 0xce6cd3630400237dull, 0x679189cad5d7233dull }},
  {{ 0xee8b056c2549cd66ull, 0x104041de280162ebull, 0x0baf61ec5a67606aull }},
  {{ 0x516e363974e205ffull, 0xa28292ad900ddd37ull, 0x74d9d33b8809c424ull }},
  {{ 0x2e4e1e3e90d43bf3ull, 0x5919bac7a08aa429ull, 0x908240535061a96eull }},
  {{ 0xcf0d2e71a84a5783ull, 0x7b014bcc456a699bull, 0xa516834123d09e4full }},
  {{ 0x1683d07092e76b1eull, 0xce0cf5fab6282016ull, 0x72e1208b66262f1aull }},
  {{ 0xe1262465bd0a2f28ull, 0x0c819bcb1d9140dcull, 0x7ccb4571fd7dd70cull }},
  {{ 0xcb7d6bf96265d78eull, 0x7d1015ef27ac88a0ull, 0xdff0b673e6ea6678ull }},
  {{ 0xf2e637bdd7fa6b88ull, 0xe2a0db578cbd5647ull, 0xbf672087052800b4ull }},
  {{ 0x7cfe2d6a6fc83354ull, 0xda48916b7f655ecfull, 0x7a07454633900710ull }},
  {{ 0xe1edc6285dd20144ull, 0x86d5ae32f9f5b41aull, 0xc448b4be03a046a8ull }},
  {{ 0xd349bd93aa340cacull, 0x4458cdfdc399090cull, 0xaad70f6c2442c295ull }},
  {{ 0x40e167c4a6087eb4ull, 0xab780be9a3fa5a80ull, 0xac669a396a9b99d4ull }},
  {{ 0x88ce0dae7c54f308ull, 0xb2b0772067c78902ull, 0xbc02063e2a14024eull }},
  {{ 0x580c88d0db517e53ull, 0xfae4a7440dcb5a19ull, 0x58143e6da4c81712ull }},
  {{ 0x707d5828912eef41ull, 0xccee88a889f184fdull, 0x70ca70486fd0e6bdull }},
  {{ 0x64e57195abd55888ull, 0x01515695636f31e6ull, 0x67e862d45e29036aull }},
  {{ 0xf0f66fd8b6557554ull, 0x0d2d61d5e257f2ffull, 0x0f13dc4bad9a2224ull }},
  {{ 0x69a05e771f56954dull, 0x83c5d25ad76f7dffull, 0x96c69af4c8055568ull }},
  {{ 0x2043b0a73961d4feull, 0x25ba378c6a5aebfaull, 0xe3c20d8fd0355615ull }},
  {{ 0x42a4e6883dd251e8ull, 0x79462b7c278d37c5ull, 0xe594879e22155cd3ull }},
  {{ 0x9a7101526a373314ull, 0xbcbdb2d98b842db4ull, 0xf7cd4c2d54d5a042ull }},
  {{ 0x086a0d382627fecaull, 0x5f68fc7f7329c90eull, 0xae04f9c55058429bull }},
  {{ 0x542484317d8ff3e5ull, 0xba19dcfa7fa1da8cull, 0xcc31c1b523729a11ull }},
  {{ 0x496d29eee79f86f2ull, 0x4502a1c8fc52897bull, 0xf9f19113627a04b1ull }},
  {{ 0xde43a3550c3b4579ull, 0xb21a51d9db395ed0ull, 0xc36faac1d8c42eecull }},
  {{ 0xaea461527a50b6b7ull, 0xf5073282903db428ull, 0xa25cab9277a9d53eull }},
  {{ 0xd26bcd38c727232aull, 0x9247f919a2690996ull, 0x579eb3b8aca25475ull }},
  {{ 0x38360437c7875fa5ull, 0xb6cfbb00581a5fe4ull, 0x6c330536be574c97ull }},
  {{ 0x321c2a2dcb49bc6full, 0x241d4e037107beeaull, 0x39fe34236f68fdedull }},
  {{ 0xf519a5c9f0e15c56ull, 0x69250c226a4d7525ull, 0x43ee09625a19eb43ull }},
  {{ 0x930079e368cd9b60ull, 0x1b7279582706937bull, 0xa74c5dd7850330a2ull }},
  {{ 0xbe04c2e2180811c2ull, 0x1278bd718641c2d3ull, 0x88fbaa6b321fe655ull }},
  {{ 0x6c2f9cd4f050b192ull, 0xb8b7666f3e919c45ull, 0x59d4a82ff53eff52ull }},
  {{ 0x39dc20516326efb7ull, 0x372a005871b01ab6ull, 0x824e91df9475f93bull }},
  {{ 0x4299432ddf855d22ull, 0x27a4037470e10b1eull, 0x1711b2bbcc9bbc50ull }},
  {{ 0x99fc9fcabb35a357ull, 0x8c68228c68ca6f2eull, 0xe6b0fb55fe155b21ull }},
  {{ 0x03de3deb50186164ull, 0x7c11597c17e857d2ull, 0x02e9d15becd58f4full }},
  {{ 0x26ae6b3120f3cde8ull, 0xd8ad7ed8ef136e34ull, 0x1d222d974057991aull }},
  {{ 0x82d02feb49860b15ull, 0x76c6f47956c24e09ull, 0x2355c7e8836bfb0cull }},
  {{ 0x1c21df30df3c6ed0ull, 0xa3c58cbd63970c5full, 0x6159cf152237ce7cull }},
  {{ 0x1952b7e8b85c541cull, 0x65b77f65e3e67bb7ull, 0xcd8216d3562e10deull }},
  {{ 0xfd3b2f17339b491aull, 0xf92af9fae700d526ull, 0x0714e4415dcca8afull }},
  {{ 0xe44fd6e80410db03ull, 0xbbadc3cd06085385ull, 0x46d0ea8da9fe96dfull }},
  {{ 0xeb1e651028a88e1cull, 0x54c9a6023c53433aull, 0xc4292988a3f1e4bdull }},
  {{ 0x2f2ff2a196958d19ull, 0x4fe07c165b40a04dull, 0xa99b9f566772ef65ull }},
  {{ 0xd7df7a4fe1d782f7ull, 0x1ec4d8df90864303ull, 0xa01439600a7d59f5ull }},
  {{ 0x6ebac71ed26b1da8ull, 0x33b078bba53e9e26ull, 0x40ca3dc068e58393ull }},
  {{ 0x534bc734382f2893ull, 0x04e4b75474722d80ull, 0x87e6698418f723c0ull }},
  {{ 0x40f5c80a31d795b9ull, 0x30ef294c8c75c703ull, 0x4f001f28f9a76580ull }},
  {{ 0x8999d065f26bd93cull, 0xe9579cfd7c99c620ull, 0x16013799c089f701ull }},
  {{ 0x600223fb78367c58ull, 0x1d6c21e6de01bd45ull, 0xdc0c2c018563a613ull }},
  {{ 0xc01567d2b220db73ull, 0x26395304ac1164b5ull, 0x9879b80f35e47cbfull }},
  {{ 0x80d60e3af548927full, 0x7e3d3e2eb8adef19ull, 0xf4c130981aecdf77ull }},
  {{ 0x085c8e4d94d5b8f5ull, 0xee646dd336cb56ffull, 0x8f8be5f10d40baaaull }},
  {{ 0x539d8f07d059398eull, 0x4fec4a4023f165f6ull, 0x9b76fb6a84874aadull }},
  {{ 0x4427964e237c3f90ull, 0x1f3ae681676dfb9full, 0x12a5d2292d48eac5ull }},
  {{ 0xa98bdf0d62da7ba2ull, 0x384d010e0a4bd438ull, 0xba7a359bc4d92bb3ull }},
  {{ 0x9f76b685dc88d453ull, 0x33020a8c66f64a36ull, 0x48c61815b07bb500ull }},
  {{ 0x3aa3213a9d584b3full, 0xfe14697c059ee622ull, 0xd7bcf0d8e4d51201ull }},
  {{ 0x4a5f4c4a2572f075ull, 0xeccc1ed83834fd56ull, 0x6d616878f052b413ull }},
  {{ 0xe7b8fae5767d6496ull, 0x3ff934723211e55eull, 0x45ce14b9633b08c7ull }},
  {{ 0x0d39ccf6a0e5eddfull, 0x7fbc0c75f4b2f5b5ull, 0xba0ccf3de04e57c8ull }},
  {{ 0x844201a248fb4ab1ull, 0xfd587c9b8efd9912ull, 0x4480186ac30f6dd4ull }},
  {{ 0x2a941056d9d0eaeeull, 0xe574de1395e7fab9ull, 0xad00f42b9e9a4a51ull }},
  {{ 0xa9c8a36482292d4bull, 0xf690acc3db0fcb3bull, 0xc20989b43206e732ull }},
  {{ 0xa1d661ed159bc4edull, 0xa1a6bfa68e9df054ull, 0x945f6109f44507fdull }},
  {{ 0x525fd342d815b147ull, 0x50837c81922b634eull, 0xcbb9ca638ab24fe8ull }},
  {{ 0x37be409c70d8ecc4ull, 0x2522dd0fb5b1e10full, 0xf541e7e36af71f13ull }},
  {{ 0x2d6e861c68793fa9ull, 0x735ca29d18f2ca98ull, 0x94930ee22da736bfull }},
  {{ 0xc6513d1c14bc7c9bull, 0x819e5a22f97be9f1ull, 0xcdbe94d5c888237aull }},
  {{ 0xbf2c6318cf5cde10ull, 0x102f855dbed72371ull, 0x0971d059d55162c9ull }},
  {{ 0x77bbdef819a0aca3ull, 0xa1db35a974676271ull, 0x5e722382552ddbdaull }},
  {{ 0xad56b5b10046be5bull, 0x5290189e8c09d86eull, 0xb075631753ca968aull }},
  {{ 0xc56318ea02c36f8dull, 0x39a0f63178627452ull, 0xe495dee945e9e167ull }},
  {{ 0xb5def9241ba25b85ull, 0x40499deeb3d88b3bull, 0xeddab51cbb22ce08ull }},
  {{ 0x1ab5bb6914579333ull, 0x82e02b5306757055ull, 0x4a8b131f4f5c0c52ull }},
  {{ 0x0b19521acb6bc000ull, 0x1cc1b13e40966353ull, 0xe96ebf3919987b39ull }},
  {{ 0x6efd350bf2358002ull, 0x1f90ec6e85dfe13eull, 0x1e53783afff4d03bull }},
  {{ 0x55e412777617000full, 0x3ba93c513abecc70ull, 0x2f42b24dff90224full }},
  {{ 0x5ae8b8aa9ce6009aull, 0x549c5b2c4b73fc63ull, 0xd89af70bfba15718ull }},
  {{ 0x8d1736aa20fc0604ull, 0x4e1b8fbaf287dbe1ull, 0x760da677d44d66f3ull }},
  {{ 0x82e822a549d83c27ull, 0x0d139d4d794e96cfull, 0x9c8880ae4b060581ull }},
  {{ 0x1d115a74e2725984ull, 0x82c42506bd11e41bull, 0x1d5506ceee3c370aull }},
  {{ 0x22ad8890d8777f2bull, 0x1ba9724362b2e90full, 0x255244154e5a2669ull }},
  {{ 0x5ac755a874aaf7aaull, 0x149e76a1dafd1a97ull, 0x7536a8d50f85801bull }},
  {{ 0x8bc958948eadaca7ull, 0xce30a2528de309e9ull, 0x942298529b37010eull }},
  {{ 0x75dd75cd92c8be8aull, 0x0de657398ade631full, 0xc959f33a10260a94ull }},
  {{ 0x9aa69a07bbd77163ull, 0x8aff683f6cafdf3aull, 0xdd838044a17c69c8ull }},
  {{ 0x0a82044d566a6de1ull, 0x6dfa127a3edeb84aull, 0xa72302ae4edc21d5ull }},
  {{ 0x69142b0560284acbull, 0x4bc4b8c674b332e4ull, 0x875e1acf14995256ull }},
  {{ 0x1ac9ae35c192ebecull, 0xf5af37c08efffcecull, 0x49ad0c16cdfd375eull }},
  {{ 0x0be0ce198fbd3737ull, 0x98d82d8595ffe139ull, 0xe0c278e40be429b5ull }},
  {{ 0x76c80cff9d64282aull, 0xf871c737dbfecc3aull, 0xc798b8e876e9a117ull }},
  {{ 0xa3d081fc25e991a8ull, 0xb471c82e97f3fa48ull, 0xcbf73914a5204aefull }},
  {{ 0x662513d97b1fb090ull, 0x0c71d1d1ef87c6d6ull, 0xf7a83ace7342ed5dull }},
  {{ 0xfd72c67ecf3ce59dull, 0x7c7232335b4dc45full, 0xac924c10809d45a2ull }},
  {{ 0xe67bc0f41860f81full, 0xdc75f6019109abbfull, 0xbdb6f8a50624b858ull }},
  {{ 0x00d58988f3c9b134ull, 0x9c9b9c0faa60b57full, 0x6925b6723d6f3378ull }},
  {{ 0x08575f5985e0ec04ull, 0x1e14189ca7c716f6ull, 0x1b792076665802b6ull }},
  {{ 0x5369b97f3ac93829ull, 0x2cc8f61e8dc6e59cull, 0x12bb449fff701b1dull }},
  {{ 0x42213ef84bdc3197ull, 0xbfd99d3189c4f81bull, 0xbb50ae3ffa610f23ull }},
  {{ 0x954c75b2f699efe1ull, 0x7e8023ef61b1b110ull, 0x5126ce7fc7ca9765ull }},
  {{ 0xd4fc98fda2035ecfull, 0xf1016759d0f0eaa5ull, 0x2b8410fdcde9e9f6ull }},
  {{ 0x51ddf9e85421b411ull, 0x6a0e098229692a7aull, 0xb328a9ea0b2323a5ull }},
  {{ 0x32abc313495108acull, 0x248c5f159e1ba8c7ull, 0xff96a3246f5f6476ull }},
  {{ 0xfab59ec0dd2a56b6ull, 0x6d7bb6d82d1497c7ull, 0xfbe25f6c59b9ec9dull }},
  {{ 0xcb183388a3a7631eull, 0x46d52471c2cdedcfull, 0xd6d7ba3b81433e26ull }},
  {{ 0xeef203566489df2aull, 0xc4536c719c0b4a1dull, 0x646d46530ca06d7eull }},
  {{ 0x5574215fed62b7a7ull, 0xab423c701870e52bull, 0xec44bf3e7e4446f3ull }},
  {{ 0x56894dbf45db2c85ull, 0xb0965c60f468f3b1ull, 0x3aaf7870eeaac584ull }},
  {{ 0x615d0978ba8fbd37ull, 0xe5df9bc98c1984edull, 0x4adab46952abb72eull }},
  {{ 0xcda25eb7499d6422ull, 0xfabc15df78ff3145ull, 0xec8b0c1d3ab527d4ull }},
  {{ 0x0857b328e025e950ull, 0xcb58dabab9f7ecbaull, 0x3d6e79244b138e51ull }},
  {{ 0x536cff98c17b1d20ull, 0xf1788b4b43af3f44ull, 0x6650bb6aeec38f31ull }},
  {{ 0x4241fbf78ecf2342ull, 0x6eb570f0a4d878abull, 0xff27522d53a397f3ull }},
  {{ 0x9693d7ab94176092ull, 0x531669667074b6b0ull, 0xf78935c54463ef82ull }},
  {{ 0xe1c66cb3c8e9c5b5ull, 0x3ee01e00648f22e5ull, 0xab5c19b4abe75b17ull }},
  {{ 0xd1c03f05d921b916ull, 0x74c12c03ed975cfaull, 0xb199010eb7098ee8ull }},
  {{ 0x3182763a7b513ad9ull, 0x8f8bb82747e9a1ccull, 0xeffa0a93265f9514ull }},
  {{ 0xef189e48d12c4c76ull, 0x9b753188cf2051f9ull, 0x5fc469bf7fbbd2cdull }},
  {{ 0x56f62ed82bbafc98ull, 0x1293ef58174333c3ull, 0xbdac217afd563c08ull }},
  {{ 0x659dd471b54dddf4ull, 0xb9c75970e8a005a1ull, 0x68b94ecde55e5850ull }},
  {{ 0xf82a4c71150aab84ull, 0x41c97e691640384dull, 0x173d140af5af7327ull }},
  {{ 0xb1a6fc6ad26ab328ull, 0x91def01ade82330bull, 0xe862c86d98da7f88ull }},
  {{ 0xf085dc2c382aff8eull, 0xb2b5610cb115fe74ull, 0x13dbd447f888fb55ull }},
  {{ 0x653a99ba31adfb8cull, 0xfb15ca7eeadbf091ull, 0xc6964acfb559d158ull }},
  {{ 0xf44a0145f0cbd377ull, 0xced9e8f52c9765adull, 0xc1deec1d15822d79ull }},
  {{ 0x8ae40cbb67f642a8ull, 0x14831993bde9f8cbull, 0x92b53922d715c6c2ull }},
  {{ 0x6ce87f520f9e9a92ull, 0xcd1effc56b23b7f3ull, 0xbb143b5c66d9c394ull }},
  {{ 0x4114f9349c3209afull, 0x0335fdb62f652f82ull, 0x4eca519c0481a3d0ull }},
  {{ 0x8ad1bc0e19f460daull, 0x201be91dd9f3db16ull, 0x13e730182d106620ull }},
  {{ 0x6c31588d038bc888ull, 0x41171b2a83868ee1ull, 0xc707e0f1c2a3fd41ull }},
  {{ 0x39ed75822375d54bull, 0x8ae70fa9234194ceull, 0xc64ec9719a67e48cull }},
  {{ 0x43469715629a54efull, 0x6d069c9b608fd00eull, 0xbf13de70080eed7dull }},
  {{ 0xa0c1e6d5da075157ull, 0x42421e11c59e208eull, 0x76c6b060509546e6ull }},
  {{ 0x4793045a84492d63ull, 0x96952cb1b82d4592ull, 0xa3c2e3c325d4c4feull }},
  {{ 0xcbbe2b892adbc5daull, 0xe1d3bef131c4b7b6ull, 0x659ce59f7a4fb1f1ull }},
  {{ 0xf56db35bac95ba86ull, 0xd245756bf1af2d23ull, 0xf820f83ac71cf372ull }},
  {{ 0x96490194bdd9493dull, 0x36b6963770d7c367ull, 0xb149b24bc721827cull }},
  {{ 0xdeda0fcf6a7cdc62ull, 0x2321de2a686da20bull, 0xece0f6f5c74f18daull }},
  {{ 0xb4849e1a28e09bd3ull, 0x5f52ada814485476ull, 0x40c9a599c916f885ull }},
  {{ 0x0d2e2d0598c61642ull, 0xb93ac890cad34ca3ull, 0x87e07801dae5b535ull }},
  {{ 0x83cdc237f7bcde94ull, 0x3c4bd5a7ec40fe5eull, 0x4ec4b0128cf91419ull }},
  {{ 0x2609962fad60b1c6ull, 0x5af6588f3a89efb1ull, 0x13aee0b981bac8fcull }},
  {{ 0x7c5fdddcc5c6f1bfull, 0x8d9f759849635cebull, 0xc4d4c73f114bd9dbull }},
  {{ 0xdbbeaa9fb9c5717aull, 0x883a97f2dde1a132ull, 0xb04fc876acf68293ull }},
  {{ 0x9572aa3d41b66ec8ull, 0x5249ef7caad04bfcull, 0xe31dd4a2c1a119c3ull }},
  {{ 0xd67aa664912053ccull, 0x36e35adeac22f7ddull, 0xdf2a4e5b904b01a1ull }},
  {{ 0x60ca7fedab4345f6ull, 0x24e18cb2b95daeaaull, 0xb7a70f93a2ee104cull }},
  {{ 0xc7e8ff48b0a0bb9dull, 0x70cf7efb3da8d2a7ull, 0x2c869bc45d4ca2f9ull }},
  {{ 0xcf19f8d6e6475422ull, 0x681af5d068983a8dull, 0xbd4215aba4fe5dbeull }},
  {{ 0x1703b864fec94951ull, 0x110d9a2415f2498aull, 0x6494d8b471efa970ull }},
  {{ 0xe62533f1f3dcdd25ull, 0xaa880568db76df64ull, 0xedd0770c735c9e60ull }},
  {{ 0xfd74077386a0a376ull, 0xa950361892a4b9f0ull, 0x4a24a67c819e2fc6ull }},
  {{ 0xe6884a8342466298ull, 0x9d221cf5ba6f4369ull, 0xe56e80dd102dddc2ull }},
  {{ 0x0152e92096bfd9eeull, 0x235521994858a223ull, 0xf65108a2a1caa99aull }},
  {{ 0x0d3d1b45e37e8347ull, 0x61534ffcd376555eull, 0x9f2a565a51eaa005ull }},
  {{ 0x846310bae2f120c8ull, 0xcd411fe0429f55acull, 0x37a75f87332a4035ull }},
  {{ 0x2bdea74cdd6b47d4ull, 0x048b3ec29a3958bdull, 0x2c89bb47ffa6821aull }},
  {{ 0xb6b28900a630ce4bull, 0x2d70739a063d7763ull, 0xbd6150cffc811504ull }},
  {{ 0x22f95a067de80ef1ull, 0xc66484043e66a9e5ull, 0x65cd281fdd0ad229ull }},
  {{ 0x5dbd8440eb109565ull, 0xbfed282a7002a2f3ull, 0xfa03913ea26c35a1ull }},
  {{ 0xa9672a892ea5d5f6ull, 0x7f4391a8601a5d81ull, 0xc423ac72583a1851ull }},
  {{ 0x9e07a95bd27a5b9cull, 0xf8a3b093c107a710ull, 0xa964bc777244f32eull }},
  {{ 0x2c4c9d9638c7941bull, 0xb664e5c58a4c86a6ull, 0x9def5caa76b17fd5ull }},
  {{ 0xbafe27de37cbc90cull, 0x1ff0f9b766fd427dull, 0x2b599ea8a2eefe59ull }},
  {{ 0x4ded8eae2df5da7cull, 0x3f69c12a05e498e9ull, 0xb18032965d55ef7bull }},
  {{ 0x0b4792cdcb9a88d4ull, 0x7a218ba43aedf91dull, 0xef01f9dfa55b5ad0ull }},
  {{ 0x70cbbc09f4095845ull, 0xc54f746a4d4bbb22ull, 0x5613c2bc75918c24ull }},
  {{ 0x67f55863885d72aeull, 0xb51a8c2704f54f58ull, 0x5cc59b5c97af796full }},
  {{ 0x0f9573e353a67acbull, 0x1309798631951974ull, 0x9fb8119decdabe5dull }},
  {{ 0x9bd686e14480cbf2ull, 0xbe5ebf3defd2fe88ull, 0x3d30b02b408b6fa2ull }},
  {{ 0x166144ccad07f76full, 0x6fb3786b5e3df156ull, 0x63e6e1b085725c5bull }},
  {{ 0xdfccaffec24faa58ull, 0x5d02b431ae6b6d5cull, 0xe704d0e536779b92ull }},
  {{ 0xbdfedff3971ca775ull, 0xa21b09f0d03245a0ull, 0x063028f420ac13b7ull }},
  {{ 0x6bf4bf83e71e8a90ull, 0x550e636821f6b847ull, 0x3de1998946b8c52cull }},
  {{ 0x378f7b270731699dull, 0x528fe21153a332caull, 0x6acfff5cc337b3bbull }},
  {{ 0x2b9acf8647ee2021ull, 0x399ed4ad445ffbe6ull, 0x2c1ff99fa02d0551ull }},
  {{ 0xb40c1b3ecf4d4146ull, 0x40344ec4abbfd6fdull, 0xb93fc03c41c2352cull }},
  {{ 0x0879107419048cc0ull, 0x820b13aeb57e65e9ull, 0x3c7d825a919613baull }},
  {{ 0x54baa488fa2d7f7eull, 0x146ec4d316effb1aull, 0x5ce71789afdcc549ull }},
  {{ 0x4f4a6d59c5c6faf0ull, 0xcc53b03ee55fcf07ull, 0xa106eb60de9fb4daull }},
  {{ 0x18e84581b9c5cd61ull, 0xfb44e274f5be1649ull, 0x4a4531c8b23d108bull }},
  {{ 0xf912b71141ba05caull, 0xd0b0d891996cdedaull, 0xe6b3f1d6f662a577ull }},
  {{ 0xbabb26ac914439e5ull, 0x26e875affe40b48dull, 0x030772659fda76aeull }},
  {{ 0x4b4f82bdacaa42f2ull, 0x851498dfee870d89ull, 0x1e4a77f83e88a2cdull }},
  {{ 0xf11b1b68bea69d6full, 0x32cdf8bf5146875cull, 0x2ee8afb271565c07ull }},
  {{ 0x6b0f12177282265aull, 0xfc0bb7792cc149a1ull, 0xd516dcf86d5f9847ull }},
  {{ 0x2e96b4ea79157f87ull, 0xd8752abbbf8ce04eull, 0x52e4a1b445bbf2cfull }},
  {{ 0xd1e31128bad6fb4bull, 0x7493ab557b80c30dull, 0x3cee510ab9577c1eull }},
  {{ 0x32deab974c65d0ecull, 0x8dc4b156d3079e8aull, 0x614f2a6b3d6ad930ull }},
  {{ 0xfcb2b3e8fbfa2937ull, 0x89aeed643e4c3165ull, 0xcd17a830662c7be5ull }},
  {{ 0xdefb0719d7c59c23ull, 0x60d545ea6ef9edfbull, 0x02ec91e3fdbcd6f7ull }},
  {{ 0xb5ce47026db81962ull, 0xc854bb2855c34bd6ull, 0x1d3db2e7e96065a9ull }},
  {{ 0x1a0ec6184930fdd1ull, 0xd34f4f9359a0f663ull, 0x2468fd0f1dc3f8a1ull }},
  {{ 0x0493bcf2dbe9ea29ull, 0x41191bc180499fdfull, 0x6c19e29729a7b652ull }},
  {{ 0x2dc5617c9723259aull, 0x8afb158f02e03eb6ull, 0x3902d9e7a08d1f36ull }},
  {{ 0xc9b5cedde75f77ffull, 0x6dced7961cc2731dull, 0x3a1c830c45833821ull }},
  {{ 0xe11a14ab09baaffaull, 0x4a146bdd1f987f29ull, 0x451d1e7ab720314eull }},
  {{ 0xcb04ceae614adfc5ull, 0xe4cc36a33bf4f7a2ull, 0xb32330cb2741ed0eull }},
  {{ 0xee3012cfccecbdafull, 0xeffa22605791ac5bull, 0xff5fe7ef88934294ull }},
  {{ 0x4de0bc1e013f68d3ull, 0x5fc557c36bb0bb97ull, 0xf9bf0f5b55c099d1ull }},
  {{ 0x0ac7592c0c7a1842ull, 0xbdb56da234e753e9ull, 0xc17699915986022dull }},
  {{ 0x6bc97bb87cc4f297ull, 0x691648561109471aull, 0x8ea1ffad7f3c15c9ull }},
  {{ 0x35ded534dfb179ebull, 0x1aded35caa5cc708ull, 0x9253fcc6f858d9deull }},
  {{ 0x1ab45410bceec32eull, 0x0cb4419ea79fc652ull, 0xb747dfc5b37882adull }},
  {{ 0x0b0b48a761539fc8ull, 0x7f0a90328c3dbf35ull, 0x28cebdb902b51ac2ull }},
  {{ 0x6e70d689cd443dd3ull, 0xf669a1f97a697812ull, 0x9813693a1b130b98ull }},
  {{ 0x5068616204aa6a42ull, 0xa02053bec81eb0b8ull, 0xf0c21c450ebe73f9ull }},
  {{ 0x2413cdd42ea82691ull, 0x41434573d132e733ull, 0x67951ab2937087c0ull }},
  {{ 0x68c60a49d29181abull, 0x8ca0b6862bfd07ffull, 0x0bd30af9c2654d82ull }},
  {{ 0x17bc66e239af10a9ull, 0x7e47213db7e24ffaull, 0x763e6dc197f50719ull }},
  {{ 0xed5c04d640d6a69bull, 0xeec74c692ed71fc4ull, 0x9e70498fef9246feull }},
  {{ 0x4598305e88628212ull, 0x53c8fc1bd4673db1ull, 0x3062df9f5bb6c5f5ull }},
  {{ 0xb7f1e3b153d914b3ull, 0x45d9d9164c0868ecull, 0xe3dcbc399523bb95ull }},
  {{ 0x2f72e4ed467acefeull, 0xba827adef854193full, 0xe69f5a3fd36553d4ull }},
  {{ 0xda7cf144c0cc15eaull, 0x4918ccb5b348fc77ull, 0x0239867e41f5464full }},
  {{ 0x88e16caf87f8db29ull, 0xdaf7ff1900d9dcaeull, 0x163f40ee9394bf18ull }},
  {{ 0x58ce3edb4fb88f96ull, 0x8daff6fa08829ed1ull, 0xde788951c3cf76f8ull }},
  {{ 0x780e74911d359bdbull, 0x88dfa5c4551a342dull, 0xb0b55d31a61aa5b5ull }},
  {{ 0xb0908dab2418168aull, 0x58bc79ab530609c6ull, 0xe715a3f07d0a7917ull }},
  {{ 0xe5a588af68f0e167ull, 0x775cc0b13e3c61c2ull, 0x06d86764e268bae9ull }},
  {{ 0xf87756da1968ce01ull, 0xa99f86ec6e5bd19cull, 0x447409f0d8174d1eull }},
  {{ 0xb4a96484fe180c0bull, 0xa03b453c4f963021ull, 0xac88636870e90332ull }},
  {{ 0x0e9ded31ecf07870ull, 0x4250b45b1bdde151ull, 0xbd53e214691a1ffaull }},
  {{ 0x922b43f34164b462ull, 0x97270b8f16aacd2aull, 0x6546d4cc1b053fc6ull }},
  {{ 0xb5b0a7808def0bd8ull, 0xe7867396e2ac03a9ull, 0xf4c44ff90e347dc1ull }},
  {{ 0x18e68b058b56766cull, 0x0b4083e4dab824a1ull, 0x8fab1fba8e0ce993ull }},
  {{ 0xf9016e377160a03cull, 0x708526f08b316e4aull, 0x9caf3d498c811fbeull }},
  {{ 0xba0e4e2a6dc64255ull, 0x653385656fee4eedull, 0x1ed864df7d0b3d70ull }},
  {{ 0x448f0da849be9755ull, 0xf40335f65f4f1549ull, 0x3473f0bae2706663ull }},
  {{ 0xad968892e171e956ull, 0x88201b9fb916d4dcull, 0x0c87674cd863ffe7ull }},
  {{ 0xc7e155bcce731d58ull, 0x5141143d3ae4509eull, 0x7d4a090073e7ff0bull }},
  {{ 0xcecd5960107f2575ull, 0x2c8aca644ceb2633ull, 0xe4e45a04870ff671ull }},
  {{ 0x14057dc0a4f7768eull, 0xbd6be7eb012f7e06ull, 0xf0eb842d469fa06bull }},
  {{ 0xc836e98671aaa18eull, 0x66370f2e0bdaec3cull, 0x693329c4c23c4435ull }},
  {{ 0xd2251f4070aa4f8bull, 0xfe2697cc768d3a5full, 0x1bffa1af965aaa15ull }},
  {{ 0x3573388466a71b6cull, 0xed81edfca18447beull, 0x17fc50dbdf8aa4dbull }},
  {{ 0x1680352c02871238ull, 0x47134bde4f2acd6eull, 0xefdb2896bb6a7097ull }},
  {{ 0xe10213b81946b62cull, 0xc6c0f6af17ac064cull, 0x5e8f95e3522865e8ull }},
  {{ 0xca14c530fcc31db5ull, 0xc389a2d6ecb83f00ull, 0xb19bdae13593fb17ull }},
  {{ 0xe4cfb3e9df9f2914ull, 0xa3605c653f327607ull, 0xf0168ccc17c7ceedull }},
  {{ 0xf01d0722bc379ac8ull, 0x61c39bf477f89c4eull, 0x60e17ff8edce1548ull }},
  {{ 0x6122475b5a2c0bccull, 0xd1a4178cafb61b15ull, 0xc8ceffb94a0cd4d3ull }},
  {{ 0xcb56c99185b875fbull, 0x3068eb7edd1d0ed5ull, 0xd815fd3ce4805046ull }},
  {{ 0xf163dfaf39349bcdull, 0xe41932f4a3229459ull, 0x70dbe460ed0322bdull }},
  {{ 0x6de6bcd83c0e1600ull, 0xe8fbfd8e5f59cb83ull, 0x6896ebc9421f5b6aull }},
  {{ 0x4b036072588cdbfdull, 0x19d7e78fb981f322ull, 0x15e535dc9539922dull }},
  {{ 0xee21c477758097e3ull, 0x026f0b9d3f137f56ull, 0xdaf41a9dd43fb5c3ull }},
  {{ 0x4d51acaa9705eedfull, 0x1856742476c2f965ull, 0x8d890a2a4a7d199eull }},
  {{ 0x0530bea9e63b54b8ull, 0xf360896ca39dbdf5ull, 0x875a65a6e8e3002cull }},
  {{ 0x33e772a2fe514f31ull, 0x81c55e3e64296b92ull, 0x4987f88518de01c1ull }},
  {{ 0x070a7a5def2d17e6ull, 0x11b5ae6fe99e33b6ull, 0xdf4fb532f8ac118full }},
  {{ 0x4668c7ab57c2eefeull, 0xb118d05f202e051cull, 0xb91d13fdb6b8af96ull }},
  {{ 0xc017ccb16d9d55ecull, 0xeaf823b741cc331aull, 0x3b22c7e92336dbe2ull }},
  {{ 0x80edfeee48255b36ull, 0x2db1652891f9ff0bull, 0x4f5bcf1b602496ddull }},
  {{ 0x094bf54ed175901full, 0xc8edf395b3c3f673ull, 0x19961711c16de4a3ull }},
  {{ 0x5cf795142e97a136ull, 0xd94b83d905a7a07eull, 0xffdce6b18e4aee65ull }},
  {{ 0xa1abd2c9d1ec4c1aull, 0x7cf3267a388c44efull, 0xfea102ef8eed4ffaull }},
  {{ 0x50b63be2333af905ull, 0xe17f80c6357ab15cull, 0xf24a1d5b95451fc8ull }},
  {{ 0x271e56d6004dba32ull, 0xcefb07be16caed9bull, 0x76e52593d4b33dd8ull }},
  {{ 0x872f645c030945f7ull, 0x15ce4d6ce3ed480full, 0xa4f377c64f006a78ull }},
  {{ 0x47d9eb981e5cbba2ull, 0xda0f0640e744d09bull, 0x7182adbf160428b0ull }},
  {{ 0xce8333f12f9f5451ull, 0x84963e8908b02610ull, 0x6f1ac976dc2996e8ull }},
  {{ 0x1120076bdc394b26ull, 0x2dde715a56e17ca8ull, 0x570bdea4999fe515ull }},
  {{ 0xab404a369a3cef7bull, 0xcab06d8764cede90ull, 0x6676b26e003ef2d3ull }},
  {{ 0xb082e62206615aceull, 0xeae44749f014b1a6ull, 0x00a2f84c02757c45ull }},
  {{ 0xe51cfd543fcd8c0bull, 0x2ceac8e360cef082ull, 0x065db2f81896dabbull }},
  {{ 0xf321e54a7e077872ull, 0xc12bd8e1c815651cull, 0x3fa8fdb0f5e48b4full }},
  {{ 0x7f52f4e8ec4ab472ull, 0x8bb678d1d0d5f321ull, 0x7c99e8e99aed711dull }},
  {{ 0xf93d91193aeb0c72ull, 0x7520b832285b7f4eull, 0xde0319200d466b27ull }},
  {{ 0xbc67aafc4d2e7c75ull, 0x934731f59392f915ull, 0xac1efb4084c02f8aull }},
  {{ 0x5c0caddb03d0dc95ull, 0xc0c7f397c3bdbad9ull, 0xb935d0852f81db69ull }},
  {{ 0x987eca8e26289dd1ull, 0x87cf83eda5694c7dull, 0x3c1a2533db129221ull }},
  {{ 0xf4f3e98d7d962a25ull, 0x4e1b2748761cfce7ull, 0x590574068eb9b54full }},
  {{ 0x91871f86e7dda573ull, 0x0d0f88d49d21e10full, 0x7a36884193411519ull }},
  {{ 0xaf473b450ea8767cull, 0x829b584e2352ca9bull, 0xc621528fc08ad2faull }},
  {{ 0xd8c850b29294a0d5ull, 0x1a11730d613bea14ull, 0xbd4d399d856c3dc9ull }},
  {{ 0x77d326f9b9ce4855ull, 0x04ae7e85cc5724d0ull, 0x65044027363a69dbull }},
  {{ 0xae3f85c1420ed354ull, 0x2ed0f139fb677024ull, 0xf22a81881e48228eull }},
  {{ 0xce7b398c94944143ull, 0xd4296c43d20a616eull, 0x75a90f512ed1598dull }},
  {{ 0x10d03f7dcdca8ca2ull, 0x499e3aa63467ce54ull, 0x989a992bd42d7f8aull }},
  {{ 0xa8227aea09e97e51ull, 0xe02e4a7e0c0e0f48ull, 0xf609fbb649c6fb66ull }},
  {{ 0x9158cd24631eef28ull, 0xc1cee8ec788c98d6ull, 0x9c63d51ee1c5d204ull }},
  {{ 0xad78036bdf355794ull, 0x9215193cb57df861ull, 0x1be65334d1ba342full }},
  {{ 0xc6b02236b8156bcdull, 0xb4d2fc5f16ebb3d0ull, 0x16ff4010314609dbull }},
  {{ 0xc2e1562330d635feull, 0x103ddbb6e5350627ull, 0xe5f880a1ecbc6295ull }},
  {{ 0x9ccd5d5fe85e1beeull, 0xa26a9524f4123d8dull, 0xfbb506533f5bd9d2ull }},
  {{ 0x2005a5bf13ad1748ull, 0x5829d37188b66788ull, 0xd5123f407996823aull }},
  {{ 0x40387976c4c2e8d3ull, 0x71a2426f57200b51ull, 0x52b67884bfe11647ull }},
  {{ 0x8234bea3af9d1842ull, 0x705698596740712cull, 0x3b20b52f7ecadecaull }},
  {{ 0x160f7264dc22f291ull, 0x6361f37e08846bbdull, 0x4f4713daf3ecb3e8ull }},
  {{ 0xdc9a77f0995d79a6ull, 0xe1d382ec552c3562ull, 0x18c6c68d873f0713ull }},
  {{ 0x9e08af65fda6c07cull, 0xd2431d3b53ba15dcull, 0xf7c3c187487646c6ull }},
  {{ 0x2c56d9fbe88384d9ull, 0x369f24514544da9eull, 0xada58f48d49ec3c4ull }},
  {{ 0xbb6483d715233075ull, 0x22376b2cb4b08a2dull, 0xc87798d84e33a5aaull }},
  {{ 0x51ed2666d35fe497ull, 0x562a2fbf0ee565c9ull, 0xd4abf8730e0478a5ull }},
  {{ 0x3343800441beede5ull, 0x5da5dd7694f5f9ddull, 0x4eb7b47e8c2cb675ull }},
  {{ 0x00a3002a91754af2ull, 0xa87aa6a1d19bc2a4ull, 0x132d0cf179bf2095ull }},
  {{ 0x065e01a9ae94ed70ull, 0x94ca825230159a68ull, 0xbfc2816ec17745d8ull }},
  {{ 0x3fac10a0d1d1465full, 0xcfe91735e0d80810ull, 0x7d990e538ea8ba75ull }},
  {{ 0x7cb8a648322cbfb4ull, 0x1f1ae81ac87050a2ull, 0xe7fa8f439297489aull }},
  {{ 0xdf367ed1f5bf7d06ull, 0x370d110bd4632658ull, 0x0fc998a3b9e8d605ull }},
  {{ 0xb820f433997ae238ull, 0x2682aa764bdf7f78ull, 0x9ddff66543185c34ull }},
  {{ 0x31498a03feccd62bull, 0x811aa89ef6bafab7ull, 0x2abf9ff49ef39a09ull }},
  {{ 0xecdf6427f4005db0ull, 0x0b0a9635a34dcb27ull, 0xab7c3f8e3584045full }},
  {{ 0x40b9e98f8803a8e5ull, 0x6e69de186109ef8full, 0xb2da7b8e17282bb6ull }},
  {{ 0x87431f9b502498eeull, 0x5022acf3ca635b98ull, 0xfc88d38ce791b520ull }},
  {{ 0x489f3c11216df949ull, 0x215ac185e7e193f5ull, 0xdd5843810bb11343ull }},
  {{ 0xd63858ab4e4bbcddull, 0x4d8b8f3b0ecfc794ull, 0xa572a30a74eac09full }},
  {{ 0x5e3376b10ef560a2ull, 0x0773984e941dcbd0ull, 0x767a5e68912b8639ull }},
  {{ 0xae02a2ea9595c658ull, 0x4a83f311c929f623ull, 0xa0c7b015abb33e3aull }},
  {{ 0xcc1a5d29d7d9bf70ull, 0xe9277eb1dba39d64ull, 0x47cce0d8b5006e46ull }},
  {{ 0xf907a3a26e817a5full, 0x1b8af2f2946425efull, 0xce00c87712044ec5ull }},
  {{ 0xba4c6458510ec7b4ull, 0x136d7d79cbe97b5full, 0x0c07d4a6b42b13b3ull }},
  {{ 0x46fbeb732a93cd03ull, 0xc246e6c1f71ed1bdull, 0x784e4e8309aec4feull }},
  {{ 0xc5d7327fa9c6021full, 0x96c50393a7343164ull, 0xb30f111e60d3b1f3ull }},
  {{ 0xba67f8fca1bc1535ull, 0xe3b223c48809edefull, 0xfe96ab2fc844f383ull }},
  {{ 0x480fb9de5158d410ull, 0xe4f565ad50634b5dull, 0xf1e2afddd2b18326ull }},
  {{ 0xd09d42af2d78489full, 0xf195f8c523e0f1a4ull, 0x72dadeaa3aef1f84ull }},
  {{ 0x26249ad7c6b2d63aull, 0x6fdbb7b366c97070ull, 0x7c8cb2a64d573b31ull }},
  {{ 0x7d6e0c6dc2fc5e48ull, 0x5e952d0203de6461ull, 0xdd7efa7f05684feeull }},
  {{ 0xe64c7c499ddbaed1ull, 0xb1d3c21426afebceull, 0xa6f5c8f636131f4full }}
};

BID_F80_CONST_DEF( c_zero,    0000000000000000, 0000000000000000 ); // 0.0
BID_F80_CONST_DEF( c_pi_ov_2, 3fff921fb54442d1, 8469898cc51701b8); // pi/2
#if 0
BID_F80_CONST_DEF( c_pi_ov_2, 3fff921fb54442d1, 8469898cc51701b8); // pi/2
BID_F80_CONST_DEF( c_pi,     4000921fb54442d1, 8469898cc51701b8); // pi
BID_F80_CONST_DEF( c_one,    3fff000000000000, 0000000000000000); // 1.0
BID_F80_CONST_DEF( c_half,   3ffe000000000000, 0000000000000000); // 0.5
BID_F80_CONST_DEF( c_8000,   400bf40000000000, 0000000000000000); // 8000
BID_F80_CONST_DEF( c_1e2000, 59f2cf6c9c9bc5f8, 84a294e53edc955f); // 1e2000
BID_F80_CONST_DEF( c_neg_8000, c00bf40000000000, 0000000000000000); //-8000
BID_F80_CONST_DEF( c_1em2000,  260b1ad56d712a5d, 7f02384e5ded39be); // 1e-2000
BID_F80_CONST_DEF( c_12000,     400c770000000000, 0000000000000000); // 12000
BID_F80_CONST_DEF( c_neg_12000, c00c770000000000, 0000000000000000); // -12000
BID_F80_CONST_DEF( c_1_ov_ln_2, 3fff71547652b82f, e1777d0ffda0d23a); // 1/ln(2)
BID_F80_CONST_DEF(c_ln_10,  400026bb1bbb5551, 582dd4adac5705a6); // ln(10)
BID_F80_CONST_DEF( c_ln_pi, 3fff250d048e7a1b, d0bd5f956c6a843f); // ln(pi)
BID_F80_CONST_DEF( c_385,     4007810000000000, 0000000000000000); // 385.
BID_F80_CONST_DEF( c_neg_398, c0078e0000000000, 0000000000000000); // -398
BID_F80_CONST_DEF( c_9_10ths, 3ffecccccccccccc, cccccccccccccccd); // .9
BID_F80_CONST_DEF( c_zero,    0000000000000000, 0000000000000000); // 0.0
BID_F80_CONST_DEF( c_two,     4000000000000000, 0000000000000000); // 2.0
#endif


BID_TYPE0_FUNCTION_ARGTYPE1(BID_UINT64, bid64_cos, BID_UINT64, x)

// Local variables.

  BID_UINT64 res;
  int s, e;
  BID_UINT64 c;
  BID_F80_TYPE xd, yd;
  BID_UINT192 m;
  BID_UINT256 p;
  int sf, k, ef, el;

  BID_F80_ASSIGN( yd, c_zero );

// Decompose the input and check for NaN and infinity.

  s = x >> 63;
  if ((x & (3ull<<61)) == (3ull<<61))
   { if ((x & (0xFull<<59)) == (0xFull<<59))
      { if ((x & (0x1Full<<58)) != (0x1Full<<58))
         { // input is infinite, so return NaN
           #ifdef BID_SET_STATUS_FLAGS
           __set_status_flags(pfpsf, BID_INVALID_EXCEPTION);
           #endif
           res = BID64_NAN;
           BID_RETURN (res);
         }
        else
         { // input is NaN, so quiet/canonize it etc.
           #ifdef BID_SET_STATUS_FLAGS
           if ((x & SNAN_MASK64) == SNAN_MASK64)
           __set_status_flags(pfpsf, BID_INVALID_EXCEPTION);
           #endif
           res = x & 0xfc03ffffffffffffull;
           if ((res & 0x0003ffffffffffffull) > 999999999999999ull)
           res &= ~0x0003ffffffffffffull;
           BID_RETURN(res);
         }
      }
     else
      { // "large coefficient" input
        e = ((x >> 51) & ((1ull<<10)-1)) - 398;
        c = (1ull<<53) + (x & ((1ull<<51)-1));
        if ((unsigned long long)(c) > 9999999999999999ull) c = 0ull;
      }
   }
  else
   { // "small coefficient" input
     e = ((x >> 53) & ((1ull<<10)-1)) - 398;
     c = x & ((1ull<<53)-1);
   }

// Make sure we treat zero even with huge exponent as small

  if (c == 0) e = -18;

// If the input is trivially <= 1/10, just do the naive computation
// since no range reduction is needed and the function is well-conditioned

  if (e < -17)
   { BIDECIMAL_CALL1(bid64_to_binary80,xd,x);
     __bid_f80_cos( yd, xd );
     BIDECIMAL_CALL1(binary80_to_bid64,res,yd);
     BID_RETURN(res);
   }

// Pick out the appropriate modulus for the exponent and multiply by coeff
// Since we discard the top word p.w[3], we could specially optimize this.

  m = bid_decimal64_moduli[e+17];
  __mul_64x192_to_256(p,c,m);

// Shift up by two bits to give an integer part k and a fraction
// modulo (pi/2). Note that we have to do this afterwards rather than
// use modulo (pi/2) reduction at the start to keep integer parities.

  k = p.w[2] >> 62;
  sll192_short(p.w[2],p.w[1],p.w[0],2);

// If the fraction is >= 1/2, add 1 to integer and complement the fraction
// with an appropriate sign change so we have a "rounded to nearest" version
// (Complementing is slightly different from negation but it's negligible.)
// Set "sf" to the correct sign for the fraction

  if (p.w[2] >= 0x8000000000000000ull)
   { k = (k + 1) & 3;
     p.w[2] = ~p.w[2];
     p.w[1] = ~p.w[1];
     p.w[0] = ~p.w[0];
     sf = 1 - s;
   }
  else
   { sf = s;
   }

// Also correct k to take into account the sign

  if (s) k = (-k) & 3;

// Normalize the binary fraction with exponent ef

  if (p.w[2] == 0)      // This probably can't happen but I'm not quite sure
   { ef = 16382-64;
     p.w[2] = p.w[1];
     p.w[1] = p.w[0];
   }
  else ef = 16382;

  el = clz64_nz(p.w[2]);
  ef = ef - el;
  if (el != 0) sll128_short(p.w[2],p.w[1],el);

// Now package it as a double-extended number.

  { BID_F80_CONST tmp;
    BID_F80_PACK_TRIG( tmp, sf, ef, p.w[2] );
    BID_F80_ASSIGN( xd, tmp );
  }

// Multiply by pi/2 so we can use regular binary trig functions.

 __bid_f80_mul( xd, c_pi_ov_2.v, xd);

// Now use the trig function depending on k:

 switch(k)
  { case 0: __bid_f80_cos( yd, xd ); break;
    case 1: __bid_f80_sin( yd, xd );
            __bid_f80_neg( yd, yd); break;
    case 2: __bid_f80_cos( yd, xd );
            __bid_f80_neg( yd, yd); break;
    case 3: __bid_f80_sin( yd, xd); break;
  }

  BIDECIMAL_CALL1(binary80_to_bid64,res,yd);
  BID_RETURN(res);
}
