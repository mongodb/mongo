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

double sin(double);
double cos(double);

#define BID32_1   0x32800001ul

#define BID32_NAN 0x7c000000ul

// Values of (10^a / 2 pi) mod 1 for -8 <= a <= 90
// Each one is a 128-bit binary fraction.
// Maybe it would be just about OK to use 64-bit fractions?

static BID_UINT128 bid_decimal32_moduli[] =
{ {{ 0xd1ec52e455229a49ull, 0x00000006d5ed56c8ull }},
  {{ 0x333b3ceb535a06d8ull, 0x000000445b4563d8ull }},
  {{ 0x0050613141844470ull, 0x000002ab90b5e672ull }},
  {{ 0x0323cbec8f2aac65ull, 0x00001ab3a71b0074ull }},
  {{ 0x1f65f73d97aabbefull, 0x00010b04870e0488ull }},
  {{ 0x39fba867ecab575aull, 0x000a6e2d468c2d51ull }},
  {{ 0x43d4940f3eb16984ull, 0x00684dc4c179c52cull }},
  {{ 0xa64dc89872ee1f25ull, 0x041309af8ec1b3baull }},
  {{ 0x7f09d5f47d4d3770ull, 0x28be60db9391054aull }},
  {{ 0xf6625b8ce5042a62ull, 0x976fc893c3aa34e8ull }},
  {{ 0x9fd79380f229a7d5ull, 0xea5dd5c5a4a61119ull }},
  {{ 0x3e6bc30975a08e56ull, 0x27aa59b86e7cab00ull }},
  {{ 0x70359e5e98458f5eull, 0x8ca7813450deae02ull }},
  {{ 0x62182fb1f2b799b0ull, 0x7e8b0c0b28b2cc18ull }},
  {{ 0xd4f1dcf37b2c00e3ull, 0xf16e786f96fbf8f3ull }},
  {{ 0x5172a182cfb808e1ull, 0x6e50b45be5d7b986ull }},
  {{ 0x2e7a4f1c1d3058c6ull, 0x4f270b96fa6d3f3full }},
  {{ 0xd0c7171923e377b9ull, 0x178673e5c8447877ull }},
  {{ 0x27c6e6fb66e2ad3aull, 0xeb4086f9d2acb4aeull }},
  {{ 0x8dc505d204dac446ull, 0x308545c23abf0ecdull }},
  {{ 0x89b23a34308baac1ull, 0xe534b9964b769407ull }},
  {{ 0x60f64609e574ab85ull, 0xf40f3fdef2a1c84bull }},
  {{ 0xc99ebc62f68eb334ull, 0x88987eb57a51d2f1ull }},
  {{ 0xe0335bdda193000cull, 0x55f4f316c7323d71ull }},
  {{ 0xc20196a84fbe0075ull, 0x5b917ee3c7f66672ull }},
  {{ 0x940fe2931d6c0490ull, 0x93aef4e5cfa0007bull }},
  {{ 0xc89ed9bf26382da2ull, 0xc4d590fa1c4004d3ull }},
  {{ 0xd63481777e31c854ull, 0xb057a9c51a803045ull }},
  {{ 0x5e0d0eaaedf1d34cull, 0xe36ca1b30901e2baull }},
  {{ 0xac8292ad4b7240f5ull, 0xe23e50fe5a12db47ull }},
  {{ 0xbd19bac4f276898full, 0xd66f29ef84bc90ccull }},
  {{ 0x63014bb178a15f9bull, 0x6057a35b2f5da7ffull }},
  {{ 0xde0cf4eeb64dbc0bull, 0xc36c618fd9a88ff9ull }},
  {{ 0xac8191531f095870ull, 0xa23bcf9e80959fc2ull }},
  {{ 0xbd0fad3f365d7461ull, 0x56561c3105d83d9aull }},
  {{ 0x629cc4781fa68bcdull, 0x5f5d19ea3a72680bull }},
  {{ 0xda1facb13c817602ull, 0xb9a3032648781071ull }},
  {{ 0x853cbeec5d0e9c19ull, 0x405e1f7ed4b0a472ull }},
  {{ 0x345f753ba29218f7ull, 0x83ad3af44ee66c79ull }},
  {{ 0x0bba945459b4f9a8ull, 0x24c44d8b15003cbcull }},
  {{ 0x7549cb4b8111c093ull, 0x6fab076ed2025f58ull }},
  {{ 0x94e1f0f30ab185baull, 0x5cae4a543417b974ull }},
  {{ 0xd0d3697e6aef3943ull, 0x9ecee74a08ed3e8dull }},
  {{ 0x28421ef02d583ca2ull, 0x341508e45944718aull }},
  {{ 0x92953561c5725e56ull, 0x08d258eb7cac6f65ull }},
  {{ 0xb9d415d1b677af57ull, 0x58377932debc59f7ull }},
  {{ 0x4248da3120acd968ull, 0x722abbfcb35b83adull }},
  {{ 0x96d885eb46c07e11ull, 0x75ab57df019324c4ull }},
  {{ 0xe4753b30c384eca7ull, 0x98b16eb60fbf6fadull }},
  {{ 0xec944fe7a3313e81ull, 0xf6ee531c9d7a5ccaull }},
  {{ 0x3dcb1f0c5fec710eull, 0xa54f3f1e26c79fedull }},
  {{ 0x69ef367bbf3c6a88ull, 0x7518772d83cc3f44ull }},
  {{ 0x235820d5785c2951ull, 0x92f4a7c725fa78acull }},
  {{ 0x61714856b3999d26ull, 0xbd8e8dc77bc8b6b9ull }},
  {{ 0xce6cd3630400237eull, 0x679189cad5d7233dull }},
  {{ 0x104041de280162ecull, 0x0baf61ec5a67606aull }},
  {{ 0xa28292ad900ddd37ull, 0x74d9d33b8809c424ull }},
  {{ 0x5919bac7a08aa429ull, 0x908240535061a96eull }},
  {{ 0x7b014bcc456a699cull, 0xa516834123d09e4full }},
  {{ 0xce0cf5fab6282016ull, 0x72e1208b66262f1aull }},
  {{ 0x0c819bcb1d9140ddull, 0x7ccb4571fd7dd70cull }},
  {{ 0x7d1015ef27ac88a1ull, 0xdff0b673e6ea6678ull }},
  {{ 0xe2a0db578cbd5648ull, 0xbf672087052800b4ull }},
  {{ 0xda48916b7f655ecfull, 0x7a07454633900710ull }},
  {{ 0x86d5ae32f9f5b41bull, 0xc448b4be03a046a8ull }},
  {{ 0x4458cdfdc399090dull, 0xaad70f6c2442c295ull }},
  {{ 0xab780be9a3fa5a80ull, 0xac669a396a9b99d4ull }},
  {{ 0xb2b0772067c78903ull, 0xbc02063e2a14024eull }},
  {{ 0xfae4a7440dcb5a19ull, 0x58143e6da4c81712ull }},
  {{ 0xccee88a889f184fdull, 0x70ca70486fd0e6bdull }},
  {{ 0x01515695636f31e6ull, 0x67e862d45e29036aull }},
  {{ 0x0d2d61d5e257f300ull, 0x0f13dc4bad9a2224ull }},
  {{ 0x83c5d25ad76f7dffull, 0x96c69af4c8055568ull }},
  {{ 0x25ba378c6a5aebfaull, 0xe3c20d8fd0355615ull }},
  {{ 0x79462b7c278d37c5ull, 0xe594879e22155cd3ull }},
  {{ 0xbcbdb2d98b842db5ull, 0xf7cd4c2d54d5a042ull }},
  {{ 0x5f68fc7f7329c90eull, 0xae04f9c55058429bull }},
  {{ 0xba19dcfa7fa1da8cull, 0xcc31c1b523729a11ull }},
  {{ 0x4502a1c8fc52897bull, 0xf9f19113627a04b1ull }},
  {{ 0xb21a51d9db395ed1ull, 0xc36faac1d8c42eecull }},
  {{ 0xf5073282903db429ull, 0xa25cab9277a9d53eull }},
  {{ 0x9247f919a2690997ull, 0x579eb3b8aca25475ull }},
  {{ 0xb6cfbb00581a5fe4ull, 0x6c330536be574c97ull }},
  {{ 0x241d4e037107beeaull, 0x39fe34236f68fdedull }},
  {{ 0x69250c226a4d7526ull, 0x43ee09625a19eb43ull }},
  {{ 0x1b7279582706937cull, 0xa74c5dd7850330a2ull }},
  {{ 0x1278bd718641c2d4ull, 0x88fbaa6b321fe655ull }},
  {{ 0xb8b7666f3e919c45ull, 0x59d4a82ff53eff52ull }},
  {{ 0x372a005871b01ab6ull, 0x824e91df9475f93bull }},
  {{ 0x27a4037470e10b1eull, 0x1711b2bbcc9bbc50ull }},
  {{ 0x8c68228c68ca6f2full, 0xe6b0fb55fe155b21ull }},
  {{ 0x7c11597c17e857d2ull, 0x02e9d15becd58f4full }},
  {{ 0xd8ad7ed8ef136e34ull, 0x1d222d974057991aull }},
  {{ 0x76c6f47956c24e0aull, 0x2355c7e8836bfb0cull }},
  {{ 0xa3c58cbd63970c5full, 0x6159cf152237ce7cull }},
  {{ 0x65b77f65e3e67bb7ull, 0xcd8216d3562e10deull }},
  {{ 0xf92af9fae700d527ull, 0x0714e4415dcca8afull }},
  {{ 0xbbadc3cd06085386ull, 0x46d0ea8da9fe96dfull }},
  {{ 0x54c9a6023c53433bull, 0xc4292988a3f1e4bdull }}
};

BID_TYPE0_FUNCTION_ARGTYPE1(BID_UINT32, bid32_cos, BID_UINT32, x)
// Local variables.

  BID_UINT32 res;
  int s, e;
  BID_UINT64 c;
  double xd, yd = 0.0;
  BID_UINT128 m;
  BID_UINT192 p;
  int sf, k, ef, el;

// Decompose the input and check for NaN and infinity.

  s = x >> 31;
  if ((x & (3ul<<29)) == (3ul<<29))
   { if ((x & (0xFul<<27)) == (0xFul<<27))
      { if ((x & (0x1Ful<<26)) != (0x1Full<<26))
         { // input is infinite, so return NaN
           #ifdef BID_SET_STATUS_FLAGS
           __set_status_flags(pfpsf, BID_INVALID_EXCEPTION);
           #endif
           res = BID32_NAN;
           BID_RETURN (res);
         }
        else
         { // input is NaN, so quiet/canonize it etc.
           #ifdef BID_SET_STATUS_FLAGS
           if ((x & SNAN_MASK32) == SNAN_MASK32)
           __set_status_flags(pfpsf, BID_INVALID_EXCEPTION);
           #endif
           res = x & 0xfc0ffffful;
           if ((res & 0x000ffffful) > 999999ul)
           res &= ~0x000ffffful;
           BID_RETURN(res);
         }
      }
     else
      { // "large coefficient" input
        e = ((x >> 21) & ((1ul<<8)-1)) - 101;
        c = (1ul<<23) + (x & ((1ul<<21)-1));
        if ((unsigned long)(c) > 9999999ul) c = 0ull;
      }
   }
  else
   { // "small coefficient" input
     e = ((x >> 23) & ((1ul<<8)-1)) - 101;
     c = x & ((1ul<<23)-1);
   }

// Make sure we treat zero even with huge exponent as small

  if (c == 0) e = -9;

// If the input is trivially <= 1/10, just do the naive computation
// since no range reduction is needed and the function is well-conditioned

  if (e < -8)
   { BIDECIMAL_CALL1(bid32_to_binary64,xd,x);
     yd = cos(xd);
     BIDECIMAL_CALL1(binary64_to_bid32,res,yd);
     BID_RETURN(res);
   }

// Pick out the appropriate modulus for the exponent and multiply by coeff
// Since we discard the top word p.w[3], we could specially optimize this.

  m = bid_decimal32_moduli[e+8];
  __mul_64x128_to_192(p,c,m);

// Shift up by two bits to give an integer part k and a fraction
// modulo (pi/2). Note that we have to do this afterwards rather than
// use modulo (pi/2) reduction at the start to keep integer parities.

  k = p.w[1] >> 62;
  sll128_short(p.w[1],p.w[0],2);

// If the fraction is >= 1/2, add 1 to integer and complement the fraction
// with an appropriate sign change so we have a "rounded to nearest" version
// (Complementing is slightly different from negation but it's negligible.)
// Set "sf" to the correct sign for the fraction

  if (p.w[1] >= 0x8000000000000000ull)
   { k = (k + 1) & 3;
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

  el = clz64_nz(p.w[1]);
  ef = 1022 - el;
  if (el != 0) sll128_short(p.w[1],p.w[0],el);

// Now shift right and mask off integer bit for double coefficient
// and package up as a double-precision number

  { union { double d; BID_UINT64 i; } di;
    di.i = (((BID_UINT64) sf) << 63) + ((BID_UINT64) ef << 52) +
           ((p.w[1] >> 11) & ((1ull<<52)-1));

    xd = di.d;
  }

// Multiply by pi/2 so we can use regular binary trig functions.

 xd = 1.570796326794896619231321691639751442098584699687552910487472296 * xd;

// Now use the trig function depending on k:

 switch(k)
  { case 0: yd = cos(xd); break;
    case 1: yd = -sin(xd); break;
    case 2: yd = -cos(xd); break;
    case 3: yd = sin(xd); break;
  }

  BIDECIMAL_CALL1(binary64_to_bid32,res,yd);
  BID_RETURN(res);
}
