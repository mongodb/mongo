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

static BID_UINT128 BID128_0 = {BID128_LH_INIT( 0x0000000000000000ull, 0x3040000000000000ull )};
static BID_UINT128 BID128_ZERO = {BID128_LH_INIT( 0x0000000000000000ull, 0x0000000000000000ull )};
static BID_UINT128 BID128_1 = {BID128_LH_INIT( 0x0000000000000001ull, 0x3040000000000000ull )};
static BID_UINT128 BID128_NAN = {BID128_LH_INIT( 0x0000000000000000ull, 0x7c00000000000000ull )};
static BID_UINT128 BID128_INF = {BID128_LH_INIT( 0x0000000000000000ull, 0x7800000000000000ull )};
static BID_UINT128 BID128_NEGINF = {BID128_LH_INIT( 0x0000000000000000ull, 0xF800000000000000ull )};

// log(2) and log(10) scaled by 2^160

static BID_UINT192 bid_log_2_entry =
  {{ 0x03f2f6af40f34326ull, 0xd1cf79abc9e3b398ull, 0x00000000b17217f7ull }};

static BID_UINT192 bid_log_10_entry =
  {{ 0x0b4c28a38a3fb3e7ull, 0xaaa2b05ba95b58aeull, 0x000000024d763776ull }};

// 1/2, 1/3, ..., 1/10 as binary fractions

static BID_UINT192 bid_recip_2 =
 {{ 0x0000000000000000ull, 0x0000000000000000ull, 0x8000000000000000ull }};

static BID_UINT192 bid_recip_3 =
{{ 0x5555555555555555ull, 0x5555555555555555ull, 0x5555555555555555ull }};

static BID_UINT192 bid_recip_4 =
{{ 0x0000000000000000ull, 0x0000000000000000ull, 0x4000000000000000ull }};

static BID_UINT192 bid_recip_5 =
{{ 0x3333333333333333ull, 0x3333333333333333ull, 0x3333333333333333ull }};

static BID_UINT192 bid_recip_6 =
{{ 0xaaaaaaaaaaaaaaabull, 0xaaaaaaaaaaaaaaaaull, 0x2aaaaaaaaaaaaaaaull }};

static BID_UINT192 bid_recip_7 =
{{ 0x9249249249249249ull, 0x4924924924924924ull, 0x2492492492492492ull }};

static BID_UINT192 bid_recip_8 =
{{ 0x0000000000000000ull, 0x0000000000000000ull, 0x2000000000000000ull }};

static BID_UINT192 bid_recip_9 =
{{ 0x71c71c71c71c71c7ull, 0xc71c71c71c71c71cull, 0x1c71c71c71c71c71ull }};

static BID_UINT192 bid_recip_10 =
{{ 0x999999999999999aull, 0x9999999999999999ull, 0x1999999999999999ull }};

// 10^28 * 2^96, used for final decimal alignment

static BID_UINT192 bid_decimal_multiplier_1 =
{{ 0x0000000000000000ull, 0x1000000000000000ull, 0x204fce5e3e250261ull }};

// Taylor series coefficients -1/2, 1/3, -1/4

static BID_UINT128 bid_coeff_2 =
  {BID128_LH_INIT( 0x0000000000000005ull, 0xb03e000000000000ull )};

static BID_UINT128 bid_coeff_3 =
  {BID128_LH_INIT( 0x67d9da2155555555ull, 0x2ffca45894e48295ull )};

static BID_UINT128 bid_coeff_4 =
  {BID128_LH_INIT( 0x0000000000000019ull, 0xb03c000000000000ull )};

// Reciprocal table
// These are 1 + e for the various bitfields, scaled by 2^63

static BID_UINT64 bid_recip_table_1[] =
{
  0xfe03f80fe03f80feull,
  0xfc0fc0fc0fc0fc0full,
  0xfa232cf252138abfull,
  0xf83e0f83e0f83e0full,
  0xf6603d980f6603d9ull,
  0xf4898d5f85bb3950ull,
  0xf2b9d6480f2b9d64ull,
  0xf0f0f0f0f0f0f0f0ull,
  0xef2eb71fc4345238ull,
  0xed7303b5cc0ed730ull,
  0xebbdb2a5c1619c8bull,
  0xea0ea0ea0ea0ea0eull,
  0xe865ac7b7603a196ull,
  0xe6c2b4481cd85689ull,
  0xe525982af70c880eull,
  0xe38e38e38e38e38eull,
  0xe1fc780e1fc780e1ull,
  0xe070381c0e070381ull,
  0xdee95c4ca037ba57ull,
  0xdd67c8a60dd67c8aull,
  0xdbeb61eed19c5957ull,
  0xda740da740da740dull,
  0xd901b2036406c80dull,
  0xd79435e50d79435eull,
  0xd62b80d62b80d62bull,
  0xd4c77b03531dec0dull,
  0xd3680d3680d3680dull,
  0xd20d20d20d20d20dull,
  0xd0b69fcbd2580d0bull,
  0xcf6474a8819ec8e9ull,
  0xce168a7725080ce1ull,
  0xccccccccccccccccull,
  0xcb8727c065c393e0ull,
  0xca4587e6b74f0329ull,
  0xc907da4e871146acull,
  0xc7ce0c7ce0c7ce0cull,
  0xc6980c6980c6980cull,
  0xc565c87b5f9d4d1bull,
  0xc4372f855d824ca5ull,
  0xc30c30c30c30c30cull,
  0xc1e4bbd595f6e947ull,
  0xc0c0c0c0c0c0c0c0ull,
  0xbfa02fe80bfa02feull,
  0xbe82fa0be82fa0beull,
  0xbd69104707661aa2ull,
  0xbc52640bc52640bcull,
  0xbb3ee721a54d880bull,
  0xba2e8ba2e8ba2e8bull,
  0xb92143fa36f5e02eull,
  0xb81702e05c0b8170ull,
  0xb70fbb5a19be3658ull,
  0xb60b60b60b60b60bull,
  0xb509e68a9b94821full,
  0xb40b40b40b40b40bull,
  0xb30f63528917c80bull,
  0xb21642c8590b2164ull,
  0xb11fd3b80b11fd3bull,
  0xb02c0b02c0b02c0bull,
  0xaf3addc680af3addull,
  0xae4c415c9882b931ull,
  0xad602b580ad602b5ull,
  0xac7691840ac76918ull,
  0xab8f69e28359cd11ull,
  0xaaaaaaaaaaaaaaaaull,
  0xa9c84a47a07f5637ull,
  0xa8e83f5717c0a8e8ull,
  0xa80a80a80a80a80aull,
  0xa72f05397829cbc1ull,
  0xa655c4392d7b73a7ull,
  0xa57eb50295fad40aull,
  0xa4a9cf1d96833751ull,
  0xa3d70a3d70a3d70aull,
  0xa3065e3fae7cd0e0ull,
  0xa237c32b16cfd772ull,
  0xa16b312ea8fc377cull,
  0xa0a0a0a0a0a0a0a0ull,
  0x9fd809fd809fd809ull,
  0x9f1165e7254813e2ull,
  0x9e4cad23dd5f3a20ull,
  0x9d89d89d89d89d89ull,
  0x9cc8e160c3fb19b8ull,
  0x9c09c09c09c09c09ull,
  0x9b4c6f9ef03a3ca9ull,
  0x9a90e7d95bc609a9ull,
  0x99d722dabde58f06ull,
  0x991f1a515885fb37ull,
  0x9868c809868c8098ull,
  0x97b425ed097b425eull,
  0x97012e025c04b809ull,
  0x964fda6c0964fda6ull,
  0x95a02568095a0256ull,
  0x94f2094f2094f209ull,
  0x9445809445809445ull,
  0x939a85c40939a85cull,
  0x92f113840497889cull,
  0x9249249249249249ull,
  0x91a2b3c4d5e6f809ull,
  0x90fdbc090fdbc090ull,
  0x905a38633e06c43aull,
  0x8fb823ee08fb823eull,
  0x8f1779d9fdc3a218ull,
  0x8e78356d1408e783ull,
  0x8dda520237694808ull,
  0x8d3dcb08d3dcb08dull,
  0x8ca29c046514e023ull,
  0x8c08c08c08c08c08ull,
  0x8b70344a139bc75aull,
  0x8ad8f2fba9386822ull,
  0x8a42f8705669db46ull,
  0x89ae4089ae4089aeull,
  0x891ac73ae9819b50ull,
  0x8888888888888888ull,
  0x87f78087f78087f7ull,
  0x8767ab5f34e47ef1ull,
  0x86d905447a34acc6ull,
  0x864b8a7de6d1d608ull,
  0x85bf37612cee3c9aull,
  0x8534085340853408ull,
  0x84a9f9c8084a9f9cull,
  0x8421084210842108ull,
  0x839930523fbe3367ull,
  0x83126e978d4fdf3bull,
  0x828cbfbeb9a020a3ull,
  0x8208208208208208ull,
  0x81848da8faf0d277ull,
  0x8102040810204081ull,
  0x8080808080808080ull,
  0x8000000000000000ull
};


static BID_UINT64 bid_recip_table_2[] =
{
  0x80fffbf800203ffeull,
  0x80fdf3f850df3775ull,
  0x80fbec0901971b20ull,
  0x80f9e42a1181ebb8ull,
  0x80f7dc5b7fd9b66aull,
  0x80f5d49d4bd894dbull,
  0x80f3ccef74b8ad27ull,
  0x80f1c551f9b431ddull,
  0x80efbdc4da056202ull,
  0x80edb64814e6890cull,
  0x80ebaedba991fee5ull,
  0x80e9a77f974227e8ull,
  0x80e7a033dd3174ddull,
  0x80e598f87a9a6300ull,
  0x80e391cd6eb77bf6ull,
  0x80e18ab2b8c355d6ull,
  0x80df83a857f8931full,
  0x80dd7cae4b91e2bdull,
  0x80db75c492ca0008ull,
  0x80d96eeb2cdbb2bdull,
  0x80d768221901cf06ull,
  0x80d5616956773570ull,
  0x80d35ac0e476d2f2ull,
  0x80d15428c23ba0e5ull,
  0x80cf4da0ef00a509ull,
  0x80cd47296a00f180ull,
  0x80cb40c23277a4d0ull,
  0x80c93a6b479fe9ddull,
  0x80c73424a8b4f7efull,
  0x80c52dee54f212acull,
  0x80c327c84b928a19ull,
  0x80c121b28bd1ba97ull,
  0x80bf1bad14eb0ce7ull,
  0x80bd15b7e619f621ull,
  0x80bb0fd2fe99f7bbull,
  0x80b909fe5da69f85ull,
  0x80b7043a027b87a4ull,
  0x80b4fe85ec545699ull,
  0x80b2f8e21a6cbf39ull,
  0x80b0f34e8c0080b0ull,
  0x80aeedcb404b667full,
  0x80ace85836894879ull,
  0x80aae2f56df60ac6ull,
  0x80a8dda2e5cd9ddfull,
  0x80a6d8609d4bfe8eull,
  0x80a4d32e93ad35edull,
  0x80a2ce0cc82d5965ull,
  0x80a0c8fb3a088aadull,
  0x809ec3f9e87af7c9ull,
  0x809cbf08d2c0db0aull,
  0x809aba27f8167b0cull,
  0x8098b55757b82ab2ull,
  0x8096b096f0e2492dull,
  0x8094abe6c2d141f4ull,
  0x8092a746ccc18cc4ull,
  0x8090a2b70defada3ull,
  0x808e9e37859834daull,
  0x808c99c832f7bef8ull,
  0x808a9569154af4cfull,
  0x8088911a2bce8b74ull,
  0x80868cdb75bf443bull,
  0x808488acf259ecbcull,
  0x8082848ea0db5eccull,
  0x8080808080808080ull,
  0x807e7c829086442bull,
  0x807c7894d029a85bull,
  0x807a74b73ea7b7dbull,
  0x807870e9db3d89b1ull,
  0x80766d2ca528411cull,
  0x8074697f9ba50d94ull,
  0x807265e2bdf12acaull,
  0x807062560b49e0a4ull,
  0x806e5ed982ec8340ull,
  0x806c5b6d241672f0ull,
  0x806a5810ee051c3bull,
  0x806854c4dff5f7d9ull,
  0x80665188f9268ab6ull,
  0x80644e5d38d465efull,
  0x80624b419e3d26d1ull,
  0x80604836289e76d9ull,
  0x805e453ad7360bb0ull,
  0x805c424fa941a730ull,
  0x805a3f749dff175cull,
  0x80583ca9b4ac3665ull,
  0x805639eeec86eaa5ull,
  0x8054374444cd26a1ull,
  0x805234a9bcbce905ull,
  0x8050321f53943ca5ull,
  0x804e2fa50891387eull,
  0x804c2d3adaf1ffafull,
  0x804a2ae0c9f4c17full,
  0x80482896d4d7b958ull,
  0x8046265cfad92ec5ull,
  0x804424333b377576ull,
  0x804222199530ed3aull,
  0x8040201008040201ull,
  0x803e1e1692ef2bd9ull,
  0x803c1c2d3530eef0ull,
  0x803a1a53ee07db8full,
  0x8038188abcb28e1eull,
  0x803616d1a06faf1dull,
  0x80341528987df32aull,
  0x8032138fa41c1afaull,
  0x80301206c288f35bull,
  0x802e108df3035532ull,
  0x802c0f2534ca257cull,
  0x802a0dcc871c554bull,
  0x80280c83e938e1c6ull,
  0x80260b4b5a5ed426ull,
  0x80240a22d9cd41baull,
  0x8022090a66c34be0ull,
  0x8020080200802008ull,
  0x801e0709a642f7b2ull,
  0x801c0621574b186dull,
  0x801a054912d7d3d7ull,
  0x80180480d8288799ull,
  0x801603c8a67c9d6bull,
  0x801403207d138b0dull,
  0x801202885b2cd24dull,
  0x8010020040080100ull,
  0x800e01882ae4b103ull,
  0x800c01201b02883cull,
  0x800a00c80fa13898ull,
  0x8008008008008008ull,
  0x8006004803602881ull,
  0x8004002001000800ull,
  0x8002000800200080ull,
  0x8000000000000000ull
};

// Corresponding logs of the bitfields (1 + e)
// Scaled by 2^160 so the binary point is in the middle of the top word.

static BID_UINT192 bid_log_table_1[] =
{{ { 0xe63073dc8d1016e7ull, 0x20c9011c026d235eull, 0x00000000af741551ull }},
  {{ 0x949b4bd30ae78496ull, 0xb24efd31120864fdull, 0x00000000ad7a02e1ull }},
  {{ 0xa902ef3bca1d3892ull, 0xdc633300f9e6607bull, 0x00000000ab83d135ull }},
  {{ 0x14d057006dd861daull, 0x33c2b99803a4aea6ull, 0x00000000a9917134ull }},
  {{ 0xa7688479bf28cadfull, 0xd270c9d6c3362382ull, 0x00000000a7a2d41aull }},
  {{ 0xdec6e0729cfc4174ull, 0xb860fb886f6a62a0ull, 0x00000000a5b7eb7cull }},
  {{ 0x088c0d645ff0dfe5ull, 0x45169a49fb594fabull, 0x00000000a3d0a93full }},
  {{ 0x8e597e15edabf5f3ull, 0xc91e267a0b7efae0ull, 0x00000000a1ecff97ull }},
  {{ 0x30cd12419c4a3d80ull, 0x2e5498c357879c5aull, 0x00000000a00ce109ull }},
  {{ 0xb6df81ad1ee478cbull, 0xb5fda918f0603d87ull, 0x000000009e304061ull }},
  {{ 0xd4619e62e8c9628bull, 0xcbb73a4194554b2dull, 0x000000009c5710b8ull }},
  {{ 0xea3cb5e0b3b153c1ull, 0xec642e0f3549f9aaull, 0x000000009a81456cull }},
  {{ 0x1647b357f38b04e6ull, 0xa03458b5592f8932ull, 0x0000000098aed221ull }},
  {{ 0xc94e2f14a18360f8ull, 0x86fa1646ba1188fbull, 0x0000000096dfaabdull }},
  {{ 0xa24180119a004a4aull, 0x7608369597cbc416ull, 0x000000009513c368ull }},
  {{ 0x0554e1517fa486e6ull, 0xa6dc93c19f5bb3b6ull, 0x00000000934b1089ull }},
  {{ 0xd1a4bab00f4e88a5ull, 0xf5e4bf007d92199eull, 0x00000000918586c5ull }},
  {{ 0x7e95da6695b64599ull, 0x30b2c6ddbf00bf16ull, 0x000000008fc31afeull }},
  {{ 0xce247837b0283991ull, 0x73003959a7dae1ccull, 0x000000008e03c24dull }},
  {{ 0x15fe25a52caa3c17ull, 0x91e533134e2ad194ull, 0x000000008c477207ull }},
  {{ 0xa173c82d0a3a687full, 0x94b0913374b628dbull, 0x000000008a8e1fb7ull }},
  {{ 0x07b6de2c2af1b1b1ull, 0x3ad53cdb5e3111a7ull, 0x0000000088d7c11eull }},
  {{ 0x6dbfa8f76fb9d89eull, 0x8e670a6557e005d0ull, 0x0000000087244c30ull }},
  {{ 0xc1ab80b25996cfd0ull, 0x82a7d21a821f9f89ull, 0x000000008573b716ull }},
  {{ 0x8fbb68b8860b39b3ull, 0x9e2b409086f6fafeull, 0x0000000083c5f829ull }},
  {{ 0xf7e2ea1f34421975ull, 0xb01d6773a70d58c3ull, 0x00000000821b05f3ull }},
  {{ 0x70c40109e16c8c6full, 0x903d588b47d1b09cull, 0x000000008072d72dull }},
  {{ 0x92c60a629d146f57ull, 0xe92210bf1f82c926ull, 0x000000007ecd62bdull }},
  {{ 0xd0daa6e73c846810ull, 0x0c64b378d9c052a2ull, 0x000000007d2a9fb8ull }},
  {{ 0x6a075f47c3b28981ull, 0xd04f93f9c9238128ull, 0x000000007b8a855aull }},
  {{ 0xc69f32e99837fa4dull, 0x76b5cd575446a17bull, 0x0000000079ed0b0full }},
  {{ 0x047fb218f98d58b2ull, 0x9c9b36527e3375b2ull, 0x0000000078522868ull }},
  {{ 0x99f214cbd1b63067ull, 0x325856f467c8e7a5ull, 0x0000000076b9d521ull }},
  {{ 0x06b6cbf3cccdcaa6ull, 0x7be9add7d8d3b3d4ull, 0x000000007524091bull }},
  {{ 0xf38eccb923c9bca3ull, 0x191d0d06a0270b29ull, 0x000000007390bc60ull }},
  {{ 0xda4d629cce9afed1ull, 0x155324907f56db28ull, 0x0000000071ffe71dull }},
  {{ 0x155d126aa6727766ull, 0xfe8e763fb7f470c0ull, 0x00000000707181a4ull }},
  {{ 0x43c21420e4b59368ull, 0x038bec2cfe053336ull, 0x000000006ee5846eull }},
  {{ 0x377f156217d5a735ull, 0x18a4254891610665ull, 0x000000006d5be811ull }},
  {{ 0xe867747032763e82ull, 0x2337419d16c45dd3ull, 0x000000006bd4a549ull }},
  {{ 0x20373373b04257c8ull, 0x2b678dbc6da9de97ull, 0x000000006a4fb4f2ull }},
  {{ 0x8ee6398026460b7full, 0x93e9e321bfcebcfaull, 0x0000000068cd1008ull }},
  {{ 0xc30d6b5478801390ull, 0x57b4ec304b979fa7ull, 0x00000000674cafa8ull }},
  {{ 0xe38603338cc5d374ull, 0x4d5ab73a66bf4983ull, 0x0000000065ce8d0cull }},
  {{ 0xd3af1a0c9ffb2cb0ull, 0x6fda2651a84673e0ull, 0x000000006452a18dull }},
  {{ 0xcfcf793fe0d57366ull, 0x2cb7d28e9d0c7dc7ull, 0x0000000062d8e6a2ull }},
  {{ 0xeac9714ab299694dull, 0xb72feab6a399bbc4ull, 0x00000000616155ddull }},
  {{ 0x1225e6576c261e66ull, 0x60546fb6bbf6d4cbull, 0x000000005febe8efull }},
  {{ 0x0dd9d7f5da25bcb3ull, 0xf3ecf63e337e8bbcull, 0x000000005e7899a1ull }},
  {{ 0x582cd3054f0c938eull, 0x19eec58464ee3059ull, 0x000000005d0761dbull }},
  {{ 0xa335f5b6183f100dull, 0xbc65c8586f088b61ull, 0x000000005b983b9aull }},
  {{ 0x05e19cbb9c1e9c72ull, 0x71a850690bab75d0ull, 0x000000005a2b20faull }},
  {{ 0xfcca3ce6a26bcbddull, 0xeab124ede26728ffull, 0x0000000058c00c2cull }},
  {{ 0x75d89d81427e26f3ull, 0x657cbe9a62eb7344ull, 0x000000005756f77dull }},
  {{ 0xa4e735e98bc1f4c1ull, 0x2347eb7b3597503bull, 0x0000000055efdd4full }},
  {{ 0xb43511045b123558ull, 0xe28f5f3800b263acull, 0x00000000548ab81cull }},
  {{ 0x168ae10f3a3251a3ull, 0x5cb0efbab87a93aeull, 0x0000000053278278ull }},
  {{ 0x6eeebf99342b772full, 0xc7106c18f74c14c5ull, 0x0000000051c63709ull }},
  {{ 0x9c8c6dbe569cd133ull, 0x57a31c85bb921c13ull, 0x000000005066d08full }},
  {{ 0x139d42af7aa0c172ull, 0xccc60ed52581af57ull, 0x000000004f0949dcull }},
  {{ 0xe9c95f123aa58242ull, 0xf8445bb2ae3c5df1ull, 0x000000004dad9ddaull }},
  {{ 0xc2383c1c9230e55cull, 0x4d738ec2366f61a3ull, 0x000000004c53c787ull }},
  {{ 0xf78319893222f82cull, 0x724d4e7c83280279ull, 0x000000004afbc1f3ull }},
  {{ 0x02aa70a843d24373ull, 0xd36e49dfefadd9dbull, 0x0000000049a58844ull }},
  {{ 0x304e443b00672a50ull, 0x3ae350fac548d75dull, 0x00000000485115b4ull }},
  {{ 0x6ee57ace0f2a5500ull, 0x69ae537648a3dedbull, 0x0000000046fe658dull }},
  {{ 0x9352c5cc8dc684e3ull, 0xb3edcd6637d28b40ull, 0x0000000045ad732eull }},
  {{ 0xcb7a078ed127f61eull, 0x9f91ef78562d07f1ull, 0x00000000445e3a08ull }},
  {{ 0x47b9078e5335fd06ull, 0x858b8c44911e7574ull, 0x000000004310b59dull }},
  {{ 0x1387d0f9f0b76226ull, 0x356189cd296ed4e9ull, 0x0000000041c4e181ull }},
  {{ 0x9a3ceafa276bc5a5ull, 0x9b1a43dce8de85adull, 0x00000000407ab958ull }},
  {{ 0x050c6d83a0276e3eull, 0x6766f2fad28337ccull, 0x000000003f3238d9ull }},
  {{ 0xdd7e5a6c1fdb41c1ull, 0xb9ffcbbd953488e3ull, 0x000000003deb5bc9ull }},
  {{ 0x608325248af34d37ull, 0xce20242436c083e8ull, 0x000000003ca61dffull }},
  {{ 0xb2e20c5dc5072b02ull, 0xa91280692c7527e5ull, 0x000000003b627b61ull }},
  {{ 0x8d10f80f708af640ull, 0xcabcf6af31492123ull, 0x000000003a206fe4ull }},
  {{ 0xdada1e05f743145dull, 0xe01ee1373da69d42ull, 0x0000000038dff78dull }},
  {{ 0x16d742aa95451881ull, 0x77b15a1d8b55f6a5ull, 0x0000000037a10e70ull }},
  {{ 0xb596fba6a1c37918ull, 0xb79c794e162a63caull, 0x000000003663b0aeull }},
  {{ 0x901b99b8ea622be4ull, 0x15b3c6dcf7d4ef4bull, 0x000000003527da79ull }},
  {{ 0xcfde7059c049ab10ull, 0x112cc8252432c0bcull, 0x0000000033ed880eull }},
  {{ 0xe8f42fd9b008540eull, 0xee02fe43cf141fedull, 0x0000000032b4b5b9ull }},
  {{ 0xe900afcb09640480ull, 0x71fd185400a2da65ull, 0x00000000317d5fd6ull }},
  {{ 0xc3f4c240f447b6daull, 0xa3478376ce98c7a0ull, 0x00000000304782caull }},
  {{ 0xc805a90e24626144ull, 0x8898e67bdfdbaf3eull, 0x000000002f131b0aull }},
  {{ 0x4c9d43f79ad3f115ull, 0xead577390131ef0full, 0x000000002de02516ull }},
  {{ 0xe412be9dc55fe900ull, 0x182673e21b0f0b9dull, 0x000000002cae9d7dull }},
  {{ 0x040c5b4a18838733ull, 0xa87b63f5a525d9f9ull, 0x000000002b7e80d6ull }},
  {{ 0x520152b7a0832ecbull, 0x436b19f3b4b4bee3ull, 0x000000002a4fcbc9ull }},
  {{ 0x3b2d046d0aec4996ull, 0x676ac1bb627edb3cull, 0x0000000029227b06ull }},
  {{ 0x7d4d5460189545e6ull, 0x32519712e4cae559ull, 0x0000000027f68b4bull }},
  {{ 0x12b2a1c1e2a033f2ull, 0x2b202c5ec84696e5ull, 0x0000000026cbf960ull }},
  {{ 0x1a822120a8d8deb0ull, 0x0d0273acbb703694ull, 0x0000000025a2c219ull }},
  {{ 0x14b59f9eaf893c64ull, 0x9384034873f4f7d7ull, 0x00000000247ae254ull }},
  {{ 0x21032f2feeafc974ull, 0x47ee53c6ea1c4c9aull, 0x00000000235456fcull }},
  {{ 0xe5bd03c76ea3fb0full, 0x4fc8f7bc271683f8ull, 0x00000000222f1d04ull }},
  {{ 0x066e5825f4b6b1feull, 0x3c740d1119fb37eaull, 0x00000000210b316bull }},
  {{ 0xb1b2523bfc137707ull, 0xdbd56593182f7a81ull, 0x000000001fe89139ull }},
  {{ 0xe581ee6749cad473ull, 0x0a111fca840cdd0full, 0x000000001ec73983ull }},
  {{ 0xc062faaab5f5d01dull, 0x8446a24e77e9c5ccull, 0x000000001da72763ull }},
  {{ 0x9c1799a1b453ed90ull, 0xbc4b2367d32d5669ull, 0x000000001c885801ull }},
  {{ 0xb4c1cc6e6c244ae4ull, 0xad5b1bdf590225c6ull, 0x000000001b6ac88dull }},
  {{ 0xf69a64178f2919c2ull, 0xb1bc37a798d77f06ull, 0x000000001a4e7640ull }},
  {{ 0xcd2508971d032ff3ull, 0x594988adad5ea3ecull, 0x0000000019335e5dull }},
  {{ 0xed428f997c2e0ec4ull, 0x40e3f01b552dffbeull, 0x0000000018197e2full }},
  {{ 0x917d845b31136fa4ull, 0xeac0e0f30d4cef69ull, 0x000000001700d30aull }},
  {{ 0x1429fe196852d6feull, 0x9791cb7c1dd17171ull, 0x0000000015e95a4dull }},
  {{ 0x0b2f674c4d5b793full, 0x207eac5c57d0b1e1ull, 0x0000000014d3115dull }},
  {{ 0x68bed941469189cdull, 0xd1ee642eeeeda76bull, 0x0000000013bdf5a7ull }},
  {{ 0x70238b2e873caddcull, 0x4717a48b30b1cb41ull, 0x0000000012aa04a4ull }},
  {{ 0xa6148a2a2200085bull, 0x465566d0b4f930b2ull, 0x0000000011973bd1ull }},
  {{ 0x03372c127c6c58ffull, 0x9e3a0687a3fd9bf5ull, 0x00000000108598b5ull }},
  {{ 0x93278a93171c8028ull, 0x035c3dd74406d890ull, 0x000000000f7518e0ull }},
  {{ 0x2058d6004ad0f9a6ull, 0xeed965c31209f5feull, 0x000000000e65b9e6ull }},
  {{ 0x056e45ed4faccaf3ull, 0x7d887e0cfe9dda17ull, 0x000000000d577968ull }},
  {{ 0xa23cc5408981b14eull, 0x4fd9a199cbe97660ull, 0x000000000c4a550aull }},
  {{ 0xcc06c2f862da0baaull, 0x6a5dac1f467cca0bull, 0x000000000b3e4a79ull }},
  {{ 0xd904dc965179ff1dull, 0x16f1f4c5a521016bull, 0x000000000a33576aull }},
  {{ 0x4dd423bc8ee5b88eull, 0xc68c1f4c7810db3dull, 0x0000000009297997ull }},
  {{ 0x6c444ef0506133bcull, 0xf3a222378b9e3aeaull, 0x000000000820aec4ull }},
  {{ 0x42798e198e5a93b5ull, 0x052abc617dcf5979ull, 0x000000000718f4bbull }},
  {{ 0x059928ed9fb983caull, 0x3232afa222d2f9e6ull, 0x000000000612494aull }},
  {{ 0xbe6da572cf969071ull, 0x66033026d450c6ffull, 0x00000000050caa49ull }},
  {{ 0xe71eee69b5553ecfull, 0x24d611d23c8e8416ull, 0x0000000004081596ull }},
  {{ 0xc26800aca0931098ull, 0x7114554348c584dfull, 0x0000000003048914ull }},
  {{ 0xf3b401e916faf842ull, 0xb11bce251598b505ull, 0x00000000020202aeull }},
  {{ 0x8d9db379a9250bccull, 0x9588b356e598e33dull, 0x0000000001008055ull }},
  {{ 0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull }}
};

static BID_UINT192 bid_log_table_2[] =
{{ { 0x72fe3e8d2a020d93ull, 0xb126788d20bbe98eull, 0x0000000001fdfaa6ull }},
  {{ 0xd10f58ffc08c8951ull, 0xf1701f78d37ed9aaull, 0x0000000001f9f2aeull }},
  {{ 0x6172d205bb2f8b61ull, 0x7175c001e25b2bcdull, 0x0000000001f5eac7ull }},
  {{ 0x04939d9b99f60c13ull, 0x30b45d7e491b02c9ull, 0x0000000001f1e2f0ull }},
  {{ 0xf68243407a5c91f7ull, 0x2ea9016c0535b7f5ull, 0x0000000001eddb29ull }},
  {{ 0xe359660c41a45a7cull, 0x6ad0bb7a94f46f9full, 0x0000000001e9d372ull }},
  {{ 0x61ec2c1014a74ae8ull, 0xe4a8a18a8a4686ffull, 0x0000000001e5cbcbull }},
  {{ 0x0a55cdfa48fa35bfull, 0x9badcfa728addb01ull, 0x0000000001e1c435ull }},
  {{ 0x72eac6dfa53cd862ull, 0x8f5d680de86ae71cull, 0x0000000001ddbcafull }},
  {{ 0x72f2a8aa5dd72519ull, 0xbf34932617c0bb7dull, 0x0000000001d9b539ull }},
  {{ 0x1f75851bb3c02a44ull, 0x2ab07f885e10c9c2ull, 0x0000000001d5add4ull }},
  {{ 0x0b492500d549c660ull, 0xd14e61fa554e8790ull, 0x0000000001d1a67eull }},
  {{ 0x6868f04dbec04b53ull, 0xb28b75682682e643ull, 0x0000000001cd9f39ull }},
  {{ 0xbd7f72756a551b1full, 0xcde4faf3fe679efaull, 0x0000000001c99804ull }},
  {{ 0xf761ce28cb1f49baull, 0x22d839e1c2ea5241ull, 0x0000000001c590e0ull }},
  {{ 0xb3143cdfb3903ac7ull, 0xb0e27faa80a77a9full, 0x0000000001c189cbull }},
  {{ 0xb1c2dee0d1548f92ull, 0x77811fea1c053147ull, 0x0000000001bd82c7ull }},
  {{ 0x7cebab42362b725bull, 0x7631746acce5c435ull, 0x0000000001b97bd3ull }},
  {{ 0x55c53a351a985556ull, 0xac70dd24afc21cf2ull, 0x0000000001b574efull }},
  {{ 0x9fba713fec263f27ull, 0x19bcc0316adbf749ull, 0x0000000001b16e1cull }},
  {{ 0x0a9cc842dc4db7a3ull, 0xbd9289dd9fffe72full, 0x0000000001ad6758ull }},
  {{ 0xd5f5ef682208558full, 0x976fac969b6f2d19ull, 0x0000000001a960a5ull }},
  {{ 0x9ba612e7a4a62b66ull, 0xa6d1a0f9c9295818ull, 0x0000000001a55a02ull }},
  {{ 0x24bdd0b7a19d24e3ull, 0xeb35e5c65c4db4e1ull, 0x0000000001a1536full }},
  {{ 0xe0422faffacaf120ull, 0x6419ffe8cb2c891full, 0x00000000019d4cedull }},
  {{ 0xa845866d624aac96ull, 0x10fb7a726c301a4aull, 0x000000000199467bull }},
  {{ 0x967a43c213bc5e66ull, 0xf157e69efbc57f3aull, 0x0000000001954018ull }},
  {{ 0xbe1bf24ab9273ce4ull, 0x04acdbca3bad3bd0ull, 0x00000000019139c7ull }},
  {{ 0xb5bf9dfe1b9d3380ull, 0x4a77f77d6c13a5e9ull, 0x00000000018d3385ull }},
  {{ 0xf04ef4996e941430ull, 0xc236dd64ea9112e3ull, 0x0000000001892d53ull }},
  {{ 0xf921123f7c075f09ull, 0x6b673753bad9ccfcull, 0x0000000001852732ull }},
  {{ 0xbbd0e9b40a862568ull, 0x4586b53f1ba5cfc9ull, 0x0000000001812121ull }},
  {{ 0x151b9c888f00fdbaull, 0x50130d4806484b0eull, 0x00000000017d1b20ull }},
  {{ 0xfeb8e2f2a438b4a9ull, 0x8a89fbaad53eeb37ull, 0x000000000179152full }},
  {{ 0xbcc4d64889e6d1eeull, 0xf46942ceba98e6c2ull, 0x0000000001750f4eull }},
  {{ 0x87f50b27f9bf4a84ull, 0x8d2eab3f5755cfc6ull, 0x000000000171097eull }},
  {{ 0x4472eaf46c7fedfdull, 0x545803a454e428f9ull, 0x00000000016d03beull }},
  {{ 0xe9d2a7437b53ae7dull, 0x496320d0dbf7bd64ull, 0x000000000168fe0eull }},
  {{ 0x5538f4cd84538f66ull, 0x6bcdddb5366fba18ull, 0x000000000164f86eull }},
  {{ 0x5359f88d5125505cull, 0xbb161b6651848917ull, 0x000000000160f2deull }},
  {{ 0xc4917742397580e3ull, 0x36b9c11b4e256cceull, 0x00000000015ced5full }},
  {{ 0xd1e7678118175537ull, 0xde36bc2913addb51ull, 0x000000000158e7efull }},
  {{ 0x3e6480701f925de6ull, 0xb10b0009d22298a5ull, 0x000000000154e290ull }},
  {{ 0xf4a8438df76df5bbull, 0xaeb4865697668f5dull, 0x000000000150dd41ull }},
  {{ 0x053d4f76b259dcbdull, 0xd6b14ec8d95766d8ull, 0x00000000014cd802ull }},
  {{ 0x5eb1840e53ab9ac1ull, 0x287f5f3a0281d64bull, 0x000000000148d2d4ull }},
  {{ 0x9cfda6c0f52c5915ull, 0xa39cc3a2feadb403ull, 0x000000000144cdb5ull }},
  {{ 0x624bc7be087a8c15ull, 0x47878e1bc741c000ull, 0x000000000140c8a7ull }},
  {{ 0xbfaca7a5d00c5a4eull, 0x13bdd6daef7f2943ull, 0x00000000013cc3a9ull }},
  {{ 0x48cac86ea7b5553full, 0x07bdbc372e24cd14ull, 0x000000000138bebbull }},
  {{ 0x8225ac962da2313full, 0x230562a4ebea2f78ull, 0x000000000134b9ddull }},
  {{ 0x6dd90d92765d54abull, 0x6512f4afd6622c31ull, 0x000000000130b50full }},
  {{ 0x0f6a8627972ffe09ull, 0xcd64a30a585d5f7aull, 0x00000000012cb051ull }},
  {{ 0xd38c4ecb13f5175full, 0x5b78a47f365445d1ull, 0x000000000128aba4ull }},
  {{ 0xdd353ae7d7de1145ull, 0x0ecd35f317191217ull, 0x000000000124a707ull }},
  {{ 0x4ddd24e2e245cc83ull, 0xe6e09a6c05d1393dull, 0x000000000120a279ull }},
  {{ 0xb31965279fd8ef2cull, 0xe3311b07087eb2d4ull, 0x00000000011c9dfcull }},
  {{ 0xd83eced7bf537999ull, 0x033d06ffa1c0edc4ull, 0x0000000001189990ull }},
  {{ 0x4f15fa9536ee72c0ull, 0x4682b3ab601d7865ull, 0x0000000001149533ull }},
  {{ 0x171366734612a9b7ull, 0xac807c7b66605b4bull, 0x00000000011090e6ull }},
  {{ 0xdee72126da0f36aeull, 0x34b4c2f5fc64260bull, 0x00000000010c8caaull }},
  {{ 0x709756e7debe794full, 0xde9deec20c91ad3eull, 0x000000000108887dull }},
  {{ 0xecb627d755399c55ull, 0xa9ba6d9abab778feull, 0x0000000001048461ull }},
  {{ 0x8d9db379a9250bccull, 0x9588b356e598e33dull, 0x0000000001008055ull }},
  {{ 0xbff53a2f0fa84d57ull, 0xa18739e6b2ece51eull, 0x0000000000fc7c59ull }},
  {{ 0x72179dc36c8d9d06ull, 0xcd34814f1cf492b3ull, 0x0000000000f8786dull }},
  {{ 0x9046652d7be50b85ull, 0x180f0faf76014450ull, 0x0000000000f47492ull }},
  {{ 0xb7e4b5b0f57c0b29ull, 0x8195713ef3e26cccull, 0x0000000000f070c6ull }},
  {{ 0x40437575f443563aull, 0x0946384a3afb1bebull, 0x0000000000ec6d0bull }},
  {{ 0xcbd3015f0e95ce08ull, 0xae9ffd34e55f2c3bull, 0x0000000000e8695full }},
  {{ 0xa8d789f43d76ac25ull, 0x71215e7b09f81bb5ull, 0x0000000000e465c4ull }},
  {{ 0x5d0349f15814d0cdull, 0x504900aacab18e56ull, 0x0000000000e06239ull }},
  {{ 0xcb9d5cd39ce72611ull, 0x4b958e6bd5e57a0aull, 0x0000000000dc5ebeull }},
  {{ 0x7a1d04d21207e8b5ull, 0x6285b878f42ffb2aull, 0x0000000000d85b53ull }},
  {{ 0x8b5faea8825823ceull, 0x949835a38d42d0ccull, 0x0000000000d457f8ull }},
  {{ 0x1ddbf67f0b96cb4full, 0xe14bc2cd3510803eull, 0x0000000000d054adull }},
  {{ 0xcd6d5cdf0acd61a8ull, 0x481f22ef2d171ee1ull, 0x0000000000cc5173ull }},
  {{ 0x2d8b3ca925df83f4ull, 0xc8911f15efa2c1c0ull, 0x0000000000c84e48ull }},
  {{ 0x24f1ecb495eb1645ull, 0x62208660b7779211ull, 0x0000000000c44b2eull }},
  {{ 0x27f6c8a1221ae7e9ull, 0x144c2e0305d38606ull, 0x0000000000c04824ull }},
  {{ 0x62ef25475f73fa45ull, 0xde92f13e2e57bd1dull, 0x0000000000bc4529ull }},
  {{ 0xfa3ef675acd26d22ull, 0xc073b16cd5497f45ull, 0x0000000000b8423full }},
  {{ 0x99ce252ea0caacc4ull, 0xb96d55f48112de1dull, 0x0000000000b43f65ull }},
  {{ 0xa1cb48a0dc2053aaull, 0xc8fecc51186af78full, 0x0000000000b03c9bull }},
  {{ 0x52c79fcf96942b77ull, 0xeea7080c6ee5d91eull, 0x0000000000ac39e1ull }},
  {{ 0x6f5acfd4697689f3ull, 0x29e502c3c76c031eull, 0x0000000000a83738ull }},
  {{ 0xdd9d0a338c021284ull, 0x7a37bc215ec98b2dull, 0x0000000000a4349eull }},
  {{ 0xe6e3d90895ae9033ull, 0xdf1e39dfef44dd2dull, 0x0000000000a03214ull }},
  {{ 0xc848e394b16e2df0ull, 0x581787ce346d1a09ull, 0x00000000009c2f9bull }},
  {{ 0x5a9a8da6a14b641eull, 0xe4a2b7c278a01392ull, 0x0000000000982d31ull }},
  {{ 0xac6c600b35601bf2ull, 0x843ee1a8123fe4b7ull, 0x0000000000942ad8ull }},
  {{ 0x7d10af0414062f9bull, 0x366b2376ee702569ull, 0x000000000090288full }},
  {{ 0x9b55f789c001b21bull, 0xfaa6a13117a2b967ull, 0x00000000008c2655ull }},
  {{ 0x3ef0ebeabfb14621ull, 0xd07084ec356c394bull, 0x000000000088242cull }},
  {{ 0x87882500b8a908c8ull, 0xb747fec7176ff512ull, 0x0000000000842213ull }},
  {{ 0x605fe77f29ee7d82ull, 0xaeac44ef37338f77ull, 0x000000000080200aull }},
  {{ 0x1bab643f22de0952ull, 0xb61c939c3f32315bull, 0x00000000007c1e11ull }},
  {{ 0x2c8d50170d5157a1ull, 0xcd182d158cb75490ull, 0x0000000000781c28ull }},
  {{ 0x7ad3a38da93d4a4cull, 0xf31e59a9b879254bull, 0x0000000000741a4full }},
  {{ 0xdf79c540233a69b2ull, 0x27ae67b8166a7986ull, 0x0000000000701887ull }},
  {{ 0x6cf9532e517c2b73ull, 0x6a47aba43f9c5d9eull, 0x00000000006c16ceull }},
  {{ 0x3a6c301993a32058ull, 0xba697fe390473572ull, 0x0000000000681525ull }},
  {{ 0x7c7a6a6090b70a57ull, 0x179344f0b1237156ull, 0x000000000064138dull }},
  {{ 0xcb04029eac835c05ull, 0x8144615118b9d61bull, 0x0000000000601204ull }},
  {{ 0x8669890a8a1e364cull, 0xf6fc41928ffb5779ull, 0x00000000005c108bull }},
  {{ 0x6346fa6788380cb6ull, 0x783a584eb4708424ull, 0x0000000000580f23ull }},
  {{ 0x3762381d7cb42211ull, 0x047e1e267d4082dbull, 0x0000000000540dcbull }},
  {{ 0x3679eb5e7383af73ull, 0x9b4711c3bdcf9fb4ull, 0x0000000000500c82ull }},
  {{ 0xe18a9945d1997d35ull, 0x3c14b7d4aa2d68f4ull, 0x00000000004c0b4aull }},
  {{ 0xff0626418f01282dull, 0xe6669b15570a5abeull, 0x0000000000480a21ull }},
  {{ 0x065e01819d3dac0aull, 0x99bc4c43404d18ddull, 0x0000000000440909ull }},
  {{ 0x7d21af41c15e526full, 0x55956224c95f35f8ull, 0x0000000000400801ull }},
  {{ 0xd7d2691f0e98398bull, 0x19717986c0698784ull, 0x00000000003c0709ull }},
  {{ 0x8469105720533f8dull, 0xe4d0353be0c805a9ull, 0x0000000000380620ull }},
  {{ 0xd865b6010e103082ull, 0xb7313e1e54ed3675ull, 0x0000000000340548ull }},
  {{ 0xb01789dc8c0b4938ull, 0x9014430939cd23a9ull, 0x0000000000300480ull }},
  {{ 0xa0a1118ee52234cbull, 0x6ef8f8e21ecfda5dull, 0x00000000002c03c8ull }},
  {{ 0xb20f202afb997aaaull, 0x535f1a8c89fb73d5ull, 0x0000000000280320ull }},
  {{ 0xaaa92179ee907a16ull, 0x3cc668f7772da6c9ull, 0x0000000000240288ull }},
  {{ 0x086eed5a712b85c0ull, 0x2aaeab10dbbce06eull, 0x0000000000200200ull }},
  {{ 0xd9837ee4d512d887ull, 0x1c97adc92710e488ull, 0x00000000001c0188ull }},
  {{ 0xb80c98472e9031c0ull, 0x12014416c372f3ddull, 0x0000000000180120ull }},
  {{ 0x41d59088bb97b73full, 0x0a6b46f3978d783cull, 0x00000000001400c8ull }},
  {{ 0x77c7438dd251003full, 0x0555955887b3357cull, 0x0000000000100080ull }},
  {{ 0x85085f6da7c841d9ull, 0x0240143ef655feb4ull, 0x00000000000c0048ull }},
  {{ 0x81581464acb2f9baull, 0x00aaaeaa4444eef3ull, 0x0000000000080020ull }},
  {{ 0xd5f17f16631fcc03ull, 0x00155595522224ccull, 0x0000000000040008ull }},
  {{ 0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull }}
};

// Some internal macros.

#define __add_128_64(R128, A128, B64)    \
{                                        \
BID_UINT64 R64H;                             \
        R64H = (A128).w[1];                 \
        (R128).w[0] = (B64) + (A128).w[0];     \
        if((R128).w[0] < (B64))               \
          R64H ++;                           \
    (R128).w[1] = R64H;                  \
}

#define __mul_64x64_to_128(P, CX, CY)   \
{                                       \
BID_UINT64 CXH, CXL, CYH,CYL,PL,PH,PM,PM2;\
        CXH = (CX) >> 32;                     \
        CXL = (BID_UINT32)(CX);                   \
        CYH = (CY) >> 32;                     \
        CYL = (BID_UINT32)(CY);                   \
        PM = CXH*CYL;                         \
        PH = CXH*CYH;                         \
        PL = CXL*CYL;                         \
        PM2 = CXL*CYH;                        \
        PH += (PM>>32);                       \
        PM = (BID_UINT64)((BID_UINT32)PM)+PM2+(PL>>32); \
        (P).w[1] = PH + (PM>>32);             \
        (P).w[0] = (PM<<32)+(BID_UINT32)PL;       \
}

#define __mul_64x64_to_64_hi(P, CX, CY)      \
{                                            \
BID_UINT64 CXH, CXL, CYH,CYL,PL,PH,PM,PM2;        \
        CXH = (CX) >> 32;                     \
        CXL = (BID_UINT32)(CX);                   \
        CYH = (CY) >> 32;                     \
        CYL = (BID_UINT32)(CY);                   \
        PM = CXH*CYL;                         \
        PH = CXH*CYH;                         \
        PL = CXL*CYL;                         \
        PM2 = CXL*CYH;                        \
        PH += (PM>>32);                       \
        PM = (BID_UINT64)((BID_UINT32)PM)+PM2+(PL>>32); \
        (P) = PH + (PM>>32);             \
}

#define __mul_64x128_to_192(Q, A, B)              \
{                                                 \
BID_UINT128 ALBL, ALBH, QM2;                          \
                                                  \
        __mul_64x64_to_128(ALBH, (A), (B).w[1]);      \
        __mul_64x64_to_128(ALBL, (A), (B).w[0]);      \
                                                  \
        (Q).w[0] = ALBL.w[0];                         \
    __add_128_64(QM2, ALBH, ALBL.w[1]);           \
        (Q).w[1] = QM2.w[0];                          \
    (Q).w[2] = QM2.w[1];                          \
}

#define __add_carry_out(S, CY, X, Y)    \
{                                      \
BID_UINT64 X1=X;                           \
        S = X + Y;                         \
        CY = (S<X1) ? 1 : 0;                \
}

#define __add_carry_in_out(S, CY, X, Y, CI)    \
{                                             \
BID_UINT64 X1;                                    \
        X1 = X + CI;                              \
        S = X1 + Y;                               \
        CY = ((S<X1) || (X1<CI)) ? 1 : 0;          \
}

#define __sub_borrow_out(S, CY, X, Y)    \
{                                      \
BID_UINT64 X1=X;                           \
        S = X - Y;                         \
        CY = (S>X1) ? 1 : 0;                \
}

#define __sub_borrow_in_out(S, CY, X, Y, CI)    \
{                                             \
BID_UINT64 X1, X0=X;                              \
        X1 = X - CI;                              \
        S = X1 - Y;                               \
        CY = ((S>X1) || (X1>X0)) ? 1 : 0;          \
}

#define __mul_64x192_to_256(lP, lA, lB)                      \
{                                                         \
BID_UINT128 lP0,lP1,lP2;                                      \
BID_UINT64 lC;                                                 \
        __mul_64x64_to_128(lP0, lA, (lB).w[0]);              \
        __mul_64x64_to_128(lP1, lA, (lB).w[1]);              \
        __mul_64x64_to_128(lP2, lA, (lB).w[2]);              \
        (lP).w[0] = lP0.w[0];                                \
        __add_carry_out((lP).w[1],lC,lP1.w[0],lP0.w[1]);      \
        __add_carry_in_out((lP).w[2],lC,lP2.w[0],lP1.w[1],lC); \
        (lP).w[3] = lP2.w[1] + lC;                           \
}

// Case of guaranteed no overflow.

#define __mul_64x192_to_192(lP, lA, lB)                      \
{                                                         \
BID_UINT128 lP0,lP1,lP2;                                      \
BID_UINT64 lC;                                                 \
        __mul_64x64_to_128(lP0, lA, (lB).w[0]);              \
        __mul_64x64_to_128(lP1, lA, (lB).w[1]);              \
        __mul_64x64_to_128(lP2, lA, (lB).w[2]);              \
        (lP).w[0] = lP0.w[0];                                \
        __add_carry_out((lP).w[1],lC,lP1.w[0],lP0.w[1]);      \
        __add_carry_in_out((lP).w[2],lC,lP2.w[0],lP1.w[1],lC); \
}

#define __add_192_192(S,X,Y)                            \
 { BID_UINT64 S0, S1, S2;                                   \
   int CA0, CA1, CA2;                                      \
   __add_carry_out(S0,CA0,(X).w[0],(Y).w[0]);            \
   __add_carry_in_out(S1,CA1,(X).w[1],(Y).w[1],CA0);      \
   __add_carry_in_out(S2,CA2,(X).w[2],(Y).w[2],CA1);      \
   (S).w[0] = S0; (S).w[1] = S1; (S).w[2] = S2;         \
 }

#define __sub_192_192(S,X,Y)                            \
 { BID_UINT64 S0, S1, S2;                                   \
   int B0, B1, B2;                                      \
   __sub_borrow_out(S0,B0,(X).w[0],(Y).w[0]);            \
   __sub_borrow_in_out(S1,B1,(X).w[1],(Y).w[1],B0);      \
   __sub_borrow_in_out(S2,B2,(X).w[2],(Y).w[2],B1);      \
   (S).w[0] = S0; (S).w[1] = S1; (S).w[2] = S2;         \
 }

#define __mul_192x192_to_192_hi(P, A, B)                       \
{                                                              \
BID_UINT256 P0,P1,P2;                                              \
BID_UINT64 CY, PL0, PL1, PL2;                                       \
        __mul_64x192_to_256(P0, (A).w[0], B);                   \
        __mul_64x192_to_256(P1, (A).w[1], B);                   \
        __mul_64x192_to_256(P2, (A).w[2], B);                   \
        PL0 = P0.w[0];                                     \
        __add_carry_out(PL1,CY,P1.w[0],P0.w[1]);      \
        __add_carry_in_out(PL2,CY,P1.w[1],P0.w[2],CY); \
        __add_carry_in_out((P).w[0],CY,P1.w[2],P0.w[3],CY); \
        (P).w[1] = P1.w[3] + CY;                              \
        __add_carry_out(PL2,CY,P2.w[0],PL2);     \
        __add_carry_in_out((P).w[0],CY,P2.w[1],(P).w[0],CY);   \
        __add_carry_in_out((P).w[1],CY,P2.w[2],(P).w[1],CY);   \
        (P).w[2] = P2.w[3] + CY;                              \
}

// Useful macros lifted from "bid_binarydecimal.c"

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

#define clz64(n) (((n)==0) ? 64 : clz64_nz(n))

#define clz128(n_hi,n_lo) (((n_hi) == 0) ? 64 + clz64(n_lo) : clz64_nz(n_hi))

#define sll128_short(hi,lo,c)                                   \
  ((hi) = ((hi) << (c)) + ((lo)>>(64-(c))),                     \
   (lo) = (lo) << (c)                                           \
  )

#define sll128(hi,lo,c)                                         \
  (((c) == 0) ? hi = hi, lo = lo :                              \
  (((c) >= 64) ? hi = lo << ((c) - 64), lo = 0 : sll128_short(hi,lo,c)))

#define lt128(x_hi,x_lo,y_hi,y_lo)                              \
  (((x_hi) < (y_hi)) || (((x_hi) == (y_hi)) && ((x_lo) < (y_lo))))

#define sll192_short(hi,med,lo,c)                               \
  ((hi) = ((hi) << (c)) + ((med)>>(64-(c))),                    \
   (med) = ((med) << (c)) + ((lo)>>(64-(c))),                   \
   (lo) = (lo) << (c)                                           \
  )

#define srl192(x2,x1,x0,c)                                      \
  ((x0) = ((x1) << (64 - (c))) + ((x0) >> (c)),                 \
   (x1) = ((x2) << (64 - (c))) + ((x1) >> (c)),                 \
   (x2) = ((x2) >> c)                                           \
  )

// Accurate decimal128 log function returning 2-part result.
// This is mainly required for the power function, but since it's more
// "direct" it may turn out more efficient than the "naive" one above.

// ***************************************************************************

// bid128_mul stands for bid128qq_mul
BID128_FUNCTION_ARG2 (bid128_pow, x, y)

  BID_UINT128 res = {{ 0xbaddbaddbaddbaddull, 0xbaddbaddbaddbaddull }};
  BID_UINT64 x_sign, y_sign, p_sign;
  BID_UINT64 x_exp, y_exp, p_exp;
  int true_p_exp;
  BID_UINT128 C1, C2, y_int;
  BID_F128_TYPE xq, yq, rq;
  int s, is_odd, tmp_res;
  BID_UINT128 l, l_hi, l_lo, l_neg;
  int is_nan, is_zero, is_signed, cmp_res, is_int;

// We will always signal on signalling NaNs anyway

#ifdef BID_SET_STATUS_FLAGS
  if (((x.w[BID_HIGH_128W] & SNAN_MASK64) == SNAN_MASK64) ||
      ((y.w[BID_HIGH_128W] & SNAN_MASK64) == SNAN_MASK64))
   {
     __set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
   }
#endif

// We have 1^y = x^+0 = x^-0 = 1 even when x or y is a NaN

  BIDECIMAL_CALL1_NORND_NOSTAT(bid128_isZero,cmp_res,y);
  if (cmp_res && ((x.w[BID_HIGH_128W] & SNAN_MASK64) != SNAN_MASK64))
   { res = BID128_1;
     BID_RETURN(res);
   }

  BIDECIMAL_CALL2_NORND(bid128_quiet_equal,cmp_res,x,BID128_1);
  if (cmp_res && ((x.w[BID_HIGH_128W] & SNAN_MASK64) != SNAN_MASK64))
   { res = BID128_1;
     BID_RETURN(res);
   }

// Otherwise a NaN input leads to a NaN result.
// Just return the same NaN, quieted and canonized

  if ((x.w[BID_HIGH_128W] & NAN_MASK64) == NAN_MASK64)
   { res.w[BID_HIGH_128W] = x.w[BID_HIGH_128W] & 0xfc003fffffffffffull;
     res.w[BID_LOW_128W] = x.w[BID_LOW_128W];
     if (((res.w[BID_HIGH_128W] & 0x00003fffffffffffull) >
          0x0000314dc6448d93ull) ||
         (((res.w[BID_HIGH_128W] & 0x00003fffffffffffull) ==
            0x0000314dc6448d93ull) &&
          res.w[BID_LOW_128W] >= 0x38c15b0a00000000ull))
      { res.w[BID_HIGH_128W] &= ~0x00003fffffffffffull;
        res.w[BID_LOW_128W] = 0ull;
      }
     BID_RETURN(res);
   }
  else if ((y.w[BID_HIGH_128W] & NAN_MASK64) == NAN_MASK64)
   { res.w[BID_HIGH_128W] = y.w[BID_HIGH_128W] & 0xfc003fffffffffffull;
     res.w[BID_LOW_128W] = y.w[BID_LOW_128W];
     if (((res.w[BID_HIGH_128W] & 0x00003fffffffffffull) >
          0x0000314dc6448d93ull) ||
         (((res.w[BID_HIGH_128W] & 0x00003fffffffffffull) ==
            0x0000314dc6448d93ull) &&
          res.w[BID_LOW_128W] >= 0x38c15b0a00000000ull))
      { res.w[BID_HIGH_128W] &= ~0x00003fffffffffffull;
        res.w[BID_LOW_128W] = 0ull;
      }
     BID_RETURN(res);
   }

// Deal with other cases where second arg is infinite:
//
//  pow(-1,+-inf) = 1
//  pow(x,+inf) = +inf when |x| > 1
//  pow(x,+inf) = +0 when |x| < 1
//  pow(x,-inf) = +0 when |x| > 1
//  pow(x,-inf) = +inf when |x| < 1

  BIDECIMAL_CALL1_NORND_NOSTAT(bid128_isInf,cmp_res,y);
  if (cmp_res)
   { BID_UINT128 a = x;
     a.w[BID_HIGH_128W] &= ~SIGNMASK64;
     BIDECIMAL_CALL2_NORND(bid128_quiet_equal,cmp_res,a,BID128_1);
     if (cmp_res)
      { res = BID128_1;
        BID_RETURN(res);
      }
     BIDECIMAL_CALL2_NORND(bid128_quiet_less,cmp_res,a,BID128_1);
     if (cmp_res)
        if ((y.w[BID_HIGH_128W] & SIGNMASK64) != 0) res = BID128_INF;
        else res = BID128_0;
     else
        if ((y.w[BID_HIGH_128W] & SIGNMASK64) != 0) res = BID128_0;
        else res = BID128_INF;
     BID_RETURN(res);
   }

// See if the exponent is an integer, and if so, find its parity.
// We can assume that bid128_round_integral_nearest_even returns a
// result with exponent >= 0, and if it's > 0 it's trivially even.

  BIDECIMAL_CALL1_NORND(bid128_round_integral_nearest_even, y_int, y);
  BIDECIMAL_CALL2_NORND(bid128_quiet_equal,is_int,y_int,y);
  is_odd = 0;

  if (is_int)
   { int e = ((y_int.w[BID_HIGH_128W] >> 49) & ((1ull<<14)-1));
     if ((e == 6176) && (y_int.w[BID_LOW_128W] & 1)) is_odd = 1;
   }

// Now the cases where the first arg is infinite:
//
//  pow(+inf,y) = 0 for y < 0
//  pow(+inf,y) = +inf for y > 0
//  and pow(-inf,y) the same with sign swapped for odd integers

  BIDECIMAL_CALL1_NORND_NOSTAT(bid128_isInf,cmp_res,x);
  if (cmp_res)
   { if ((y.w[BID_HIGH_128W] & SIGNMASK64) != 0) res = BID128_0;
     else res = BID128_INF;
     if (is_odd && ((x.w[BID_HIGH_128W] & SIGNMASK64) != 0))
        res.w[BID_HIGH_128W] ^= SIGNMASK64;
     BID_RETURN(res);
   }

// Now cases where first argument is 0, where we return +0 or +inf,
// or -0 or -inf if the second argument is an odd integer.

  BIDECIMAL_CALL1_NORND_NOSTAT(bid128_isZero,cmp_res,x);
  if (cmp_res)
   { if ((y.w[BID_HIGH_128W] & SIGNMASK64) != 0) {
        res = BID128_INF;
        __set_status_flags (pfpsf, BID_ZERO_DIVIDE_EXCEPTION);
     } else res = BID128_0;
     if (is_odd && ((x.w[BID_HIGH_128W] & SIGNMASK64) != 0))
        res.w[BID_HIGH_128W] ^= SIGNMASK64;
     BID_RETURN(res);
   }

// Return NaN for negative^noninteger

  if (((x.w[BID_HIGH_128W] & SIGNMASK64) != 0) && !is_int)
   {
     #ifdef BID_SET_STATUS_FLAGS
     __set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
     #endif
     BID_RETURN(BID128_NAN);
   }

// Finally, we can assume all arguments are finite and nonzero.
// So launch into the naive computation. But because we can be
// more discriminating about integer status prior to conversion,
// separate out the sign and correct it later.

// Compute accurate logarithm (l_hi,l_lo)

{ int e, k, b, z, s_log;
  BID_UINT64 r1, r2, c_lo;
  BID_UINT128 xa;
  BID_UINT128 c;
  BID_UINT192 p;
  BID_UINT256 q;

  BID_UINT192 ans;
  BID_UINT192 loc;

  BID_UINT192 xx, xp, cxp, sx;

// Get absolute value

  xa = x;
  xa.w[BID_HIGH_128W] &= ~SIGNMASK64;

// Unpack the number, but check canonicality or plain zero
// and return -inf in that case.

  e = ((xa.w[BID_HIGH_128W] >> 49) & ((1ull<<14)-1)) - 6176;
  c.w[1] = xa.w[BID_HIGH_128W] & ((1ull<<49)-1);
  c.w[0] = xa.w[BID_LOW_128W];
  k = clz128(c.w[1],c.w[0]);
  sll128(c.w[1],c.w[0],k);
  k = 128 - k;

// Start out our result as e * log(10) + k * log(2)
// Note that k is always positive, but e may have either sign

  __mul_64x192_to_192(ans,(BID_UINT64) k,bid_log_2_entry);
  if (e >= 0)
   { __mul_64x192_to_192(loc,(BID_UINT64) e,bid_log_10_entry);
     __add_192_192(ans,ans,loc);
   }
  else
   { __mul_64x192_to_192(loc,(BID_UINT64)(-e),bid_log_10_entry);
     __sub_192_192(ans,ans,loc);
   }

// Pick out toplevel bitfield and find its approximate reciprocal
// After this multiplication the result (considered as a fraction)
// is 1/2 * (1 - e) where 0 <= e <= 2^-7

  b = c.w[1] >> 56;
  r1 = bid_recip_table_1[b-128];
  __mul_64x128_to_192(p,r1,c);
  __sub_192_192(ans,ans,bid_log_table_1[b-128]);

// Now the next stage in this bipartite arrangment.
// After this the result (considered as a fraction) is
// 1/4 * (1 - e) where 0 <= e < 2^-12 (maybe 2^-13, I should check)

  b = (p.w[2] >> 49) & 0x7F;
  r2 = bid_recip_table_2[b];
  __mul_64x192_to_256(q,r2,p);
  __sub_192_192(ans,ans,bid_log_table_2[b]);

// Complement and shift back by 2 bits to get a proper binary fraction

  sll192_short(q.w[3],q.w[2],q.w[1],2);
  q.w[3] = ~q.w[3], q.w[2] = ~q.w[2], q.w[1] = ~q.w[1];

// Now compute the power series
// Should use Remez and maybe something shorter?

  sx.w[2] = xx.w[2] = q.w[3];
  sx.w[1] = xx.w[1] = q.w[2];
  sx.w[0] = xx.w[0] = q.w[1];

  __mul_192x192_to_192_hi(xp,xx,xx);
  __mul_192x192_to_192_hi(cxp,xp,bid_recip_2);
  __add_192_192(sx,sx,cxp);

  __mul_192x192_to_192_hi(xp,xp,xx);
  __mul_192x192_to_192_hi(cxp,xp,bid_recip_3);
  __add_192_192(sx,sx,cxp);

  __mul_192x192_to_192_hi(xp,xp,xx);
  __mul_192x192_to_192_hi(cxp,xp,bid_recip_4);
  __add_192_192(sx,sx,cxp);

  __mul_192x192_to_192_hi(xp,xp,xx);
  __mul_192x192_to_192_hi(cxp,xp,bid_recip_5);
  __add_192_192(sx,sx,cxp);

  __mul_192x192_to_192_hi(xp,xp,xx);
  __mul_192x192_to_192_hi(cxp,xp,bid_recip_6);
  __add_192_192(sx,sx,cxp);

  __mul_192x192_to_192_hi(xp,xp,xx);
  __mul_192x192_to_192_hi(cxp,xp,bid_recip_7);
  __add_192_192(sx,sx,cxp);

  __mul_192x192_to_192_hi(xp,xp,xx);
  __mul_192x192_to_192_hi(cxp,xp,bid_recip_8);
  __add_192_192(sx,sx,cxp);

  __mul_192x192_to_192_hi(xp,xp,xx);
  __mul_192x192_to_192_hi(cxp,xp,bid_recip_9);
  __add_192_192(sx,sx,cxp);

  __mul_192x192_to_192_hi(xp,xp,xx);
  __mul_192x192_to_192_hi(cxp,xp,bid_recip_10);
  __add_192_192(sx,sx,cxp);

// Now shift it right by 32 bits and add to rest

  srl192(sx.w[2],sx.w[1],sx.w[0],32);
  __sub_192_192(ans,ans,sx);

// Figure out sign and negate as needed (actually complement, which
// only makes a difference of 2^-192).

  if (ans.w[2] & (1ull<<63))
   { s_log = 1;
     ans.w[2] = ~ans.w[2];
     ans.w[1] = ~ans.w[1];
     ans.w[0] = ~ans.w[0];
   }
  else s_log = 0;

// Now turn into decimal: the top two 64-bit words are the coefficient
// of a number with exponent 10^-28

  __mul_192x192_to_192_hi(ans,ans,bid_decimal_multiplier_1);

// And the trailing part is for a number with exponent 10^-47
// (which is nowhere near normalized, btw, as this is just one word).

  __mul_64x64_to_64_hi(c_lo,10000000000000000000ull,ans.w[0]);

// We need about 127 bits of accuracy in this logarithm, and we have
// about 158 bits of fraction (being pessimistic). So if the 31 leading
// bits of the fraction (and the integer part) are all zero, we ought
// to do something different, simply returning t=|x|-1 in the high part and
// the next three terms of the Taylor series otherwise.
// (i.e. -t^2/2 + t^3/3 - t^4/4). Again, it'd be better to use a Remez series

  if (ans.w[2] < 2)
    { BID_UINT128 t, tp, tn, ts;

      BIDECIMAL_CALL2(bid128_sub,t,xa,BID128_1);
      BIDECIMAL_CALL2(bid128_mul,tp,t,t);
      BIDECIMAL_CALL2(bid128_mul,ts,bid_coeff_2,tp);
      BIDECIMAL_CALL2(bid128_mul,tp,t,tp);
      BIDECIMAL_CALL2(bid128_mul,tn,bid_coeff_3,tp);
      BIDECIMAL_CALL2(bid128_add,ts,ts,tn);
      BIDECIMAL_CALL2(bid128_mul,tp,t,tp);
      BIDECIMAL_CALL2(bid128_mul,tn,bid_coeff_4,tp);
      BIDECIMAL_CALL2(bid128_add,ts,ts,tn);

      BIDECIMAL_CALL2(bid128_add,l_hi,t,ts);
      BIDECIMAL_CALL2(bid128_sub,l_lo,l_hi,t);
      BIDECIMAL_CALL2(bid128_sub,l_lo,l_lo,ts);
    }

// Otherwise just package up our results as decimal numbers.
// Note that the sign "s_log" is needed in both of them.

  else
   { l_hi.w[BID_HIGH_128W] = ((BID_UINT64) (s_log) << 63) + (6148ull << 49) + ans.w[2];
     l_hi.w[BID_LOW_128W] = ans.w[1];
     l_lo.w[BID_HIGH_128W] = ((BID_UINT64) (s_log) << 63) + (6129ull << 49);
     l_lo.w[BID_LOW_128W] = c_lo;
   }
}

  BIDECIMAL_CALL2(bid128_mul,l,y,l_hi);
  l_neg = l; l_neg.w[BID_HIGH_128W] ^= MASK_SIGN;
  BIDECIMAL_CALL3(bid128_fma,l_hi,y,l_hi,l_neg);
  BIDECIMAL_CALL3(bid128_fma,l_lo,y,l_lo,l_hi);

// Compute dominant exponential term
// We really want exp(l + l_lo)

  BIDECIMAL_CALL1(bid128_exp,res,l);

// If this is zero or infinity, then stop now; do some flag settings
// and make zero results have an exponent that communicates inexactness

  BIDECIMAL_CALL1_NORND_NOSTAT(bid128_isZero,cmp_res,res);
  if (cmp_res)
   { *pfpsf |= BID_UNDERFLOW_EXCEPTION;
     res = BID128_ZERO;
     BID_RETURN(res);
   }

  BIDECIMAL_CALL1_NORND_NOSTAT(bid128_isInf,cmp_res,res);
  if (cmp_res)
   { *pfpsf |= BID_OVERFLOW_EXCEPTION;
     BID_RETURN(res);
   }

// Otherwise correct using l_lo

  BIDECIMAL_CALL3(bid128_fma,res,res,l_lo,res);

// Finally, fix up the sign

  if (is_odd && ((x.w[BID_HIGH_128W] & SIGNMASK64) != 0))
     res.w[BID_HIGH_128W] ^= MASK_SIGN;

  BID_RETURN(res);
}
