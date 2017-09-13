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

// the first entry of bid_nr_digits[i - 1] (where 1 <= i <= 113), indicates 
// the number of decimal digits needed to represent a binary number with i bits;
// however, if a binary number of i bits may require either k or k + 1 decimal
// digits, then the first entry of bid_nr_digits[i - 1] is 0; in this case if the
// number is less than the value represented by the second and third entries
// concatenated, then the number of decimal digits k is the fourth entry, else
// the number of decimal digits is the fourth entry plus 1
DEC_DIGITS bid_nr_digits[] = {	// only the first entry is used if it is not 0
  {1, 0x0000000000000000ULL, 0x000000000000000aULL, 1}
  ,	//   1-bit n < 10^1
  {1, 0x0000000000000000ULL, 0x000000000000000aULL, 1}
  ,	//   2-bit n < 10^1
  {1, 0x0000000000000000ULL, 0x000000000000000aULL, 1}
  ,	//   3-bit n < 10^1
  {0, 0x0000000000000000ULL, 0x000000000000000aULL, 1}
  ,	//   4-bit n ? 10^1
  {2, 0x0000000000000000ULL, 0x0000000000000064ULL, 2}
  ,	//   5-bit n < 10^2
  {2, 0x0000000000000000ULL, 0x0000000000000064ULL, 2}
  ,	//   6-bit n < 10^2
  {0, 0x0000000000000000ULL, 0x0000000000000064ULL, 2}
  ,	//   7-bit n ? 10^2
  {3, 0x0000000000000000ULL, 0x00000000000003e8ULL, 3}
  ,	//   8-bit n < 10^3
  {3, 0x0000000000000000ULL, 0x00000000000003e8ULL, 3}
  ,	//   9-bit n < 10^3
  {0, 0x0000000000000000ULL, 0x00000000000003e8ULL, 3}
  ,	//  10-bit n ? 10^3
  {4, 0x0000000000000000ULL, 0x0000000000002710ULL, 4}
  ,	//  11-bit n < 10^4
  {4, 0x0000000000000000ULL, 0x0000000000002710ULL, 4}
  ,	//  12-bit n < 10^4
  {4, 0x0000000000000000ULL, 0x0000000000002710ULL, 4}
  ,	//  13-bit n < 10^4
  {0, 0x0000000000000000ULL, 0x0000000000002710ULL, 4}
  ,	//  14-bit n ? 10^4
  {5, 0x0000000000000000ULL, 0x00000000000186a0ULL, 5}
  ,	//  15-bit n < 10^5
  {5, 0x0000000000000000ULL, 0x00000000000186a0ULL, 5}
  ,	//  16-bit n < 10^5
  {0, 0x0000000000000000ULL, 0x00000000000186a0ULL, 5}
  ,	//  17-bit n ? 10^5
  {6, 0x0000000000000000ULL, 0x00000000000f4240ULL, 6}
  ,	//  18-bit n < 10^6
  {6, 0x0000000000000000ULL, 0x00000000000f4240ULL, 6}
  ,	//  19-bit n < 10^6
  {0, 0x0000000000000000ULL, 0x00000000000f4240ULL, 6}
  ,	//  20-bit n ? 10^6
  {7, 0x0000000000000000ULL, 0x0000000000989680ULL, 7}
  ,	//  21-bit n < 10^7
  {7, 0x0000000000000000ULL, 0x0000000000989680ULL, 7}
  ,	//  22-bit n < 10^7
  {7, 0x0000000000000000ULL, 0x0000000000989680ULL, 7}
  ,	//  23-bit n < 10^7
  {0, 0x0000000000000000ULL, 0x0000000000989680ULL, 7}
  ,	//  24-bit n ? 10^7
  {8, 0x0000000000000000ULL, 0x0000000005f5e100ULL, 8}
  ,	//  25-bit n < 10^8
  {8, 0x0000000000000000ULL, 0x0000000005f5e100ULL, 8}
  ,	//  26-bit n < 10^8
  {0, 0x0000000000000000ULL, 0x0000000005f5e100ULL, 8}
  ,	//  27-bit n ? 10^8
  {9, 0x0000000000000000ULL, 0x000000003b9aca00ULL, 9}
  ,	//  28-bit n < 10^9
  {9, 0x0000000000000000ULL, 0x000000003b9aca00ULL, 9}
  ,	//  29-bit n < 10^9
  {0, 0x0000000000000000ULL, 0x000000003b9aca00ULL, 9}
  ,	//  30-bit n ? 10^9
  {10, 0x0000000000000000ULL, 0x00000002540be400ULL, 10}
  ,	//  31-bit n < 10^10
  {10, 0x0000000000000000ULL, 0x00000002540be400ULL, 10}
  ,	//  32-bit n < 10^10
  {10, 0x0000000000000000ULL, 0x00000002540be400ULL, 10}
  ,	//  33-bit n < 10^10
  {0, 0x0000000000000000ULL, 0x00000002540be400ULL, 10}
  ,	//  34-bit n ? 10^10
  {11, 0x0000000000000000ULL, 0x000000174876e800ULL, 11}
  ,	//  35-bit n < 10^11
  {11, 0x0000000000000000ULL, 0x000000174876e800ULL, 11}
  ,	//  36-bit n < 10^11
  {0, 0x0000000000000000ULL, 0x000000174876e800ULL, 11}
  ,	//  37-bit n ? 10^11
  {12, 0x0000000000000000ULL, 0x000000e8d4a51000ULL, 12}
  ,	//  38-bit n < 10^12
  {12, 0x0000000000000000ULL, 0x000000e8d4a51000ULL, 12}
  ,	//  39-bit n < 10^12
  {0, 0x0000000000000000ULL, 0x000000e8d4a51000ULL, 12}
  ,	//  40-bit n ? 10^12
  {13, 0x0000000000000000ULL, 0x000009184e72a000ULL, 13}
  ,	//  41-bit n < 10^13
  {13, 0x0000000000000000ULL, 0x000009184e72a000ULL, 13}
  ,	//  42-bit n < 10^13
  {13, 0x0000000000000000ULL, 0x000009184e72a000ULL, 13}
  ,	//  43-bit n < 10^13
  {0, 0x0000000000000000ULL, 0x000009184e72a000ULL, 13}
  ,	//  44-bit n ? 10^13
  {14, 0x0000000000000000ULL, 0x00005af3107a4000ULL, 14}
  ,	//  45-bit n < 10^14
  {14, 0x0000000000000000ULL, 0x00005af3107a4000ULL, 14}
  ,	//  46-bit n < 10^14
  {0, 0x0000000000000000ULL, 0x00005af3107a4000ULL, 14}
  ,	//  47-bit n ? 10^14
  {15, 0x0000000000000000ULL, 0x00038d7ea4c68000ULL, 15}
  ,	//  48-bit n < 10^15
  {15, 0x0000000000000000ULL, 0x00038d7ea4c68000ULL, 15}
  ,	//  49-bit n < 10^15
  {0, 0x0000000000000000ULL, 0x00038d7ea4c68000ULL, 15}
  ,	//  50-bit n ? 10^15
  {16, 0x0000000000000000ULL, 0x002386f26fc10000ULL, 16}
  ,	//  51-bit n < 10^16
  {16, 0x0000000000000000ULL, 0x002386f26fc10000ULL, 16}
  ,	//  52-bit n < 10^16
  {16, 0x0000000000000000ULL, 0x002386f26fc10000ULL, 16}
  ,	//  53-bit n < 10^16
  {0, 0x0000000000000000ULL, 0x002386f26fc10000ULL, 16}
  ,	//  54-bit n ? 10^16
  {17, 0x0000000000000000ULL, 0x016345785d8a0000ULL, 17}
  ,	//  55-bit n < 10^17
  {17, 0x0000000000000000ULL, 0x016345785d8a0000ULL, 17}
  ,	//  56-bit n < 10^17
  {0, 0x0000000000000000ULL, 0x016345785d8a0000ULL, 17}
  ,	//  57-bit n ? 10^17
  {18, 0x0000000000000000ULL, 0x0de0b6b3a7640000ULL, 18}
  ,	//  58-bit n < 10^18
  {18, 0x0000000000000000ULL, 0x0de0b6b3a7640000ULL, 18}
  ,	//  59-bit n < 10^18
  {0, 0x0000000000000000ULL, 0x0de0b6b3a7640000ULL, 18}
  ,	//  60-bit n ? 10^18
  {19, 0x0000000000000000ULL, 0x8ac7230489e80000ULL, 19}
  ,	//  61-bit n < 10^19
  {19, 0x0000000000000000ULL, 0x8ac7230489e80000ULL, 19}
  ,	//  62-bit n < 10^19
  {19, 0x0000000000000000ULL, 0x8ac7230489e80000ULL, 19}
  ,	//  63-bit n < 10^19
  {0, 0x0000000000000000ULL, 0x8ac7230489e80000ULL, 19}
  ,	//  64-bit n ? 10^19
  {20, 0x0000000000000005ULL, 0x6bc75e2d63100000ULL, 20}
  ,	//  65-bit n < 10^20
  {20, 0x0000000000000005ULL, 0x6bc75e2d63100000ULL, 20}
  ,	//  66-bit n < 10^20
  {0, 0x0000000000000005ULL, 0x6bc75e2d63100000ULL, 20}
  ,	//  67-bit n ? 10^20
  {21, 0x0000000000000036ULL, 0x35c9adc5dea00000ULL, 21}
  ,	//  68-bit n < 10^21
  {21, 0x0000000000000036ULL, 0x35c9adc5dea00000ULL, 21}
  ,	//  69-bit n < 10^21
  {0, 0x0000000000000036ULL, 0x35c9adc5dea00000ULL, 21}
  ,	//  70-bit n ? 10^21
  {22, 0x000000000000021eULL, 0x19e0c9bab2400000ULL, 22}
  ,	//  71-bit n < 10^22
  {22, 0x000000000000021eULL, 0x19e0c9bab2400000ULL, 22}
  ,	//  72-bit n < 10^22
  {22, 0x000000000000021eULL, 0x19e0c9bab2400000ULL, 22}
  ,	//  73-bit n < 10^22
  {0, 0x000000000000021eULL, 0x19e0c9bab2400000ULL, 22}
  ,	//  74-bit n ? 10^22
  {23, 0x000000000000152dULL, 0x02c7e14af6800000ULL, 23}
  ,	//  75-bit n < 10^23
  {23, 0x000000000000152dULL, 0x02c7e14af6800000ULL, 23}
  ,	//  76-bit n < 10^23
  {0, 0x000000000000152dULL, 0x02c7e14af6800000ULL, 23}
  ,	//  77-bit n ? 10^23
  {24, 0x000000000000d3c2ULL, 0x1bcecceda1000000ULL, 24}
  ,	//  78-bit n < 10^24
  {24, 0x000000000000d3c2ULL, 0x1bcecceda1000000ULL, 24}
  ,	//  79-bit n < 10^24
  {0, 0x000000000000d3c2ULL, 0x1bcecceda1000000ULL, 24}
  ,	//  80-bit n ? 10^24
  {25, 0x0000000000084595ULL, 0x161401484a000000ULL, 25}
  ,	//  81-bit n < 10^25
  {25, 0x0000000000084595ULL, 0x161401484a000000ULL, 25}
  ,	//  82-bit n < 10^25
  {25, 0x0000000000084595ULL, 0x161401484a000000ULL, 25}
  ,	//  83-bit n < 10^25
  {0, 0x0000000000084595ULL, 0x161401484a000000ULL, 25}
  ,	//  84-bit n ? 10^25
  {26, 0x000000000052b7d2ULL, 0xdcc80cd2e4000000ULL, 26}
  ,	//  85-bit n < 10^26
  {26, 0x000000000052b7d2ULL, 0xdcc80cd2e4000000ULL, 26}
  ,	//  86-bit n < 10^26
  {0, 0x000000000052b7d2ULL, 0xdcc80cd2e4000000ULL, 26}
  ,	//  87-bit n ? 10^26
  {27, 0x00000000033b2e3cULL, 0x9fd0803ce8000000ULL, 27}
  ,	//  88-bit n < 10^27
  {27, 0x00000000033b2e3cULL, 0x9fd0803ce8000000ULL, 27}
  ,	//  89-bit n < 10^27
  {0, 0x00000000033b2e3cULL, 0x9fd0803ce8000000ULL, 27}
  ,	//  90-bit n ? 10^27
  {28, 0x00000000204fce5eULL, 0x3e25026110000000ULL, 28}
  ,	//  91-bit n < 10^28
  {28, 0x00000000204fce5eULL, 0x3e25026110000000ULL, 28}
  ,	//  92-bit n < 10^28
  {28, 0x00000000204fce5eULL, 0x3e25026110000000ULL, 28}
  ,	//  93-bit n < 10^28
  {0, 0x00000000204fce5eULL, 0x3e25026110000000ULL, 28}
  ,	//  94-bit n ? 10^28
  {29, 0x00000001431e0faeULL, 0x6d7217caa0000000ULL, 29}
  ,	//  95-bit n < 10^29
  {29, 0x00000001431e0faeULL, 0x6d7217caa0000000ULL, 29}
  ,	//  96-bit n < 10^29
  {0, 0x00000001431e0faeULL, 0x6d7217caa0000000ULL, 29}
  ,	//  97-bit n ? 10^29
  {30, 0x0000000c9f2c9cd0ULL, 0x4674edea40000000ULL, 30}
  ,	//  98-bit n < 10^30
  {30, 0x0000000c9f2c9cd0ULL, 0x4674edea40000000ULL, 30}
  ,	//  99-bit n < 10^30
  {0, 0x0000000c9f2c9cd0ULL, 0x4674edea40000000ULL, 30}
  ,	// 100-bit n ? 10^30
  {31, 0x0000007e37be2022ULL, 0xc0914b2680000000ULL, 31}
  ,	// 101-bit n < 10^31
  {31, 0x0000007e37be2022ULL, 0xc0914b2680000000ULL, 31}
  ,	// 102-bit n < 10^31
  {0, 0x0000007e37be2022ULL, 0xc0914b2680000000ULL, 31}
  ,	// 103-bit n ? 10^31
  {32, 0x000004ee2d6d415bULL, 0x85acef8100000000ULL, 32}
  ,	// 104-bit n < 10^32
  {32, 0x000004ee2d6d415bULL, 0x85acef8100000000ULL, 32}
  ,	// 105-bit n < 10^32
  {32, 0x000004ee2d6d415bULL, 0x85acef8100000000ULL, 32}
  ,	// 106-bit n < 10^32
  {0, 0x000004ee2d6d415bULL, 0x85acef8100000000ULL, 32}
  ,	// 107-bit n ? 10^32
  {33, 0x0000314dc6448d93ULL, 0x38c15b0a00000000ULL, 33}
  ,	// 108-bit n < 10^33
  {33, 0x0000314dc6448d93ULL, 0x38c15b0a00000000ULL, 33}
  ,	// 109-bit n < 10^33
  {0, 0x0000314dc6448d93ULL, 0x38c15b0a00000000ULL, 33}
  ,	// 100-bit n ? 10^33
  {34, 0x0001ed09bead87c0ULL, 0x378d8e6400000000ULL, 34}
  ,	// 111-bit n < 10^34
  {34, 0x0001ed09bead87c0ULL, 0x378d8e6400000000ULL, 34}
  ,	// 112-bit n < 10^34
  {0, 0x0001ed09bead87c0ULL, 0x378d8e6400000000ULL, 34}	// 113-bit n ? 10^34
//{ 35, 0x0013426172c74d82ULL, 0x2b878fe800000000ULL, 35 }  // 114-bit n < 10^35
};

// bid_midpoint64[i - 1] = 1/2 * 10^i = 5 * 10^(i-1), 1 <= i <= 19
BID_UINT64 bid_midpoint64[] = {
  0x0000000000000005ULL,	// 1/2 * 10^1 = 5 * 10^0
  0x0000000000000032ULL,	// 1/2 * 10^2 = 5 * 10^1
  0x00000000000001f4ULL,	// 1/2 * 10^3 = 5 * 10^2
  0x0000000000001388ULL,	// 1/2 * 10^4 = 5 * 10^3
  0x000000000000c350ULL,	// 1/2 * 10^5 = 5 * 10^4
  0x000000000007a120ULL,	// 1/2 * 10^6 = 5 * 10^5
  0x00000000004c4b40ULL,	// 1/2 * 10^7 = 5 * 10^6
  0x0000000002faf080ULL,	// 1/2 * 10^8 = 5 * 10^7
  0x000000001dcd6500ULL,	// 1/2 * 10^9 = 5 * 10^8
  0x000000012a05f200ULL,	// 1/2 * 10^10 = 5 * 10^9
  0x0000000ba43b7400ULL,	// 1/2 * 10^11 = 5 * 10^10
  0x000000746a528800ULL,	// 1/2 * 10^12 = 5 * 10^11
  0x0000048c27395000ULL,	// 1/2 * 10^13 = 5 * 10^12
  0x00002d79883d2000ULL,	// 1/2 * 10^14 = 5 * 10^13
  0x0001c6bf52634000ULL,	// 1/2 * 10^15 = 5 * 10^14
  0x0011c37937e08000ULL,	// 1/2 * 10^16 = 5 * 10^15
  0x00b1a2bc2ec50000ULL,	// 1/2 * 10^17 = 5 * 10^16
  0x06f05b59d3b20000ULL,	// 1/2 * 10^18 = 5 * 10^17
  0x4563918244f40000ULL	// 1/2 * 10^19 = 5 * 10^18
};

// bid_midpoint128[i - 20] = 1/2 * 10^i = 5 * 10^(i-1), 20 <= i <= 38
BID_UINT128 bid_midpoint128[] = {	// the 64-bit word order is L, H
  {{0xb5e3af16b1880000ULL, 0x0000000000000002ULL}
   }
  ,	// 1/2 * 10^20 = 5 * 10^19
  {{0x1ae4d6e2ef500000ULL, 0x000000000000001bULL}
   }
  ,	// 1/2 * 10^21 = 5 * 10^20
  {{0x0cf064dd59200000ULL, 0x000000000000010fULL}
   }
  ,	// 1/2 * 10^22 = 5 * 10^21
  {{0x8163f0a57b400000ULL, 0x0000000000000a96ULL}
   }
  ,	// 1/2 * 10^23 = 5 * 10^22
  {{0x0de76676d0800000ULL, 0x00000000000069e1ULL}
   }
  ,	// 1/2 * 10^24 = 5 * 10^23
  {{0x8b0a00a425000000ULL, 0x00000000000422caULL}
   }
  ,	// 1/2 * 10^25 = 5 * 10^24
  {{0x6e64066972000000ULL, 0x0000000000295be9ULL}
   }
  ,	// 1/2 * 10^26 = 5 * 10^25
  {{0x4fe8401e74000000ULL, 0x00000000019d971eULL}
   }
  ,	// 1/2 * 10^27 = 5 * 10^26
  {{0x1f12813088000000ULL, 0x000000001027e72fULL}
   }
  ,	// 1/2 * 10^28 = 5 * 10^27
  {{0x36b90be550000000ULL, 0x00000000a18f07d7ULL}
   }
  ,	// 1/2 * 10^29 = 5 * 10^28
  {{0x233a76f520000000ULL, 0x000000064f964e68ULL}
   }
  ,	// 1/2 * 10^30 = 5 * 10^29
  {{0x6048a59340000000ULL, 0x0000003f1bdf1011ULL}
   }
  ,	// 1/2 * 10^31 = 5 * 10^30
  {{0xc2d677c080000000ULL, 0x0000027716b6a0adULL}
   }
  ,	// 1/2 * 10^32 = 5 * 10^31
  {{0x9c60ad8500000000ULL, 0x000018a6e32246c9ULL}
   }
  ,	// 1/2 * 10^33 = 5 * 10^32
  {{0x1bc6c73200000000ULL, 0x0000f684df56c3e0ULL}
   }
  ,	// 1/2 * 10^34 = 5 * 10^33
  {{0x15c3c7f400000000ULL, 0x0009a130b963a6c1ULL}
   }
  ,	// 1/2 * 10^35 = 5 * 10^34
  {{0xd9a5cf8800000000ULL, 0x00604be73de4838aULL}
   }
  ,	// 1/2 * 10^36 = 5 * 10^35
  {{0x807a1b5000000000ULL, 0x03c2f7086aed236cULL}
   }
  ,	// 1/2 * 10^37 = 5 * 10^36
  {{0x04c5112000000000ULL, 0x259da6542d43623dULL}
   }	// 1/2 * 10^38 = 5 * 10^37
};

// bid_midpoint192[i - 39] = 1/2 * 10^i = 5 * 10^(i-1), 39 <= i <= 58
BID_UINT192 bid_midpoint192[] = {	// the 64-bit word order is L, M, H
  {{0x2fb2ab4000000000ULL, 0x78287f49c4a1d662ULL, 0x0000000000000001ULL}
   }
  ,
  // 1/2 * 10^39 = 5 * 10^38
  {{0xdcfab08000000000ULL, 0xb194f8e1ae525fd5ULL, 0x000000000000000eULL}
   }
  ,
  // 1/2 * 10^40 = 5 * 10^39
  {{0xa1cae50000000000ULL, 0xefd1b8d0cf37be5aULL, 0x0000000000000092ULL}
   }
  ,
  // 1/2 * 10^41 = 5 * 10^40
  {{0x51ecf20000000000ULL, 0x5e313828182d6f8aULL, 0x00000000000005bdULL}
   }
  ,
  // 1/2 * 10^42 = 5 * 10^41
  {{0x3341740000000000ULL, 0xadec3190f1c65b67ULL, 0x0000000000003965ULL}
   }
  ,
  // 1/2 * 10^43 = 5 * 10^42
  {{0x008e880000000000ULL, 0xcb39efa971bf9208ULL, 0x0000000000023df8ULL}
   }
  ,
  // 1/2 * 10^44 = 5 * 10^43
  {{0x0591500000000000ULL, 0xf0435c9e717bb450ULL, 0x0000000000166bb7ULL}
   }
  ,
  // 1/2 * 10^45 = 5 * 10^44
  {{0x37ad200000000000ULL, 0x62a19e306ed50b20ULL, 0x0000000000e0352fULL}
   }
  ,
  // 1/2 * 10^46 = 5 * 10^45
  {{0x2cc3400000000000ULL, 0xda502de454526f42ULL, 0x0000000008c213d9ULL}
   }
  ,
  // 1/2 * 10^47 = 5 * 10^46
  {{0xbfa0800000000000ULL, 0x8721caeb4b385895ULL, 0x000000005794c682ULL}
   }
  ,
  // 1/2 * 10^48 = 5 * 10^47
  {{0x7c45000000000000ULL, 0x4751ed30f03375d9ULL, 0x000000036bcfc119ULL}
   }
  ,
  // 1/2 * 10^49 = 5 * 10^48
  {{0xdab2000000000000ULL, 0xc93343e962029a7eULL, 0x00000022361d8afcULL}
   }
  ,
  // 1/2 * 10^50 = 5 * 10^49
  {{0x8af4000000000000ULL, 0xdc00a71dd41a08f4ULL, 0x000001561d276ddfULL}
   }
  ,
  // 1/2 * 10^51 = 5 * 10^50
  {{0x6d88000000000000ULL, 0x9806872a4904598dULL, 0x00000d5d238a4abeULL}
   }
  ,
  // 1/2 * 10^52 = 5 * 10^51
  {{0x4750000000000000ULL, 0xf04147a6da2b7f86ULL, 0x000085a36366eb71ULL}
   }
  ,
  // 1/2 * 10^53 = 5 * 10^52
  {{0xc920000000000000ULL, 0x628ccc8485b2fb3eULL, 0x00053861e2053273ULL}
   }
  ,
  // 1/2 * 10^54 = 5 * 10^53
  {{0xdb40000000000000ULL, 0xd97ffd2d38fdd073ULL, 0x003433d2d433f881ULL}
   }
  ,
  // 1/2 * 10^55 = 5 * 10^54
  {{0x9080000000000000ULL, 0x7effe3c439ea2486ULL, 0x020a063c4a07b512ULL}
   }
  ,
  // 1/2 * 10^56 = 5 * 10^55
  {{0xa500000000000000ULL, 0xf5fee5aa43256d41ULL, 0x14643e5ae44d12b8ULL}
   }
  ,
  // 1/2 * 10^57 = 5 * 10^56
  {{0x7200000000000000ULL, 0x9bf4f8a69f764490ULL, 0xcbea6f8ceb02bb39ULL}
   }
  // 1/2 * 10^58 = 5 * 10^57
};

// bid_midpoint256[i - 59] = 1/2 * 10^i = 5 * 10^(i-1), 59 <= i <= 68
BID_UINT256 bid_midpoint256[] = {	// the 64-bit word order is LL, LH, HL, HH
  {{0x7400000000000000ULL, 0x1791b6823a9eada4ULL,
    0xf7285b812e1b5040ULL, 0x0000000000000007ULL}
   }
  ,	// 1/2 * 10^59 = 5 * 10^58
  {{0x8800000000000000ULL, 0xebb121164a32c86cULL,
    0xa793930bcd112280ULL, 0x000000000000004fULL}
   }
  ,	// 1/2 * 10^60 = 5 * 10^59
  {{0x5000000000000000ULL, 0x34eb4adee5fbd43dULL,
    0x8bc3be7602ab5909ULL, 0x000000000000031cULL}
   }
  ,	// 1/2 * 10^61 = 5 * 10^60
  {{0x2000000000000000ULL, 0x1130ecb4fbd64a65ULL,
    0x75a5709c1ab17a5cULL, 0x0000000000001f1dULL}
   }
  ,	// 1/2 * 10^62 = 5 * 10^61
  {{0x4000000000000000ULL, 0xabe93f11d65ee7f3ULL,
    0x987666190aeec798ULL, 0x0000000000013726ULL}
   }
  ,	// 1/2 * 10^63 = 5 * 10^62
  {{0x8000000000000000ULL, 0xb71c76b25fb50f80ULL,
    0xf49ffcfa6d53cbf6ULL, 0x00000000000c2781ULL}
   }
  ,	// 1/2 * 10^64 = 5 * 10^63
  {{0x0000000000000000ULL, 0x271ca2f7bd129b05ULL,
    0x8e3fe1c84545f7a3ULL, 0x0000000000798b13ULL}
   }
  ,	// 1/2 * 10^65 = 5 * 10^64
  {{0x0000000000000000ULL, 0x871e5dad62ba0e32ULL,
    0x8e7ed1d2b4bbac5fULL, 0x0000000004bf6ec3ULL}
   }
  ,	// 1/2 * 10^66 = 5 * 10^65
  {{0x0000000000000000ULL, 0x472fa8c5db448df4ULL,
    0x90f4323b0f54bbbbULL, 0x000000002f7a53a3ULL}
   }
  ,	// 1/2 * 10^67 = 5 * 10^66
  {{0x0000000000000000ULL, 0xc7dc97ba90ad8b88ULL,
    0xa989f64e994f5550ULL, 0x00000001dac74463ULL}
   }
  ,	// 1/2 * 10^68 = 5 * 10^67
  {{0x0000000000000000ULL, 0xce9ded49a6c77350ULL,
    0x9f639f11fd195527ULL, 0x000000128bc8abe4ULL}
   }
  ,	// 1/2 * 10^69 = 5 * 10^68
  {{0x0000000000000000ULL, 0x122b44e083ca8120ULL,
    0x39e436b3e2fd538eULL, 0x000000b975d6b6eeULL}
   }
  ,	// 1/2 * 10^70 = 5 * 10^69
  {{0x0000000000000000ULL, 0xb5b0b0c525e90b40ULL,
    0x42ea2306dde5438cULL, 0x0000073e9a63254eULL}
   }
  ,	// 1/2 * 10^71 = 5 * 10^70
  {{0x0000000000000000ULL, 0x18e6e7b37b1a7080ULL,
    0x9d255e44aaf4a37fULL, 0x0000487207df750eULL}
   }
  ,	// 1/2 * 10^72 = 5 * 10^71
  {{0x0000000000000000ULL, 0xf9050d02cf086500ULL,
    0x2375aeaead8e62f6ULL, 0x0002d4744eba9292ULL}
   }
  ,	// 1/2 * 10^73 = 5 * 10^72
  {{0x0000000000000000ULL, 0xba32821c1653f200ULL,
    0x6298d2d2c78fdda5ULL, 0x001c4c8b1349b9b5ULL}
   }
  ,	// 1/2 * 10^74 = 5 * 10^73
  {{0x0000000000000000ULL, 0x45f91518df477400ULL,
    0xd9f83c3bcb9ea879ULL, 0x011afd6ec0e14115ULL}
   }
  ,	// 1/2 * 10^75 = 5 * 10^74
  {{0x0000000000000000ULL, 0xbbbad2f8b8ca8800ULL,
    0x83b25a55f43294bcULL, 0x0b0de65388cc8adaULL}
   }
  ,	// 1/2 * 10^76 = 5 * 10^75
  {{0x0000000000000000ULL, 0x554c3db737e95000ULL,
    0x24f7875b89f9cf5fULL, 0x6e8aff4357fd6c89ULL}
   }	// 1/2 * 10^77 = 5 * 10^76
};

// bid_ten2k64[i] = 10^i, 0 <= i <= 19
BID_UINT64 bid_ten2k64[] = {
  0x0000000000000001ULL,	// 10^0
  0x000000000000000aULL,	// 10^1
  0x0000000000000064ULL,	// 10^2
  0x00000000000003e8ULL,	// 10^3
  0x0000000000002710ULL,	// 10^4
  0x00000000000186a0ULL,	// 10^5
  0x00000000000f4240ULL,	// 10^6
  0x0000000000989680ULL,	// 10^7
  0x0000000005f5e100ULL,	// 10^8
  0x000000003b9aca00ULL,	// 10^9
  0x00000002540be400ULL,	// 10^10
  0x000000174876e800ULL,	// 10^11
  0x000000e8d4a51000ULL,	// 10^12
  0x000009184e72a000ULL,	// 10^13
  0x00005af3107a4000ULL,	// 10^14
  0x00038d7ea4c68000ULL,	// 10^15
  0x002386f26fc10000ULL,	// 10^16
  0x016345785d8a0000ULL,	// 10^17
  0x0de0b6b3a7640000ULL,	// 10^18
  0x8ac7230489e80000ULL	// 10^19 (20 digits)
};


// bid_ten2k128[i - 20] = 10^i, 20 <= i <= 38
BID_UINT128 bid_ten2k128[] = {	// the 64-bit word order is L, H
  {{0x6bc75e2d63100000ULL, 0x0000000000000005ULL}
   }
  ,	// 10^20
  {{0x35c9adc5dea00000ULL, 0x0000000000000036ULL}
   }
  ,	// 10^21
  {{0x19e0c9bab2400000ULL, 0x000000000000021eULL}
   }
  ,	// 10^22
  {{0x02c7e14af6800000ULL, 0x000000000000152dULL}
   }
  ,	// 10^23
  {{0x1bcecceda1000000ULL, 0x000000000000d3c2ULL}
   }
  ,	// 10^24
  {{0x161401484a000000ULL, 0x0000000000084595ULL}
   }
  ,	// 10^25
  {{0xdcc80cd2e4000000ULL, 0x000000000052b7d2ULL}
   }
  ,	// 10^26
  {{0x9fd0803ce8000000ULL, 0x00000000033b2e3cULL}
   }
  ,	// 10^27
  {{0x3e25026110000000ULL, 0x00000000204fce5eULL}
   }
  ,	// 10^28
  {{0x6d7217caa0000000ULL, 0x00000001431e0faeULL}
   }
  ,	// 10^29
  {{0x4674edea40000000ULL, 0x0000000c9f2c9cd0ULL}
   }
  ,	// 10^30
  {{0xc0914b2680000000ULL, 0x0000007e37be2022ULL}
   }
  ,	// 10^31
  {{0x85acef8100000000ULL, 0x000004ee2d6d415bULL}
   }
  ,	// 10^32
  {{0x38c15b0a00000000ULL, 0x0000314dc6448d93ULL}
   }
  ,	// 10^33
  {{0x378d8e6400000000ULL, 0x0001ed09bead87c0ULL}
   }
  ,	// 10^34
  {{0x2b878fe800000000ULL, 0x0013426172c74d82ULL}
   }
  ,	// 10^35
  {{0xb34b9f1000000000ULL, 0x00c097ce7bc90715ULL}
   }
  ,	// 10^36
  {{0x00f436a000000000ULL, 0x0785ee10d5da46d9ULL}
   }
  ,	// 10^37
  {{0x098a224000000000ULL, 0x4b3b4ca85a86c47aULL}
   }	// 10^38 (39 digits)
};

// might split into ten2k192[] and bid_ten2k256[]

// bid_ten2k256[i - 39] = 10^i, 39 <= i <= 68
BID_UINT256 bid_ten2k256[] = {	// the 64-bit word order is LL, LH, HL, HH
  {{0x5f65568000000000ULL, 0xf050fe938943acc4ULL,
    0x0000000000000002ULL, 0x0000000000000000ULL}
   }
  ,	// 10^39
  {{0xb9f5610000000000ULL, 0x6329f1c35ca4bfabULL,
    0x000000000000001dULL, 0x0000000000000000ULL}
   }
  ,	// 10^40
  {{0x4395ca0000000000ULL, 0xdfa371a19e6f7cb5ULL,
    0x0000000000000125ULL, 0x0000000000000000ULL}
   }
  ,	// 10^41
  {{0xa3d9e40000000000ULL, 0xbc627050305adf14ULL,
    0x0000000000000b7aULL, 0x0000000000000000ULL}
   }
  ,	// 10^42
  {{0x6682e80000000000ULL, 0x5bd86321e38cb6ceULL,
    0x00000000000072cbULL, 0x0000000000000000ULL}
   }
  ,	// 10^43
  {{0x011d100000000000ULL, 0x9673df52e37f2410ULL,
    0x0000000000047bf1ULL, 0x0000000000000000ULL}
   }
  ,	// 10^44
  {{0x0b22a00000000000ULL, 0xe086b93ce2f768a0ULL,
    0x00000000002cd76fULL, 0x0000000000000000ULL}
   }
  ,	// 10^45
  {{0x6f5a400000000000ULL, 0xc5433c60ddaa1640ULL,
    0x0000000001c06a5eULL, 0x0000000000000000ULL}
   }
  ,	// 10^46
  {{0x5986800000000000ULL, 0xb4a05bc8a8a4de84ULL,
    0x00000000118427b3ULL, 0x0000000000000000ULL}
   }
  ,	// 10^47
  {{0x7f41000000000000ULL, 0x0e4395d69670b12bULL,
    0x00000000af298d05ULL, 0x0000000000000000ULL}
   }
  ,	// 10^48
  {{0xf88a000000000000ULL, 0x8ea3da61e066ebb2ULL,
    0x00000006d79f8232ULL, 0x0000000000000000ULL}
   }
  ,	// 10^49
  {{0xb564000000000000ULL, 0x926687d2c40534fdULL,
    0x000000446c3b15f9ULL, 0x0000000000000000ULL}
   }
  ,	// 10^50
  {{0x15e8000000000000ULL, 0xb8014e3ba83411e9ULL,
    0x000002ac3a4edbbfULL, 0x0000000000000000ULL}
   }
  ,	// 10^51
  {{0xdb10000000000000ULL, 0x300d0e549208b31aULL,
    0x00001aba4714957dULL, 0x0000000000000000ULL}
   }
  ,	// 10^52
  {{0x8ea0000000000000ULL, 0xe0828f4db456ff0cULL,
    0x00010b46c6cdd6e3ULL, 0x0000000000000000ULL}
   }
  ,	// 10^53
  {{0x9240000000000000ULL, 0xc51999090b65f67dULL,
    0x000a70c3c40a64e6ULL, 0x0000000000000000ULL}
   }
  ,	// 10^54
  {{0xb680000000000000ULL, 0xb2fffa5a71fba0e7ULL,
    0x006867a5a867f103ULL, 0x0000000000000000ULL}
   }
  ,	// 10^55
  {{0x2100000000000000ULL, 0xfdffc78873d4490dULL,
    0x04140c78940f6a24ULL, 0x0000000000000000ULL}
   }
  ,	// 10^56
  {{0x4a00000000000000ULL, 0xebfdcb54864ada83ULL,
    0x28c87cb5c89a2571ULL, 0x0000000000000000ULL}
   }
  ,	// 10^57 (58 digits)
  {{0xe400000000000000ULL, 0x37e9f14d3eec8920ULL,
    0x97d4df19d6057673ULL, 0x0000000000000001ULL}
   }
  ,	// 10^58
  {{0xe800000000000000ULL, 0x2f236d04753d5b48ULL,
    0xee50b7025c36a080ULL, 0x000000000000000fULL}
   }
  ,	// 10^59
  {{0x1000000000000000ULL, 0xd762422c946590d9ULL,
    0x4f2726179a224501ULL, 0x000000000000009fULL}
   }
  ,	// 10^60
  {{0xa000000000000000ULL, 0x69d695bdcbf7a87aULL,
    0x17877cec0556b212ULL, 0x0000000000000639ULL}
   }
  ,	// 10^61
  {{0x4000000000000000ULL, 0x2261d969f7ac94caULL,
    0xeb4ae1383562f4b8ULL, 0x0000000000003e3aULL}
   }
  ,	// 10^62
  {{0x8000000000000000ULL, 0x57d27e23acbdcfe6ULL,
    0x30eccc3215dd8f31ULL, 0x0000000000026e4dULL}
   }
  ,	// 10^63
  {{0x0000000000000000ULL, 0x6e38ed64bf6a1f01ULL,
    0xe93ff9f4daa797edULL, 0x0000000000184f03ULL}
   }
  ,	// 10^64
  {{0x0000000000000000ULL, 0x4e3945ef7a25360aULL,
    0x1c7fc3908a8bef46ULL, 0x0000000000f31627ULL}
   }
  ,	// 10^65
  {{0x0000000000000000ULL, 0x0e3cbb5ac5741c64ULL,
    0x1cfda3a5697758bfULL, 0x00000000097edd87ULL}
   }
  ,	// 10^66
  {{0x0000000000000000ULL, 0x8e5f518bb6891be8ULL,
    0x21e864761ea97776ULL, 0x000000005ef4a747ULL}
   }
  ,	// 10^67
  {{0x0000000000000000ULL, 0x8fb92f75215b1710ULL,
    0x5313ec9d329eaaa1ULL, 0x00000003b58e88c7ULL}
   }
  ,	// 10^68
  {{0x0000000000000000ULL, 0x9d3bda934d8ee6a0ULL,
    0x3ec73e23fa32aa4fULL, 0x00000025179157c9ULL}
   }
  ,	// 10^69
  {{0x0000000000000000ULL, 0x245689c107950240ULL,
    0x73c86d67c5faa71cULL, 0x00000172ebad6ddcULL}
   }
  ,	// 10^70
  {{0x0000000000000000ULL, 0x6b61618a4bd21680ULL,
    0x85d4460dbbca8719ULL, 0x00000e7d34c64a9cULL}
   }
  ,	// 10^71
  {{0x0000000000000000ULL, 0x31cdcf66f634e100ULL,
    0x3a4abc8955e946feULL, 0x000090e40fbeea1dULL}
   }
  ,	// 10^72
  {{0x0000000000000000ULL, 0xf20a1a059e10ca00ULL,
    0x46eb5d5d5b1cc5edULL, 0x0005a8e89d752524ULL}
   }
  ,	// 10^73
  {{0x0000000000000000ULL, 0x746504382ca7e400ULL,
    0xc531a5a58f1fbb4bULL, 0x003899162693736aULL}
   }
  ,	// 10^74
  {{0x0000000000000000ULL, 0x8bf22a31be8ee800ULL,
    0xb3f07877973d50f2ULL, 0x0235fadd81c2822bULL}
   }
  ,	// 10^75
  {{0x0000000000000000ULL, 0x7775a5f171951000ULL,
    0x0764b4abe8652979ULL, 0x161bcca7119915b5ULL}
   }
  ,	// 10^76
  {{0x0000000000000000ULL, 0xaa987b6e6fd2a000ULL,
    0x49ef0eb713f39ebeULL, 0xdd15fe86affad912ULL}
   }	// 10^77
};

// bid_ten2mk128[k - 1] = 10^(-k) * 2^exp (k), where 1 <= k <= 34 and
// exp (k) = bid_shiftright128[k - 1] + 128
BID_UINT128 bid_ten2mk128[] = {
  {{0x999999999999999aULL, 0x1999999999999999ULL}
   }
  ,	//  10^(-1) * 2^128
  {{0x28f5c28f5c28f5c3ULL, 0x028f5c28f5c28f5cULL}
   }
  ,	//  10^(-2) * 2^128
  {{0x9db22d0e56041894ULL, 0x004189374bc6a7efULL}
   }
  ,	//  10^(-3) * 2^128
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
};


// bid_shiftright128[] contains the right shift count to obtain C2* from the top
// 128 bits of the 128x128-bit product C2 * Kx
int bid_shiftright128[] = {
  0,	// 128 - 128
  0,	// 128 - 128
  0,	// 128 - 128

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
  102	// 230 - 128
};


// bid_maskhigh128[] contains the mask to apply to the top 128 bits of the 
// 128x128-bit product in order to obtain the high bits of f2*
// the 64-bit word order is L, H
BID_UINT64 bid_maskhigh128[] = {
  0x0000000000000000ULL,	//  0 = 128 - 128 bits
  0x0000000000000000ULL,	//  0 = 128 - 128 bits
  0x0000000000000000ULL,	//  0 = 128 - 128 bits
  0x0000000000000007ULL,	//  3 = 131 - 128 bits
  0x000000000000003fULL,	//  6 = 134 - 128 bits
  0x00000000000001ffULL,	//  9 = 137 - 128 bits
  0x0000000000001fffULL,	// 13 = 141 - 128 bits
  0x000000000000ffffULL,	// 16 = 144 - 128 bits
  0x000000000007ffffULL,	// 19 = 147 - 128 bits
  0x00000000007fffffULL,	// 23 = 151 - 128 bits
  0x0000000003ffffffULL,	// 26 = 154 - 128 bits
  0x000000001fffffffULL,	// 29 = 157 - 128 bits
  0x00000001ffffffffULL,	// 33 = 161 - 128 bits
  0x0000000fffffffffULL,	// 36 = 164 - 128 bits
  0x0000007fffffffffULL,	// 39 = 167 - 128 bits
  0x000007ffffffffffULL,	// 43 = 171 - 128 bits
  0x00003fffffffffffULL,	// 46 = 174 - 128 bits
  0x0001ffffffffffffULL,	// 49 = 177 - 128 bits
  0x001fffffffffffffULL,	// 53 = 181 - 128 bits
  0x00ffffffffffffffULL,	// 56 = 184 - 128 bits
  0x07ffffffffffffffULL,	// 59 = 187 - 128 bits
  0x7fffffffffffffffULL,	// 63 = 191 - 128 bits
  0x0000000000000003ULL,	//  2 = 194 - 192 bits
  0x000000000000001fULL,	//  5 = 197 - 192 bits
  0x00000000000001ffULL,	//  9 = 201 - 192 bits
  0x0000000000000fffULL,	// 12 = 204 - 192 bits
  0x0000000000007fffULL,	// 15 = 207 - 192 bits
  0x000000000007ffffULL,	// 21 = 211 - 192 bits
  0x00000000003fffffULL,	// 22 = 214 - 192 bits
  0x0000000001ffffffULL,	// 25 = 217 - 192 bits
  0x000000000fffffffULL,	// 28 = 220 - 192 bits
  0x00000000ffffffffULL,	// 32 = 224 - 192 bits
  0x00000007ffffffffULL,	// 35 = 227 - 192 bits
  0x0000003fffffffffULL	// 38 = 230 - 192 bits
};


// bid_onehalf128[] contains the high bits of 1/2 positioned correctly for 
// comparison with the high bits of f2*
// the 64-bit word order is L, H
BID_UINT64 bid_onehalf128[] = {
  0x0000000000000000ULL,	//  0 bits
  0x0000000000000000ULL,	//  0 bits
  0x0000000000000000ULL,	//  0 bits
  0x0000000000000004ULL,	//  3 bits
  0x0000000000000020ULL,	//  6 bits
  0x0000000000000100ULL,	//  9 bits
  0x0000000000001000ULL,	// 13 bits
  0x0000000000008000ULL,	// 16 bits
  0x0000000000040000ULL,	// 19 bits
  0x0000000000400000ULL,	// 23 bits
  0x0000000002000000ULL,	// 26 bits
  0x0000000010000000ULL,	// 29 bits
  0x0000000100000000ULL,	// 33 bits
  0x0000000800000000ULL,	// 36 bits
  0x0000004000000000ULL,	// 39 bits
  0x0000040000000000ULL,	// 43 bits
  0x0000200000000000ULL,	// 46 bits
  0x0001000000000000ULL,	// 49 bits
  0x0010000000000000ULL,	// 53 bits
  0x0080000000000000ULL,	// 56 bits
  0x0400000000000000ULL,	// 59 bits
  0x4000000000000000ULL,	// 63 bits
  0x0000000000000002ULL,	// 66 bits
  0x0000000000000010ULL,	// 69 bits
  0x0000000000000100ULL,	// 73 bits
  0x0000000000000800ULL,	// 76 bits
  0x0000000000004000ULL,	// 79 bits
  0x0000000000040000ULL,	// 83 bits
  0x0000000000200000ULL,	// 86 bits
  0x0000000001000000ULL,	// 89 bits
  0x0000000008000000ULL,	// 92 bits
  0x0000000080000000ULL,	// 96 bits
  0x0000000400000000ULL,	// 99 bits
  0x0000002000000000ULL	// 102 bits
};

BID_UINT64 bid_ten2mk64[] = {
  0x199999999999999aULL,	//  10^(-1) * 2^ 64
  0x028f5c28f5c28f5dULL,	//  10^(-2) * 2^ 64 
  0x004189374bc6a7f0ULL,	//  10^(-3) * 2^ 64 
  0x00346dc5d638865aULL,	//  10^(-4) * 2^ 67 
  0x0029f16b11c6d1e2ULL,	//  10^(-5) * 2^ 70 
  0x00218def416bdb1bULL,	//  10^(-6) * 2^ 73 
  0x0035afe535795e91ULL,	//  10^(-7) * 2^ 77 
  0x002af31dc4611874ULL,	//  10^(-8) * 2^ 80 
  0x00225c17d04dad2aULL,	//  10^(-9) * 2^ 83 
  0x0036f9bfb3af7b76ULL,	// 10^(-10) * 2^ 87 
  0x002bfaffc2f2c92bULL,	// 10^(-11) * 2^ 90 
  0x00232f33025bd423ULL,	// 10^(-12) * 2^ 93 
  0x00384b84d092ed04ULL,	// 10^(-13) * 2^ 97 
  0x002d09370d425737ULL,	// 10^(-14) * 2^100 
  0x0024075f3dceac2cULL,	// 10^(-15) * 2^103 
  0x0039a5652fb11379ULL,	// 10^(-16) * 2^107 
};

// bid_ten2mk128trunc[] contains T*, the top Ex >= 128 bits of 10^(-k), 
// for 1 <= k <= 34 
// the 64-bit word order is L, H
BID_UINT128 bid_ten2mk128trunc[] = {
  {{0x9999999999999999ULL, 0x1999999999999999ULL}},	//  10^(-1) * 2^128
  {{0x28f5c28f5c28f5c2ULL, 0x028f5c28f5c28f5cULL}},	//  10^(-2) * 2^128 
  {{0x9db22d0e56041893ULL, 0x004189374bc6a7efULL}},	//  10^(-3) * 2^128 
  {{0x4af4f0d844d013a9ULL, 0x00346dc5d6388659ULL}},	//  10^(-4) * 2^131 
  {{0x08c3f3e0370cdc87ULL, 0x0029f16b11c6d1e1ULL}},	//  10^(-5) * 2^134 
  {{0x6d698fe69270b06cULL, 0x00218def416bdb1aULL}},	//  10^(-6) * 2^137 
  {{0xaf0f4ca41d811a46ULL, 0x0035afe535795e90ULL}},	//  10^(-7) * 2^141 
  {{0xbf3f70834acdae9fULL, 0x002af31dc4611873ULL}},	//  10^(-8) * 2^144 
  {{0x65cc5a02a23e254cULL, 0x00225c17d04dad29ULL}},	//  10^(-9) * 2^147 
  {{0x6fad5cd10396a213ULL, 0x0036f9bfb3af7b75ULL}},	// 10^(-10) * 2^151 
  {{0xbfbde3da69454e75ULL, 0x002bfaffc2f2c92aULL}},	// 10^(-11) * 2^154 
  {{0x32fe4fe1edd10b91ULL, 0x00232f33025bd422ULL}},	// 10^(-12) * 2^157 
  {{0x84ca19697c81ac1bULL, 0x00384b84d092ed03ULL}},	// 10^(-13) * 2^161 
  {{0x03d4e1213067bce3ULL, 0x002d09370d425736ULL}},	// 10^(-14) * 2^164 
  {{0x3643e74dc052fd82ULL, 0x0024075f3dceac2bULL}},	// 10^(-15) * 2^167 
  {{0x56d30baf9a1e626aULL, 0x0039a5652fb11378ULL}},	// 10^(-16) * 2^171 
  {{0x12426fbfae7eb521ULL, 0x002e1dea8c8da92dULL}},	// 10^(-17) * 2^174 
  {{0x41cebfcc8b9890e7ULL, 0x0024e4bba3a48757ULL}},	// 10^(-18) * 2^177 
  {{0x694acc7a78f41b0cULL, 0x003b07929f6da558ULL}},	// 10^(-19) * 2^181 
  {{0xbaa23d2ec729af3dULL, 0x002f394219248446ULL}},	// 10^(-20) * 2^184 
  {{0xfbb4fdbf05baf297ULL, 0x0025c768141d369eULL}},	// 10^(-21) * 2^187 
  {{0x2c54c931a2c4b758ULL, 0x003c7240202ebdcbULL}},	// 10^(-22) * 2^191 
  {{0x89dd6dc14f03c5e0ULL, 0x00305b66802564a2ULL}},	// 10^(-23) * 2^194 
  {{0xd4b1249aa59c9e4dULL, 0x0026af8533511d4eULL}},	// 10^(-24) * 2^197 
  {{0x544ea0f76f60fd48ULL, 0x003de5a1ebb4fbb1ULL}},	// 10^(-25) * 2^201 
  {{0x76a54d92bf80caa0ULL, 0x00318481895d9627ULL}},	// 10^(-26) * 2^204 
  {{0x921dd7a89933d54dULL, 0x00279d346de4781fULL}},	// 10^(-27) * 2^207 
  {{0x8362f2a75b862214ULL, 0x003f61ed7ca0c032ULL}},	// 10^(-28) * 2^211 
  {{0xcf825bb91604e810ULL, 0x0032b4bdfd4d668eULL}},	// 10^(-29) * 2^214 
  {{0x0c684960de6a5340ULL, 0x00289097fdd7853fULL}},	// 10^(-30) * 2^217 
  {{0x3d203ab3e521dc33ULL, 0x002073accb12d0ffULL}},	// 10^(-31) * 2^220 
  {{0x2e99f7863b696052ULL, 0x0033ec47ab514e65ULL}},	// 10^(-32) * 2^224 
  {{0x587b2c6b62bab375ULL, 0x002989d2ef743eb7ULL}},	// 10^(-33) * 2^227 
  {{0xad2f56bc4efbc2c4ULL, 0x00213b0f25f69892ULL}},	// 10^(-34) * 2^230 
};

// bid_ten2mk128M[k - 1] = 10^(-k) * 2^exp (k), where 1 <= k <= 4 and
// exp (k) = bid_shiftright128[k - 1] + 128
// the 64-bit word order is L, H
BID_UINT128 bid_ten2mk128M[] = {
  {{0xcccccccccccccccdULL, 0xccccccccccccccccULL}},	//  10^(-1) * 2^131
  {{0x3d70a3d70a3d70a4ULL, 0xa3d70a3d70a3d70aULL}},	//  10^(-2) * 2^134
  {{0x645a1cac083126eaULL, 0x83126e978d4fdf3bULL}},	//  10^(-3) * 2^137
  {{0xd3c36113404ea4a9ULL, 0xd1b71758e219652bULL}}	//  10^(-4) * 2^141
};

// bid_ten2mk128truncM[] contains T*, the top Ex >= 128 bits of 10^(-k),
// for 1 <= k <= 4; the top bits which are 0 are not represented
// the 64-bit word order is L, H
BID_UINT128 bid_ten2mk128truncM[] = {
  {{0xccccccccccccccccULL, 0xccccccccccccccccULL}},	//  10^(-1) * 2^131
  {{0x3d70a3d70a3d70a3ULL, 0xa3d70a3d70a3d70aULL}},	//  10^(-2) * 2^134
  {{0x645a1cac083126e9ULL, 0x83126e978d4fdf3bULL}},	//  10^(-3) * 2^137
  {{0xd3c36113404ea4a8ULL, 0xd1b71758e219652bULL}}	//  10^(-4) * 2^141
};

// bid_shiftright128M[] contains the right shift count to obtain C2* from the top
// 128 bits of the 128x128-bit product C2 * Kx
int bid_shiftright128M[] = {
  3,	// 131 - 128
  6,	// 134 - 128
  9,	// 137 - 128
  13	// 141 - 128
};

// bid_maskhigh128M[] contains the mask to apply to the top 128 bits of the
// 128x128-bit product in order to obtain the high bits of f*
// the high 64 bits of the mask are 0, so only the low 64 bits are represented
BID_UINT64 bid_maskhigh128M[] = {
  0x0000000000000007ULL,	//  3 = 131 - 128 bits
  0x000000000000003fULL,	//  6 = 134 - 128 bits
  0x00000000000001ffULL,	//  9 = 137 - 128 bits
  0x0000000000001fffULL	// 13 = 141 - 128 bits
};

// bid_onehalf128M[] contains 1/2 positioned correctly for
// comparison with the high bits of f*
// the high 64 bits are 0, so only the low 64 bits are represented
BID_UINT64 bid_onehalf128M[] = {
  0x0000000000000004ULL,	//  3 bits
  0x0000000000000020ULL,	//  6 bits
  0x0000000000000100ULL,	//  9 bits
  0x0000000000001000ULL	// 13 bits
};

// bid_ten2mk192M[k - 1] = 10^(-k-4) * 2^exp (k), where 1 <= k <= 19 and
// exp (k) = bid_shiftright128[k - 1] + 128
// the 64-bit word order is L, M, H
BID_UINT192 bid_ten2mk192M[] = {
  {{0xcddd6e04c0592104ULL, 0x0fcf80dc33721d53ULL,
    0xa7c5ac471b478423ULL}},
  //  10^(-5) * 2^208
  {{0xd7e45803cd141a6aULL, 0xa63f9a49c2c1b10fULL,
    0x8637bd05af6c69b5ULL}},
  //  10^(-6) * 2^211
  {{0x8ca08cd2e1b9c3dcULL, 0x3d32907604691b4cULL,
    0xd6bf94d5e57a42bcULL}},
  //  10^(-7) * 2^215
  {{0x3d4d3d758161697dULL, 0xfdc20d2b36ba7c3dULL,
    0xabcc77118461cefcULL}},
  //  10^(-8) * 2^218
  {{0xfdd7645e011abacaULL, 0x31680a88f8953030ULL,
    0x89705f4136b4a597ULL}},
  //  10^(-9) * 2^221
  {{0x2fbf06fcce912addULL, 0xb573440e5a884d1bULL,
    0xdbe6fecebdedd5beULL}},
  //  10^(-10) * 2^225
  {{0xf2ff38ca3eda88b1ULL, 0xf78f69a51539d748ULL,
    0xafebff0bcb24aafeULL}},
  //  10^(-11) * 2^228
  {{0xf598fa3b657ba08eULL, 0xf93f87b7442e45d3ULL,
    0x8cbccc096f5088cbULL}},
  //  10^(-12) * 2^231
  {{0x88f4c3923bf900e3ULL, 0x2865a5f206b06fb9ULL,
    0xe12e13424bb40e13ULL}},
  //  10^(-13) * 2^235
  {{0x6d909c74fcc733e9ULL, 0x538484c19ef38c94ULL,
    0xb424dc35095cd80fULL}},
  //  10^(-14) * 2^238
  {{0x57a6e390ca38f654ULL, 0x0f9d37014bf60a10ULL,
    0x901d7cf73ab0acd9ULL}},
  //  10^(-15) * 2^241
  {{0xbf716c1add27f086ULL, 0x4c2ebe687989a9b3ULL,
    0xe69594bec44de15bULL}},
  //  10^(-16) * 2^245
  {{0xff8df0157db98d38ULL, 0x09befeb9fad487c2ULL,
    0xb877aa3236a4b449ULL}},
  //  10^(-17) * 2^248
  {{0x32d7f344649470faULL, 0x3aff322e62439fcfULL,
    0x9392ee8e921d5d07ULL}},
  //  10^(-18) * 2^251
  {{0x1e2652070753e7f5ULL, 0x2b31e9e3d06c32e5ULL,
    0xec1e4a7db69561a5ULL}},
  //  10^(-19) * 2^255
  {{0x181ea8059f76532bULL, 0x88f4bb1ca6bcf584ULL,
    0xbce5086492111aeaULL}},
  //  10^(-20) * 2^258
  {{0x467eecd14c5ea8efULL, 0xd3f6fc16ebca5e03ULL,
    0x971da05074da7beeULL}},
  //  10^(-21) * 2^261
  {{0x70cb148213caa7e5ULL, 0x5324c68b12dd6338ULL,
    0xf1c90080baf72cb1ULL}},
  //  10^(-22) * 2^265
  {{0x8d6f439b43088651ULL, 0x75b7053c0f178293ULL, 0xc16d9a0095928a27ULL}}
  //  10^(-23) * 2^268
};

// bid_ten2mk192truncM[] contains T*, the top Ex >= 192 bits of 10^(-k),
// for 5 <= k <= 23; the top bits which are 0 are not represented
// the 64-bit word order is L, M, H
BID_UINT192 bid_ten2mk192truncM[] = {
  {{0xcddd6e04c0592103ULL, 0x0fcf80dc33721d53ULL,
    0xa7c5ac471b478423ULL}},
  //  10^(-5) * 2^208
  {{0xd7e45803cd141a69ULL, 0xa63f9a49c2c1b10fULL,
    0x8637bd05af6c69b5ULL}},
  //  10^(-6) * 2^211
  {{0x8ca08cd2e1b9c3dbULL, 0x3d32907604691b4cULL,
    0xd6bf94d5e57a42bcULL}},
  //  10^(-7) * 2^215
  {{0x3d4d3d758161697cULL, 0xfdc20d2b36ba7c3dULL,
    0xabcc77118461cefcULL}},
  //  10^(-8) * 2^218
  {{0xfdd7645e011abac9ULL, 0x31680a88f8953030ULL,
    0x89705f4136b4a597ULL}},
  //  10^(-9) * 2^221
  {{0x2fbf06fcce912adcULL, 0xb573440e5a884d1bULL,
    0xdbe6fecebdedd5beULL}},
  //  10^(-10) * 2^225
  {{0xf2ff38ca3eda88b0ULL, 0xf78f69a51539d748ULL,
    0xafebff0bcb24aafeULL}},
  //  10^(-11) * 2^228
  {{0xf598fa3b657ba08dULL, 0xf93f87b7442e45d3ULL,
    0x8cbccc096f5088cbULL}},
  //  10^(-12) * 2^231
  {{0x88f4c3923bf900e2ULL, 0x2865a5f206b06fb9ULL,
    0xe12e13424bb40e13ULL}},
  //  10^(-13) * 2^235
  {{0x6d909c74fcc733e8ULL, 0x538484c19ef38c94ULL,
    0xb424dc35095cd80fULL}},
  //  10^(-14) * 2^238
  {{0x57a6e390ca38f653ULL, 0x0f9d37014bf60a10ULL,
    0x901d7cf73ab0acd9ULL}},
  //  10^(-15) * 2^241
  {{0xbf716c1add27f085ULL, 0x4c2ebe687989a9b3ULL,
    0xe69594bec44de15bULL}},
  //  10^(-16) * 2^245
  {{0xff8df0157db98d37ULL, 0x09befeb9fad487c2ULL,
    0xb877aa3236a4b449ULL}},
  //  10^(-17) * 2^248
  {{0x32d7f344649470f9ULL, 0x3aff322e62439fcfULL,
    0x9392ee8e921d5d07ULL}},
  //  10^(-18) * 2^251
  {{0x1e2652070753e7f4ULL, 0x2b31e9e3d06c32e5ULL,
    0xec1e4a7db69561a5ULL}},
  //  10^(-19) * 2^255
  {{0x181ea8059f76532aULL, 0x88f4bb1ca6bcf584ULL,
    0xbce5086492111aeaULL}},
  //  10^(-20) * 2^258
  {{0x467eecd14c5ea8eeULL, 0xd3f6fc16ebca5e03ULL,
    0x971da05074da7beeULL}},
  //  10^(-21) * 2^261
  {{0x70cb148213caa7e4ULL, 0x5324c68b12dd6338ULL,
    0xf1c90080baf72cb1ULL}},
  //  10^(-22) * 2^265
  {{0x8d6f439b43088650ULL, 0x75b7053c0f178293ULL, 0xc16d9a0095928a27ULL}}
  //  10^(-23) * 2^268
};

// bid_shiftright192M[] contains the right shift count to obtain C2* from the top
// 192 bits of the 192x192-bit product C2 * Kx if 0 <= ind <= 14 where ind is 
// the index in the table, or from the top 128 bits if 15 <= ind <= 18
int bid_shiftright192M[] = {
  16,	// 208 - 192
  19,	// 211 - 192
  23,	// 215 - 192
  26,	// 218 - 192
  29,	// 221 - 192
  33,	// 225 - 192
  36,	// 228 - 192
  39,	// 231 - 192
  43,	// 235 - 192
  46,	// 238 - 192
  49,	// 241 - 192
  53,	// 245 - 192
  56,	// 248 - 192
  59,	// 251 - 192
  63,	// 255 - 192
  2,	// 258 - 256
  5,	// 261 - 256
  9,	// 265 - 256
  12	// 268 - 256
};

// bid_maskhigh192M[] contains the mask to apply to the top 192 bits of the
// 192x192-bit product in order to obtain the high bits of f*
// if 0 <= ind <= 14 where ind is the index in the table, then the high 128 bits
// of the 384-bit mask are 0;  if 15 <= ind <= 18 then the high 64 bits are 0
BID_UINT64 bid_maskhigh192M[] = {
  0x000000000000ffffULL,	//  16 = 208 - 192 bits
  0x000000000007ffffULL,	//  19 = 211 - 192 bits
  0x00000000007fffffULL,	//  23 = 215 - 192 bits
  0x0000000003ffffffULL,	//  26 = 218 - 192 bits
  0x000000001fffffffULL,	//  29 = 221 - 192 bits
  0x00000001ffffffffULL,	//  33 = 225 - 192 bits
  0x0000000fffffffffULL,	//  36 = 228 - 192 bits
  0x0000007fffffffffULL,	//  39 = 231 - 192 bits
  0x000007ffffffffffULL,	//  43 = 235 - 192 bits
  0x00003fffffffffffULL,	//  46 = 238 - 192 bits
  0x0001ffffffffffffULL,	//  49 = 241 - 192 bits
  0x001fffffffffffffULL,	//  53 = 245 - 192 bits
  0x00ffffffffffffffULL,	//  56 = 248 - 192 bits
  0x07ffffffffffffffULL,	//  59 = 251 - 192 bits
  0x7fffffffffffffffULL,	//  63 = 255 - 192 bits
  0x0000000000000003ULL,	//   2 = 258 - 256 bits
  0x000000000000001fULL,	//   5 = 261 - 256 bits
  0x00000000000001ffULL,	//   9 = 265 - 256 bits
  0x0000000000000fffULL	//  12 = 268 - 256 bits
};

// bid_onehalf192M[] contains 1/2 positioned correctly for
// comparison with the high bits of f*
// if 0 <= ind <= 14 where ind is the index in the table, then the high 128 bits
// of the 384-bit mask are 0;  if 15 <= ind <= 18 then the high 648 bits are 0
BID_UINT64 bid_onehalf192M[] = {
  0x0000000000008000ULL,	//  16 = 208 - 192 bits
  0x0000000000040000ULL,	//  19 = 211 - 192 bits
  0x0000000000400000ULL,	//  23 = 215 - 192 bits
  0x0000000002000000ULL,	//  26 = 218 - 192 bits
  0x0000000010000000ULL,	//  29 = 221 - 192 bits
  0x0000000100000000ULL,	//  33 = 225 - 192 bits
  0x0000000800000000ULL,	//  36 = 228 - 192 bits
  0x0000004000000000ULL,	//  39 = 231 - 192 bits
  0x0000040000000000ULL,	//  43 = 235 - 192 bits
  0x0000200000000000ULL,	//  46 = 238 - 192 bits
  0x0001000000000000ULL,	//  49 = 241 - 192 bits
  0x0010000000000000ULL,	//  53 = 245 - 192 bits
  0x0080000000000000ULL,	//  56 = 248 - 192 bits
  0x0400000000000000ULL,	//  59 = 251 - 192 bits
  0x4000000000000000ULL,	//  63 = 255 - 192 bits
  0x0000000000000002ULL,	//   2 = 258 - 256 bits
  0x0000000000000010ULL,	//   5 = 261 - 256 bits
  0x0000000000000100ULL,	//   9 = 265 - 256 bits
  0x0000000000000800ULL	//  12 = 268 - 256 bits
};

// bid_ten2mk256M[k - 1] = 10^(-k-23) * 2^exp (k), where 1 <= k <= 11 and
// exp (k) = bid_shiftright128[k - 1] + 128
BID_UINT256 bid_ten2mk256M[] = {	// the 64-bit word order is LL, LH, HL, HH
  {{0xf23472530ce6e3edULL, 0xd78c3615cf3a050cULL,
    0xc4926a9672793542ULL, 0x9abe14cd44753b52ULL}},	//  10^(-24) * 2^335
  {{0xe9ed83b814a49fe1ULL, 0x8c1389bc7ec33b47ULL,
    0x3a83ddbd83f52204ULL, 0xf79687aed3eec551ULL}},	//  10^(-25) * 2^339
  {{0x87f1362cdd507fe7ULL, 0x3cdc6e306568fc39ULL,
    0x95364afe032a819dULL, 0xc612062576589ddaULL}},	//  10^(-26) * 2^342
  {{0x9ff42b5717739986ULL, 0xca49f1c05120c9c7ULL,
    0x775ea264cf55347dULL, 0x9e74d1b791e07e48ULL}},	//  10^(-27) * 2^345
  {{0xccb9def1bf1f5c09ULL, 0x76dcb60081ce0fa5ULL,
    0x8bca9d6e188853fcULL, 0xfd87b5f28300ca0dULL}},	//  10^(-28) * 2^349
  {{0xa3c7e58e327f7cd4ULL, 0x5f16f80067d80c84ULL,
    0x096ee45813a04330ULL, 0xcad2f7f5359a3b3eULL}},	//  10^(-29) * 2^352
  {{0xb6398471c1ff9710ULL, 0x18df2ccd1fe00a03ULL,
    0xa1258379a94d028dULL, 0xa2425ff75e14fc31ULL}},	//  10^(-30) * 2^355
  {{0xf82e038e34cc78daULL, 0x4718f0a419800802ULL,
    0x80eacf948770ced7ULL, 0x81ceb32c4b43fcf4ULL}},	//  10^(-31) * 2^358
  {{0x59e338e387ad8e29ULL, 0x0b5b1aa028ccd99eULL,
    0x67de18eda5814af2ULL, 0xcfb11ead453994baULL}},	//  10^(-32) * 2^362
  {{0x47e8fa4f9fbe0b54ULL, 0x6f7c154ced70ae18ULL,
    0xecb1ad8aeacdd58eULL, 0xa6274bbdd0fadd61ULL}},	//  10^(-33) * 2^365
  {{0xd320c83fb2fe6f76ULL, 0xbf967770bdf3be79ULL,
    0xbd5af13bef0b113eULL, 0x84ec3c97da624ab4ULL}}	//  10^(-34) * 2^368
};

// bid_ten2mk256truncM[] contains T*, the top Ex >= 256 bits of 10^(-k),
// for 24 <= k <= 34; the top bits which are 0 are not represented
BID_UINT256 bid_ten2mk256truncM[] = {	// the 64-bit word order is LL, LH, HL, HH
  {{0xf23472530ce6e3ecULL, 0xd78c3615cf3a050cULL,
    0xc4926a9672793542ULL, 0x9abe14cd44753b52ULL}},	//  10^(-24) * 2^335
  {{0xe9ed83b814a49fe0ULL, 0x8c1389bc7ec33b47ULL,
    0x3a83ddbd83f52204ULL, 0xf79687aed3eec551ULL}},	//  10^(-25) * 2^339
  {{0x87f1362cdd507fe6ULL, 0x3cdc6e306568fc39ULL,
    0x95364afe032a819dULL, 0xc612062576589ddaULL}},	//  10^(-26) * 2^342
  {{0x775ea264cf55347cULL, 0x9ff42b5717739986ULL,
    0xca49f1c05120c9c7ULL, 0x9e74d1b791e07e48ULL}},	//  10^(-27) * 2^345
  {{0xccb9def1bf1f5c08ULL, 0x76dcb60081ce0fa5ULL,
    0x8bca9d6e188853fcULL, 0xfd87b5f28300ca0dULL}},	//  10^(-28) * 2^349
  {{0xa3c7e58e327f7cd3ULL, 0x5f16f80067d80c84ULL,
    0x096ee45813a04330ULL, 0xcad2f7f5359a3b3eULL}},	//  10^(-29) * 2^352
  {{0xb6398471c1ff970fULL, 0x18df2ccd1fe00a03ULL,
    0xa1258379a94d028dULL, 0xa2425ff75e14fc31ULL}},	//  10^(-30) * 2^355
  {{0xf82e038e34cc78d9ULL, 0x4718f0a419800802ULL,
    0x80eacf948770ced7ULL, 0x81ceb32c4b43fcf4ULL}},	//  10^(-31) * 2^358
  {{0x59e338e387ad8e28ULL, 0x0b5b1aa028ccd99eULL,
    0x67de18eda5814af2ULL, 0xcfb11ead453994baULL}},	//  10^(-32) * 2^362
  {{0x47e8fa4f9fbe0b53ULL, 0x6f7c154ced70ae18ULL,
    0xecb1ad8aeacdd58eULL, 0xa6274bbdd0fadd61ULL}},	//  10^(-33) * 2^365
  {{0xd320c83fb2fe6f75ULL, 0xbf967770bdf3be79ULL,
    0xbd5af13bef0b113eULL, 0x84ec3c97da624ab4ULL}}	//  10^(-34) * 2^368
};

// bid_shiftright256M[] contains the right shift count to obtain C2* from the top
// 192 bits of the 256x256-bit product C2 * Kx 
int bid_shiftright256M[] = {
  15,	// 335 - 320
  19,	// 339 - 320
  22,	// 342 - 320
  25,	// 345 - 320
  29,	// 349 - 320
  32,	// 352 - 320 // careful of 32-bit machines!
  35,	// 355 - 320
  38,	// 358 - 320
  42,	// 362 - 320
  45,	// 365 - 320
  48	// 368 - 320
};

// bid_maskhigh256M[] contains the mask to apply to the top 192 bits of the
// 256x256-bit product in order to obtain the high bits of f*
BID_UINT64 bid_maskhigh256M[] = {
  0x0000000000007fffULL,	//  15 = 335 - 320 bits
  0x000000000007ffffULL,	//  19 = 339 - 320 bits
  0x00000000003fffffULL,	//  22 = 342 - 320 bits
  0x0000000001ffffffULL,	//  25 = 345 - 320 bits
  0x000000001fffffffULL,	//  29 = 349 - 320 bits
  0x00000000ffffffffULL,	//  32 = 352 - 320 bits
  0x00000007ffffffffULL,	//  35 = 355 - 320 bits
  0x0000003fffffffffULL,	//  38 = 358 - 320 bits
  0x000003ffffffffffULL,	//  42 = 362 - 320 bits
  0x00001fffffffffffULL,	//  45 = 365 - 320 bits
  0x0000ffffffffffffULL	//  48 = 368 - 320 bits
};

// bid_onehalf256M[] contains 1/2 positioned correctly for comparison with the 
// high bits of f*; the high 128 bits of the 512-bit mask are 0
BID_UINT64 bid_onehalf256M[] = {
  0x0000000000004000ULL,	//  15 = 335 - 320 bits
  0x0000000000040000ULL,	//  19 = 339 - 320 bits
  0x0000000000200000ULL,	//  22 = 342 - 320 bits
  0x0000000001000000ULL,	//  25 = 345 - 320 bits
  0x0000000010000000ULL,	//  29 = 349 - 320 bits
  0x0000000080000000ULL,	//  32 = 352 - 320 bits
  0x0000000400000000ULL,	//  35 = 355 - 320 bits
  0x0000002000000000ULL,	//  38 = 358 - 320 bits
  0x0000020000000000ULL,	//  42 = 362 - 320 bits
  0x0000100000000000ULL,	//  45 = 365 - 320 bits
  0x0000800000000000ULL	//  48 = 368 - 320 bits
};


// bid_char_table2[] is used to convert n to string, where 10 <= n <= 99
unsigned char bid_char_table2[180] = {
  '1', '0',
  '1', '1',
  '1', '2',
  '1', '3',
  '1', '4',
  '1', '5',
  '1', '6',
  '1', '7',
  '1', '8',
  '1', '9',
  '2', '0',
  '2', '1',
  '2', '2',
  '2', '3',
  '2', '4',
  '2', '5',
  '2', '6',
  '2', '7',
  '2', '8',
  '2', '9',
  '3', '0',
  '3', '1',
  '3', '2',
  '3', '3',
  '3', '4',
  '3', '5',
  '3', '6',
  '3', '7',
  '3', '8',
  '3', '9',
  '4', '0',
  '4', '1',
  '4', '2',
  '4', '3',
  '4', '4',
  '4', '5',
  '4', '6',
  '4', '7',
  '4', '8',
  '4', '9',
  '5', '0',
  '5', '1',
  '5', '2',
  '5', '3',
  '5', '4',
  '5', '5',
  '5', '6',
  '5', '7',
  '5', '8',
  '5', '9',
  '6', '0',
  '6', '1',
  '6', '2',
  '6', '3',
  '6', '4',
  '6', '5',
  '6', '6',
  '6', '7',
  '6', '8',
  '6', '9',
  '7', '0',
  '7', '1',
  '7', '2',
  '7', '3',
  '7', '4',
  '7', '5',
  '7', '6',
  '7', '7',
  '7', '8',
  '7', '9',
  '8', '0',
  '8', '1',
  '8', '2',
  '8', '3',
  '8', '4',
  '8', '5',
  '8', '6',
  '8', '7',
  '8', '8',
  '8', '9',
  '9', '0',
  '9', '1',
  '9', '2',
  '9', '3',
  '9', '4',
  '9', '5',
  '9', '6',
  '9', '7',
  '9', '8',
  '9', '9'
};


// bid_char_table3[] is used to convert n to string, where 000 <= n <= 999
unsigned char bid_char_table3[3000] = {
  '0', '0', '0',
  '0', '0', '1',
  '0', '0', '2',
  '0', '0', '3',
  '0', '0', '4',
  '0', '0', '5',
  '0', '0', '6',
  '0', '0', '7',
  '0', '0', '8',
  '0', '0', '9',
  '0', '1', '0',
  '0', '1', '1',
  '0', '1', '2',
  '0', '1', '3',
  '0', '1', '4',
  '0', '1', '5',
  '0', '1', '6',
  '0', '1', '7',
  '0', '1', '8',
  '0', '1', '9',
  '0', '2', '0',
  '0', '2', '1',
  '0', '2', '2',
  '0', '2', '3',
  '0', '2', '4',
  '0', '2', '5',
  '0', '2', '6',
  '0', '2', '7',
  '0', '2', '8',
  '0', '2', '9',
  '0', '3', '0',
  '0', '3', '1',
  '0', '3', '2',
  '0', '3', '3',
  '0', '3', '4',
  '0', '3', '5',
  '0', '3', '6',
  '0', '3', '7',
  '0', '3', '8',
  '0', '3', '9',
  '0', '4', '0',
  '0', '4', '1',
  '0', '4', '2',
  '0', '4', '3',
  '0', '4', '4',
  '0', '4', '5',
  '0', '4', '6',
  '0', '4', '7',
  '0', '4', '8',
  '0', '4', '9',
  '0', '5', '0',
  '0', '5', '1',
  '0', '5', '2',
  '0', '5', '3',
  '0', '5', '4',
  '0', '5', '5',
  '0', '5', '6',
  '0', '5', '7',
  '0', '5', '8',
  '0', '5', '9',
  '0', '6', '0',
  '0', '6', '1',
  '0', '6', '2',
  '0', '6', '3',
  '0', '6', '4',
  '0', '6', '5',
  '0', '6', '6',
  '0', '6', '7',
  '0', '6', '8',
  '0', '6', '9',
  '0', '7', '0',
  '0', '7', '1',
  '0', '7', '2',
  '0', '7', '3',
  '0', '7', '4',
  '0', '7', '5',
  '0', '7', '6',
  '0', '7', '7',
  '0', '7', '8',
  '0', '7', '9',
  '0', '8', '0',
  '0', '8', '1',
  '0', '8', '2',
  '0', '8', '3',
  '0', '8', '4',
  '0', '8', '5',
  '0', '8', '6',
  '0', '8', '7',
  '0', '8', '8',
  '0', '8', '9',
  '0', '9', '0',
  '0', '9', '1',
  '0', '9', '2',
  '0', '9', '3',
  '0', '9', '4',
  '0', '9', '5',
  '0', '9', '6',
  '0', '9', '7',
  '0', '9', '8',
  '0', '9', '9',
  '1', '0', '0',
  '1', '0', '1',
  '1', '0', '2',
  '1', '0', '3',
  '1', '0', '4',
  '1', '0', '5',
  '1', '0', '6',
  '1', '0', '7',
  '1', '0', '8',
  '1', '0', '9',
  '1', '1', '0',
  '1', '1', '1',
  '1', '1', '2',
  '1', '1', '3',
  '1', '1', '4',
  '1', '1', '5',
  '1', '1', '6',
  '1', '1', '7',
  '1', '1', '8',
  '1', '1', '9',
  '1', '2', '0',
  '1', '2', '1',
  '1', '2', '2',
  '1', '2', '3',
  '1', '2', '4',
  '1', '2', '5',
  '1', '2', '6',
  '1', '2', '7',
  '1', '2', '8',
  '1', '2', '9',
  '1', '3', '0',
  '1', '3', '1',
  '1', '3', '2',
  '1', '3', '3',
  '1', '3', '4',
  '1', '3', '5',
  '1', '3', '6',
  '1', '3', '7',
  '1', '3', '8',
  '1', '3', '9',
  '1', '4', '0',
  '1', '4', '1',
  '1', '4', '2',
  '1', '4', '3',
  '1', '4', '4',
  '1', '4', '5',
  '1', '4', '6',
  '1', '4', '7',
  '1', '4', '8',
  '1', '4', '9',
  '1', '5', '0',
  '1', '5', '1',
  '1', '5', '2',
  '1', '5', '3',
  '1', '5', '4',
  '1', '5', '5',
  '1', '5', '6',
  '1', '5', '7',
  '1', '5', '8',
  '1', '5', '9',
  '1', '6', '0',
  '1', '6', '1',
  '1', '6', '2',
  '1', '6', '3',
  '1', '6', '4',
  '1', '6', '5',
  '1', '6', '6',
  '1', '6', '7',
  '1', '6', '8',
  '1', '6', '9',
  '1', '7', '0',
  '1', '7', '1',
  '1', '7', '2',
  '1', '7', '3',
  '1', '7', '4',
  '1', '7', '5',
  '1', '7', '6',
  '1', '7', '7',
  '1', '7', '8',
  '1', '7', '9',
  '1', '8', '0',
  '1', '8', '1',
  '1', '8', '2',
  '1', '8', '3',
  '1', '8', '4',
  '1', '8', '5',
  '1', '8', '6',
  '1', '8', '7',
  '1', '8', '8',
  '1', '8', '9',
  '1', '9', '0',
  '1', '9', '1',
  '1', '9', '2',
  '1', '9', '3',
  '1', '9', '4',
  '1', '9', '5',
  '1', '9', '6',
  '1', '9', '7',
  '1', '9', '8',
  '1', '9', '9',
  '2', '0', '0',
  '2', '0', '1',
  '2', '0', '2',
  '2', '0', '3',
  '2', '0', '4',
  '2', '0', '5',
  '2', '0', '6',
  '2', '0', '7',
  '2', '0', '8',
  '2', '0', '9',
  '2', '1', '0',
  '2', '1', '1',
  '2', '1', '2',
  '2', '1', '3',
  '2', '1', '4',
  '2', '1', '5',
  '2', '1', '6',
  '2', '1', '7',
  '2', '1', '8',
  '2', '1', '9',
  '2', '2', '0',
  '2', '2', '1',
  '2', '2', '2',
  '2', '2', '3',
  '2', '2', '4',
  '2', '2', '5',
  '2', '2', '6',
  '2', '2', '7',
  '2', '2', '8',
  '2', '2', '9',
  '2', '3', '0',
  '2', '3', '1',
  '2', '3', '2',
  '2', '3', '3',
  '2', '3', '4',
  '2', '3', '5',
  '2', '3', '6',
  '2', '3', '7',
  '2', '3', '8',
  '2', '3', '9',
  '2', '4', '0',
  '2', '4', '1',
  '2', '4', '2',
  '2', '4', '3',
  '2', '4', '4',
  '2', '4', '5',
  '2', '4', '6',
  '2', '4', '7',
  '2', '4', '8',
  '2', '4', '9',
  '2', '5', '0',
  '2', '5', '1',
  '2', '5', '2',
  '2', '5', '3',
  '2', '5', '4',
  '2', '5', '5',
  '2', '5', '6',
  '2', '5', '7',
  '2', '5', '8',
  '2', '5', '9',
  '2', '6', '0',
  '2', '6', '1',
  '2', '6', '2',
  '2', '6', '3',
  '2', '6', '4',
  '2', '6', '5',
  '2', '6', '6',
  '2', '6', '7',
  '2', '6', '8',
  '2', '6', '9',
  '2', '7', '0',
  '2', '7', '1',
  '2', '7', '2',
  '2', '7', '3',
  '2', '7', '4',
  '2', '7', '5',
  '2', '7', '6',
  '2', '7', '7',
  '2', '7', '8',
  '2', '7', '9',
  '2', '8', '0',
  '2', '8', '1',
  '2', '8', '2',
  '2', '8', '3',
  '2', '8', '4',
  '2', '8', '5',
  '2', '8', '6',
  '2', '8', '7',
  '2', '8', '8',
  '2', '8', '9',
  '2', '9', '0',
  '2', '9', '1',
  '2', '9', '2',
  '2', '9', '3',
  '2', '9', '4',
  '2', '9', '5',
  '2', '9', '6',
  '2', '9', '7',
  '2', '9', '8',
  '2', '9', '9',
  '3', '0', '0',
  '3', '0', '1',
  '3', '0', '2',
  '3', '0', '3',
  '3', '0', '4',
  '3', '0', '5',
  '3', '0', '6',
  '3', '0', '7',
  '3', '0', '8',
  '3', '0', '9',
  '3', '1', '0',
  '3', '1', '1',
  '3', '1', '2',
  '3', '1', '3',
  '3', '1', '4',
  '3', '1', '5',
  '3', '1', '6',
  '3', '1', '7',
  '3', '1', '8',
  '3', '1', '9',
  '3', '2', '0',
  '3', '2', '1',
  '3', '2', '2',
  '3', '2', '3',
  '3', '2', '4',
  '3', '2', '5',
  '3', '2', '6',
  '3', '2', '7',
  '3', '2', '8',
  '3', '2', '9',
  '3', '3', '0',
  '3', '3', '1',
  '3', '3', '2',
  '3', '3', '3',
  '3', '3', '4',
  '3', '3', '5',
  '3', '3', '6',
  '3', '3', '7',
  '3', '3', '8',
  '3', '3', '9',
  '3', '4', '0',
  '3', '4', '1',
  '3', '4', '2',
  '3', '4', '3',
  '3', '4', '4',
  '3', '4', '5',
  '3', '4', '6',
  '3', '4', '7',
  '3', '4', '8',
  '3', '4', '9',
  '3', '5', '0',
  '3', '5', '1',
  '3', '5', '2',
  '3', '5', '3',
  '3', '5', '4',
  '3', '5', '5',
  '3', '5', '6',
  '3', '5', '7',
  '3', '5', '8',
  '3', '5', '9',
  '3', '6', '0',
  '3', '6', '1',
  '3', '6', '2',
  '3', '6', '3',
  '3', '6', '4',
  '3', '6', '5',
  '3', '6', '6',
  '3', '6', '7',
  '3', '6', '8',
  '3', '6', '9',
  '3', '7', '0',
  '3', '7', '1',
  '3', '7', '2',
  '3', '7', '3',
  '3', '7', '4',
  '3', '7', '5',
  '3', '7', '6',
  '3', '7', '7',
  '3', '7', '8',
  '3', '7', '9',
  '3', '8', '0',
  '3', '8', '1',
  '3', '8', '2',
  '3', '8', '3',
  '3', '8', '4',
  '3', '8', '5',
  '3', '8', '6',
  '3', '8', '7',
  '3', '8', '8',
  '3', '8', '9',
  '3', '9', '0',
  '3', '9', '1',
  '3', '9', '2',
  '3', '9', '3',
  '3', '9', '4',
  '3', '9', '5',
  '3', '9', '6',
  '3', '9', '7',
  '3', '9', '8',
  '3', '9', '9',
  '4', '0', '0',
  '4', '0', '1',
  '4', '0', '2',
  '4', '0', '3',
  '4', '0', '4',
  '4', '0', '5',
  '4', '0', '6',
  '4', '0', '7',
  '4', '0', '8',
  '4', '0', '9',
  '4', '1', '0',
  '4', '1', '1',
  '4', '1', '2',
  '4', '1', '3',
  '4', '1', '4',
  '4', '1', '5',
  '4', '1', '6',
  '4', '1', '7',
  '4', '1', '8',
  '4', '1', '9',
  '4', '2', '0',
  '4', '2', '1',
  '4', '2', '2',
  '4', '2', '3',
  '4', '2', '4',
  '4', '2', '5',
  '4', '2', '6',
  '4', '2', '7',
  '4', '2', '8',
  '4', '2', '9',
  '4', '3', '0',
  '4', '3', '1',
  '4', '3', '2',
  '4', '3', '3',
  '4', '3', '4',
  '4', '3', '5',
  '4', '3', '6',
  '4', '3', '7',
  '4', '3', '8',
  '4', '3', '9',
  '4', '4', '0',
  '4', '4', '1',
  '4', '4', '2',
  '4', '4', '3',
  '4', '4', '4',
  '4', '4', '5',
  '4', '4', '6',
  '4', '4', '7',
  '4', '4', '8',
  '4', '4', '9',
  '4', '5', '0',
  '4', '5', '1',
  '4', '5', '2',
  '4', '5', '3',
  '4', '5', '4',
  '4', '5', '5',
  '4', '5', '6',
  '4', '5', '7',
  '4', '5', '8',
  '4', '5', '9',
  '4', '6', '0',
  '4', '6', '1',
  '4', '6', '2',
  '4', '6', '3',
  '4', '6', '4',
  '4', '6', '5',
  '4', '6', '6',
  '4', '6', '7',
  '4', '6', '8',
  '4', '6', '9',
  '4', '7', '0',
  '4', '7', '1',
  '4', '7', '2',
  '4', '7', '3',
  '4', '7', '4',
  '4', '7', '5',
  '4', '7', '6',
  '4', '7', '7',
  '4', '7', '8',
  '4', '7', '9',
  '4', '8', '0',
  '4', '8', '1',
  '4', '8', '2',
  '4', '8', '3',
  '4', '8', '4',
  '4', '8', '5',
  '4', '8', '6',
  '4', '8', '7',
  '4', '8', '8',
  '4', '8', '9',
  '4', '9', '0',
  '4', '9', '1',
  '4', '9', '2',
  '4', '9', '3',
  '4', '9', '4',
  '4', '9', '5',
  '4', '9', '6',
  '4', '9', '7',
  '4', '9', '8',
  '4', '9', '9',
  '5', '0', '0',
  '5', '0', '1',
  '5', '0', '2',
  '5', '0', '3',
  '5', '0', '4',
  '5', '0', '5',
  '5', '0', '6',
  '5', '0', '7',
  '5', '0', '8',
  '5', '0', '9',
  '5', '1', '0',
  '5', '1', '1',
  '5', '1', '2',
  '5', '1', '3',
  '5', '1', '4',
  '5', '1', '5',
  '5', '1', '6',
  '5', '1', '7',
  '5', '1', '8',
  '5', '1', '9',
  '5', '2', '0',
  '5', '2', '1',
  '5', '2', '2',
  '5', '2', '3',
  '5', '2', '4',
  '5', '2', '5',
  '5', '2', '6',
  '5', '2', '7',
  '5', '2', '8',
  '5', '2', '9',
  '5', '3', '0',
  '5', '3', '1',
  '5', '3', '2',
  '5', '3', '3',
  '5', '3', '4',
  '5', '3', '5',
  '5', '3', '6',
  '5', '3', '7',
  '5', '3', '8',
  '5', '3', '9',
  '5', '4', '0',
  '5', '4', '1',
  '5', '4', '2',
  '5', '4', '3',
  '5', '4', '4',
  '5', '4', '5',
  '5', '4', '6',
  '5', '4', '7',
  '5', '4', '8',
  '5', '4', '9',
  '5', '5', '0',
  '5', '5', '1',
  '5', '5', '2',
  '5', '5', '3',
  '5', '5', '4',
  '5', '5', '5',
  '5', '5', '6',
  '5', '5', '7',
  '5', '5', '8',
  '5', '5', '9',
  '5', '6', '0',
  '5', '6', '1',
  '5', '6', '2',
  '5', '6', '3',
  '5', '6', '4',
  '5', '6', '5',
  '5', '6', '6',
  '5', '6', '7',
  '5', '6', '8',
  '5', '6', '9',
  '5', '7', '0',
  '5', '7', '1',
  '5', '7', '2',
  '5', '7', '3',
  '5', '7', '4',
  '5', '7', '5',
  '5', '7', '6',
  '5', '7', '7',
  '5', '7', '8',
  '5', '7', '9',
  '5', '8', '0',
  '5', '8', '1',
  '5', '8', '2',
  '5', '8', '3',
  '5', '8', '4',
  '5', '8', '5',
  '5', '8', '6',
  '5', '8', '7',
  '5', '8', '8',
  '5', '8', '9',
  '5', '9', '0',
  '5', '9', '1',
  '5', '9', '2',
  '5', '9', '3',
  '5', '9', '4',
  '5', '9', '5',
  '5', '9', '6',
  '5', '9', '7',
  '5', '9', '8',
  '5', '9', '9',
  '6', '0', '0',
  '6', '0', '1',
  '6', '0', '2',
  '6', '0', '3',
  '6', '0', '4',
  '6', '0', '5',
  '6', '0', '6',
  '6', '0', '7',
  '6', '0', '8',
  '6', '0', '9',
  '6', '1', '0',
  '6', '1', '1',
  '6', '1', '2',
  '6', '1', '3',
  '6', '1', '4',
  '6', '1', '5',
  '6', '1', '6',
  '6', '1', '7',
  '6', '1', '8',
  '6', '1', '9',
  '6', '2', '0',
  '6', '2', '1',
  '6', '2', '2',
  '6', '2', '3',
  '6', '2', '4',
  '6', '2', '5',
  '6', '2', '6',
  '6', '2', '7',
  '6', '2', '8',
  '6', '2', '9',
  '6', '3', '0',
  '6', '3', '1',
  '6', '3', '2',
  '6', '3', '3',
  '6', '3', '4',
  '6', '3', '5',
  '6', '3', '6',
  '6', '3', '7',
  '6', '3', '8',
  '6', '3', '9',
  '6', '4', '0',
  '6', '4', '1',
  '6', '4', '2',
  '6', '4', '3',
  '6', '4', '4',
  '6', '4', '5',
  '6', '4', '6',
  '6', '4', '7',
  '6', '4', '8',
  '6', '4', '9',
  '6', '5', '0',
  '6', '5', '1',
  '6', '5', '2',
  '6', '5', '3',
  '6', '5', '4',
  '6', '5', '5',
  '6', '5', '6',
  '6', '5', '7',
  '6', '5', '8',
  '6', '5', '9',
  '6', '6', '0',
  '6', '6', '1',
  '6', '6', '2',
  '6', '6', '3',
  '6', '6', '4',
  '6', '6', '5',
  '6', '6', '6',
  '6', '6', '7',
  '6', '6', '8',
  '6', '6', '9',
  '6', '7', '0',
  '6', '7', '1',
  '6', '7', '2',
  '6', '7', '3',
  '6', '7', '4',
  '6', '7', '5',
  '6', '7', '6',
  '6', '7', '7',
  '6', '7', '8',
  '6', '7', '9',
  '6', '8', '0',
  '6', '8', '1',
  '6', '8', '2',
  '6', '8', '3',
  '6', '8', '4',
  '6', '8', '5',
  '6', '8', '6',
  '6', '8', '7',
  '6', '8', '8',
  '6', '8', '9',
  '6', '9', '0',
  '6', '9', '1',
  '6', '9', '2',
  '6', '9', '3',
  '6', '9', '4',
  '6', '9', '5',
  '6', '9', '6',
  '6', '9', '7',
  '6', '9', '8',
  '6', '9', '9',
  '7', '0', '0',
  '7', '0', '1',
  '7', '0', '2',
  '7', '0', '3',
  '7', '0', '4',
  '7', '0', '5',
  '7', '0', '6',
  '7', '0', '7',
  '7', '0', '8',
  '7', '0', '9',
  '7', '1', '0',
  '7', '1', '1',
  '7', '1', '2',
  '7', '1', '3',
  '7', '1', '4',
  '7', '1', '5',
  '7', '1', '6',
  '7', '1', '7',
  '7', '1', '8',
  '7', '1', '9',
  '7', '2', '0',
  '7', '2', '1',
  '7', '2', '2',
  '7', '2', '3',
  '7', '2', '4',
  '7', '2', '5',
  '7', '2', '6',
  '7', '2', '7',
  '7', '2', '8',
  '7', '2', '9',
  '7', '3', '0',
  '7', '3', '1',
  '7', '3', '2',
  '7', '3', '3',
  '7', '3', '4',
  '7', '3', '5',
  '7', '3', '6',
  '7', '3', '7',
  '7', '3', '8',
  '7', '3', '9',
  '7', '4', '0',
  '7', '4', '1',
  '7', '4', '2',
  '7', '4', '3',
  '7', '4', '4',
  '7', '4', '5',
  '7', '4', '6',
  '7', '4', '7',
  '7', '4', '8',
  '7', '4', '9',
  '7', '5', '0',
  '7', '5', '1',
  '7', '5', '2',
  '7', '5', '3',
  '7', '5', '4',
  '7', '5', '5',
  '7', '5', '6',
  '7', '5', '7',
  '7', '5', '8',
  '7', '5', '9',
  '7', '6', '0',
  '7', '6', '1',
  '7', '6', '2',
  '7', '6', '3',
  '7', '6', '4',
  '7', '6', '5',
  '7', '6', '6',
  '7', '6', '7',
  '7', '6', '8',
  '7', '6', '9',
  '7', '7', '0',
  '7', '7', '1',
  '7', '7', '2',
  '7', '7', '3',
  '7', '7', '4',
  '7', '7', '5',
  '7', '7', '6',
  '7', '7', '7',
  '7', '7', '8',
  '7', '7', '9',
  '7', '8', '0',
  '7', '8', '1',
  '7', '8', '2',
  '7', '8', '3',
  '7', '8', '4',
  '7', '8', '5',
  '7', '8', '6',
  '7', '8', '7',
  '7', '8', '8',
  '7', '8', '9',
  '7', '9', '0',
  '7', '9', '1',
  '7', '9', '2',
  '7', '9', '3',
  '7', '9', '4',
  '7', '9', '5',
  '7', '9', '6',
  '7', '9', '7',
  '7', '9', '8',
  '7', '9', '9',
  '8', '0', '0',
  '8', '0', '1',
  '8', '0', '2',
  '8', '0', '3',
  '8', '0', '4',
  '8', '0', '5',
  '8', '0', '6',
  '8', '0', '7',
  '8', '0', '8',
  '8', '0', '9',
  '8', '1', '0',
  '8', '1', '1',
  '8', '1', '2',
  '8', '1', '3',
  '8', '1', '4',
  '8', '1', '5',
  '8', '1', '6',
  '8', '1', '7',
  '8', '1', '8',
  '8', '1', '9',
  '8', '2', '0',
  '8', '2', '1',
  '8', '2', '2',
  '8', '2', '3',
  '8', '2', '4',
  '8', '2', '5',
  '8', '2', '6',
  '8', '2', '7',
  '8', '2', '8',
  '8', '2', '9',
  '8', '3', '0',
  '8', '3', '1',
  '8', '3', '2',
  '8', '3', '3',
  '8', '3', '4',
  '8', '3', '5',
  '8', '3', '6',
  '8', '3', '7',
  '8', '3', '8',
  '8', '3', '9',
  '8', '4', '0',
  '8', '4', '1',
  '8', '4', '2',
  '8', '4', '3',
  '8', '4', '4',
  '8', '4', '5',
  '8', '4', '6',
  '8', '4', '7',
  '8', '4', '8',
  '8', '4', '9',
  '8', '5', '0',
  '8', '5', '1',
  '8', '5', '2',
  '8', '5', '3',
  '8', '5', '4',
  '8', '5', '5',
  '8', '5', '6',
  '8', '5', '7',
  '8', '5', '8',
  '8', '5', '9',
  '8', '6', '0',
  '8', '6', '1',
  '8', '6', '2',
  '8', '6', '3',
  '8', '6', '4',
  '8', '6', '5',
  '8', '6', '6',
  '8', '6', '7',
  '8', '6', '8',
  '8', '6', '9',
  '8', '7', '0',
  '8', '7', '1',
  '8', '7', '2',
  '8', '7', '3',
  '8', '7', '4',
  '8', '7', '5',
  '8', '7', '6',
  '8', '7', '7',
  '8', '7', '8',
  '8', '7', '9',
  '8', '8', '0',
  '8', '8', '1',
  '8', '8', '2',
  '8', '8', '3',
  '8', '8', '4',
  '8', '8', '5',
  '8', '8', '6',
  '8', '8', '7',
  '8', '8', '8',
  '8', '8', '9',
  '8', '9', '0',
  '8', '9', '1',
  '8', '9', '2',
  '8', '9', '3',
  '8', '9', '4',
  '8', '9', '5',
  '8', '9', '6',
  '8', '9', '7',
  '8', '9', '8',
  '8', '9', '9',
  '9', '0', '0',
  '9', '0', '1',
  '9', '0', '2',
  '9', '0', '3',
  '9', '0', '4',
  '9', '0', '5',
  '9', '0', '6',
  '9', '0', '7',
  '9', '0', '8',
  '9', '0', '9',
  '9', '1', '0',
  '9', '1', '1',
  '9', '1', '2',
  '9', '1', '3',
  '9', '1', '4',
  '9', '1', '5',
  '9', '1', '6',
  '9', '1', '7',
  '9', '1', '8',
  '9', '1', '9',
  '9', '2', '0',
  '9', '2', '1',
  '9', '2', '2',
  '9', '2', '3',
  '9', '2', '4',
  '9', '2', '5',
  '9', '2', '6',
  '9', '2', '7',
  '9', '2', '8',
  '9', '2', '9',
  '9', '3', '0',
  '9', '3', '1',
  '9', '3', '2',
  '9', '3', '3',
  '9', '3', '4',
  '9', '3', '5',
  '9', '3', '6',
  '9', '3', '7',
  '9', '3', '8',
  '9', '3', '9',
  '9', '4', '0',
  '9', '4', '1',
  '9', '4', '2',
  '9', '4', '3',
  '9', '4', '4',
  '9', '4', '5',
  '9', '4', '6',
  '9', '4', '7',
  '9', '4', '8',
  '9', '4', '9',
  '9', '5', '0',
  '9', '5', '1',
  '9', '5', '2',
  '9', '5', '3',
  '9', '5', '4',
  '9', '5', '5',
  '9', '5', '6',
  '9', '5', '7',
  '9', '5', '8',
  '9', '5', '9',
  '9', '6', '0',
  '9', '6', '1',
  '9', '6', '2',
  '9', '6', '3',
  '9', '6', '4',
  '9', '6', '5',
  '9', '6', '6',
  '9', '6', '7',
  '9', '6', '8',
  '9', '6', '9',
  '9', '7', '0',
  '9', '7', '1',
  '9', '7', '2',
  '9', '7', '3',
  '9', '7', '4',
  '9', '7', '5',
  '9', '7', '6',
  '9', '7', '7',
  '9', '7', '8',
  '9', '7', '9',
  '9', '8', '0',
  '9', '8', '1',
  '9', '8', '2',
  '9', '8', '3',
  '9', '8', '4',
  '9', '8', '5',
  '9', '8', '6',
  '9', '8', '7',
  '9', '8', '8',
  '9', '8', '9',
  '9', '9', '0',
  '9', '9', '1',
  '9', '9', '2',
  '9', '9', '3',
  '9', '9', '4',
  '9', '9', '5',
  '9', '9', '6',
  '9', '9', '7',
  '9', '9', '8',
  '9', '9', '9'
};

// bid_ten2m3k64[], bid_shift_ten2m3k64[] used for conversion from BID128 to string
BID_UINT64 bid_ten2m3k64[] = {
  0x4189374bc6a7ef9eull,	// 4189374bc6a7ef9e * 2^-72 = (10^-3)RP,63
  0x10c6f7a0b5ed8d37ull,	// 10c6f7a0b5ed8d37 * 2^-80 = (10^-6)RP,61
  0x44b82fa09b5a52ccull,	// 44b82fa09b5a52cc * 2^-92 = (10^-9)RP,63
  0x119799812dea111aull,	// 119799812dea111a * 2^-100 = (10^-12)RP,61
  0x480ebe7b9d58566dull	// 480ebe7b9d58566d * 2^-112 = (10^-15)RP,63
};

unsigned int bid_shift_ten2m3k64[] = {
  8,	// 72 - 64
  16,	// 80 - 64
  28,	// 92 - 64
  36,	// 100 - 64
  48	// 112 - 64
};

BID_UINT128 bid_ten2m3k128[] = {
  {{0xb22d0e5604189375ull, 0x4189374bc6a7ef9dull}},
  // 4189374bc6a7ef9d  b22d0e5604189375  * 2^-136 = (10^-3)RP,127
  {{0xb4c7f34938583622ull, 0x10c6f7a0b5ed8d36ull}},
  // 10c6f7a0b5ed8d36  b4c7f34938583622  * 2^-144 = (10^-6)RP,125
  {{0x98b405447c4a9819ull, 0x44b82fa09b5a52cbull}},
  // 44b82fa09b5a52cb  98b405447c4a9819  * 2^-156 = (10^-9)RP,127
  {{0x7f27f0f6e885c8bbull, 0x119799812dea1119ull}},
  // 119799812dea1119  7f27f0f6e885c8bb  * 2^-164 = (10^-12)RP,125
  {{0x87ce9b80a5fb0509ull, 0x480ebe7b9d58566cull}},
  // 480ebe7b9d58566c  87ce9b80a5fb0509  * 2^-176 = (10^-15)RP,127
  {{0xe75fe645cc4873faull, 0x12725dd1d243aba0ull}},
  // 12725dd1d243aba0  e75fe645cc4873fa  * 2^-184 = (10^-18)RP,125
  {{0x69fb7e0b75e52f02ull, 0x4b8ed0283a6d3df7ull}},
  // 4b8ed0283a6d3df7  69fb7e0b75e52f02  * 2^-196 = (10^-21)RP,127
  {{0x58924d52ce4f26a9ull, 0x1357c299a88ea76aull}},
  // 1357c299a88ea76a  58924d52ce4f26a9  * 2^-204 = (10^-24)RP,125
  {{0x3baf513267aa9a3full, 0x4f3a68dbc8f03f24ull}},
  // 4f3a68dbc8f03f24  3baf513267aa9a3f  * 2^-216 = (10^-27)RP,127
  {{0x3424b06f3529a052ull, 0x14484bfeebc29f86ull}},
  // 14484bfeebc29f86  3424b06f3529a052  * 2^-224 = (10^-30)RP,125
  {{0xf658d6c57566eac8ull, 0x5313a5dee87d6eb0ull}}
  // 5313a5dee87d6eb0  f658d6c57566eac8  * 2^-236 = (10^-33)RP,127
};

unsigned int bid_shift_ten2m3k128[] = {
  8,	// 136 - 128
  16,	// 144 - 128
  28,	// 156 - 128
  36,	// 164 - 128
  48,	// 176 - 128
  56,	// 184 - 128
  4,	// 196 - 192
  12,	// 204 - 192
  24,	// 216 - 192
  32,	// 224 - 192
  44	// 236 - 192
};


/***************************************************************************
 *************** TABLES FOR GENERAL ROUNDING FUNCTIONS *********************
 ***************************************************************************/
// Note: not all entries in these tables will be used with IEEE 754 decimal
// floating-point arithmetic
// a) In round128_2_18() numbers with 2 <= q <= 18 will be rounded only
//    for 1 <= x <= 3:
//     x = 1 or x = 2 when q = 17
//     x = 2 or x = 3 when q = 18
// b) In bid_round128_19_38() numbers with 19 <= q <= 38 will be rounded only
//    for 1 <= x <= 23:
//     x = 3 or x = 4 when q = 19
//     x = 4 or x = 5 when q = 20
//     ...
//     x = 18 or x = 19 when q = 34
//     x = 1 or x = 2 or x = 19 or x = 20 when q = 35
//     x = 2 or x = 3 or x = 20 or x = 21 when q = 36
//     x = 3 or x = 4 or x = 21 or x = 22 when q = 37
//     x = 4 or x = 5 or x = 22 or x = 23 when q = 38
// c) ...
// However, for generality and possible uses outside the frame of IEEE 754
// this implementation includes table values for all x in [1, q - 1]

// Note: 64-bit tables generated with ten2mx64.ma; output in ten2mx64.out

// Kx from 10^(-x) ~= Kx * 2^(-Ex); Kx rounded up to 64 bits, 1 <= x <= 17
BID_UINT64 bid_Kx64[] = {
  0xcccccccccccccccdULL,	// 10^-1 ~= cccccccccccccccd * 2^-67
  0xa3d70a3d70a3d70bULL,	// 10^-2 ~= a3d70a3d70a3d70b * 2^-70
  0x83126e978d4fdf3cULL,	// 10^-3 ~= 83126e978d4fdf3c * 2^-73
  0xd1b71758e219652cULL,	// 10^-4 ~= d1b71758e219652c * 2^-77
  0xa7c5ac471b478424ULL,	// 10^-5 ~= a7c5ac471b478424 * 2^-80
  0x8637bd05af6c69b6ULL,	// 10^-6 ~= 8637bd05af6c69b6 * 2^-83
  0xd6bf94d5e57a42bdULL,	// 10^-7 ~= d6bf94d5e57a42bd * 2^-87
  0xabcc77118461cefdULL,	// 10^-8 ~= abcc77118461cefd * 2^-90
  0x89705f4136b4a598ULL,	// 10^-9 ~= 89705f4136b4a598 * 2^-93
  0xdbe6fecebdedd5bfULL,	// 10^-10 ~= dbe6fecebdedd5bf * 2^-97
  0xafebff0bcb24aaffULL,	// 10^-11 ~= afebff0bcb24aaff * 2^-100
  0x8cbccc096f5088ccULL,	// 10^-12 ~= 8cbccc096f5088cc * 2^-103
  0xe12e13424bb40e14ULL,	// 10^-13 ~= e12e13424bb40e14 * 2^-107
  0xb424dc35095cd810ULL,	// 10^-14 ~= b424dc35095cd810 * 2^-110
  0x901d7cf73ab0acdaULL,	// 10^-15 ~= 901d7cf73ab0acda * 2^-113
  0xe69594bec44de15cULL,	// 10^-16 ~= e69594bec44de15c * 2^-117
  0xb877aa3236a4b44aULL	// 10^-17 ~= b877aa3236a4b44a * 2^-120
};

// Ex-64 from 10^(-x) ~= Kx * 2^(-Ex); Kx rounded up to 64 bits, 1 <= x <= 17
unsigned int bid_Ex64m64[] = {
  3,	// 67 - 64, Ex = 67
  6,	// 70 - 64, Ex = 70
  9,	// 73 - 64, Ex = 73
  13,	// 77 - 64, Ex = 77
  16,	// 80 - 64, Ex = 80
  19,	// 83 - 64, Ex = 83
  23,	// 87 - 64, Ex = 87
  26,	// 90 - 64, Ex = 90
  29,	// 93 - 64, Ex = 93
  33,	// 97 - 64, Ex = 97
  36,	// 100 - 64, Ex = 100
  39,	// 103 - 64, Ex = 103
  43,	// 107 - 64, Ex = 107
  46,	// 110 - 64, Ex = 110
  49,	// 113 - 64, Ex = 113
  53,	// 117 - 64, Ex = 117
  56	// 120 - 64, Ex = 120
};

// Values of 1/2 in the right position to be compared with the fraction from
// C * kx, 1 <= x <= 17; the fraction consists of the low Ex bits in C * kx
// (these values are aligned with the high 64 bits of the fraction)
BID_UINT64 bid_half64[] = {
  0x0000000000000004ULL,	// half / 2^64 = 4
  0x0000000000000020ULL,	// half / 2^64 = 20
  0x0000000000000100ULL,	// half / 2^64 = 100
  0x0000000000001000ULL,	// half / 2^64 = 1000
  0x0000000000008000ULL,	// half / 2^64 = 8000
  0x0000000000040000ULL,	// half / 2^64 = 40000
  0x0000000000400000ULL,	// half / 2^64 = 400000
  0x0000000002000000ULL,	// half / 2^64 = 2000000
  0x0000000010000000ULL,	// half / 2^64 = 10000000
  0x0000000100000000ULL,	// half / 2^64 = 100000000
  0x0000000800000000ULL,	// half / 2^64 = 800000000
  0x0000004000000000ULL,	// half / 2^64 = 4000000000
  0x0000040000000000ULL,	// half / 2^64 = 40000000000
  0x0000200000000000ULL,	// half / 2^64 = 200000000000
  0x0001000000000000ULL,	// half / 2^64 = 1000000000000
  0x0010000000000000ULL,	// half / 2^64 = 10000000000000
  0x0080000000000000ULL	// half / 2^64 = 80000000000000
};

// Values of mask in the right position to obtain the high Ex - 64 bits
// of the fraction from C * kx, 1 <= x <= 17; the fraction consists of
// the low Ex bits in C * kx
BID_UINT64 bid_mask64[] = {
  0x0000000000000007ULL,	// mask / 2^64
  0x000000000000003fULL,	// mask / 2^64
  0x00000000000001ffULL,	// mask / 2^64
  0x0000000000001fffULL,	// mask / 2^64
  0x000000000000ffffULL,	// mask / 2^64
  0x000000000007ffffULL,	// mask / 2^64
  0x00000000007fffffULL,	// mask / 2^64
  0x0000000003ffffffULL,	// mask / 2^64
  0x000000001fffffffULL,	// mask / 2^64
  0x00000001ffffffffULL,	// mask / 2^64
  0x0000000fffffffffULL,	// mask / 2^64
  0x0000007fffffffffULL,	// mask / 2^64
  0x000007ffffffffffULL,	// mask / 2^64
  0x00003fffffffffffULL,	// mask / 2^64
  0x0001ffffffffffffULL,	// mask / 2^64
  0x001fffffffffffffULL,	// mask / 2^64
  0x00ffffffffffffffULL	// mask / 2^64
};

// Values of 10^(-x) trancated to Ex bits beyond the binary point, and
// in the right position to be compared with the fraction from C * kx,
// 1 <= x <= 17; the fraction consists of the low Ex bits in C * kx
// (these values are aligned with the low 64 bits of the fraction)
BID_UINT64 bid_ten2mxtrunc64[] = {
  0xccccccccccccccccULL,	// (ten2mx >> 64) = cccccccccccccccc
  0xa3d70a3d70a3d70aULL,	// (ten2mx >> 64) = a3d70a3d70a3d70a
  0x83126e978d4fdf3bULL,	// (ten2mx >> 64) = 83126e978d4fdf3b
  0xd1b71758e219652bULL,	// (ten2mx >> 64) = d1b71758e219652b
  0xa7c5ac471b478423ULL,	// (ten2mx >> 64) = a7c5ac471b478423
  0x8637bd05af6c69b5ULL,	// (ten2mx >> 64) = 8637bd05af6c69b5
  0xd6bf94d5e57a42bcULL,	// (ten2mx >> 64) = d6bf94d5e57a42bc
  0xabcc77118461cefcULL,	// (ten2mx >> 64) = abcc77118461cefc
  0x89705f4136b4a597ULL,	// (ten2mx >> 64) = 89705f4136b4a597
  0xdbe6fecebdedd5beULL,	// (ten2mx >> 64) = dbe6fecebdedd5be
  0xafebff0bcb24aafeULL,	// (ten2mx >> 64) = afebff0bcb24aafe
  0x8cbccc096f5088cbULL,	// (ten2mx >> 64) = 8cbccc096f5088cb
  0xe12e13424bb40e13ULL,	// (ten2mx >> 64) = e12e13424bb40e13
  0xb424dc35095cd80fULL,	// (ten2mx >> 64) = b424dc35095cd80f
  0x901d7cf73ab0acd9ULL,	// (ten2mx >> 64) = 901d7cf73ab0acd9
  0xe69594bec44de15bULL,	// (ten2mx >> 64) = e69594bec44de15b
  0xb877aa3236a4b449ULL	// (ten2mx >> 64) = b877aa3236a4b449
};

// Note: 128-bit tables generated with ten2mx128.ma; output in ten2mx128.out
// The order of the 64-bit components is L, H

// Kx from 10^(-x) ~= Kx * 2^(-Ex); Kx rounded up to 128 bits, 1 <= x <= 37
BID_UINT128 bid_Kx128[] = {
  {{0xcccccccccccccccdULL, 0xccccccccccccccccULL}},
  // 10^-1 ~= cccccccccccccccccccccccccccccccd * 2^-131
  {{0x3d70a3d70a3d70a4ULL, 0xa3d70a3d70a3d70aULL}},
  // 10^-2 ~= a3d70a3d70a3d70a3d70a3d70a3d70a4 * 2^-134
  {{0x645a1cac083126eaULL, 0x83126e978d4fdf3bULL}},
  // 10^-3 ~= 83126e978d4fdf3b645a1cac083126ea * 2^-137
  {{0xd3c36113404ea4a9ULL, 0xd1b71758e219652bULL}},
  // 10^-4 ~= d1b71758e219652bd3c36113404ea4a9 * 2^-141
  {{0x0fcf80dc33721d54ULL, 0xa7c5ac471b478423ULL}},
  // 10^-5 ~= a7c5ac471b4784230fcf80dc33721d54 * 2^-144
  {{0xa63f9a49c2c1b110ULL, 0x8637bd05af6c69b5ULL}},
  // 10^-6 ~= 8637bd05af6c69b5a63f9a49c2c1b110 * 2^-147
  {{0x3d32907604691b4dULL, 0xd6bf94d5e57a42bcULL}},
  // 10^-7 ~= d6bf94d5e57a42bc3d32907604691b4d * 2^-151
  {{0xfdc20d2b36ba7c3eULL, 0xabcc77118461cefcULL}},
  // 10^-8 ~= abcc77118461cefcfdc20d2b36ba7c3e * 2^-154
  {{0x31680a88f8953031ULL, 0x89705f4136b4a597ULL}},
  // 10^-9 ~= 89705f4136b4a59731680a88f8953031 * 2^-157
  {{0xb573440e5a884d1cULL, 0xdbe6fecebdedd5beULL}},
  // 10^-10 ~= dbe6fecebdedd5beb573440e5a884d1c * 2^-161
  {{0xf78f69a51539d749ULL, 0xafebff0bcb24aafeULL}},
  // 10^-11 ~= afebff0bcb24aafef78f69a51539d749 * 2^-164
  {{0xf93f87b7442e45d4ULL, 0x8cbccc096f5088cbULL}},
  // 10^-12 ~= 8cbccc096f5088cbf93f87b7442e45d4 * 2^-167
  {{0x2865a5f206b06fbaULL, 0xe12e13424bb40e13ULL}},
  // 10^-13 ~= e12e13424bb40e132865a5f206b06fba * 2^-171
  {{0x538484c19ef38c95ULL, 0xb424dc35095cd80fULL}},
  // 10^-14 ~= b424dc35095cd80f538484c19ef38c95 * 2^-174
  {{0x0f9d37014bf60a11ULL, 0x901d7cf73ab0acd9ULL}},
  // 10^-15 ~= 901d7cf73ab0acd90f9d37014bf60a11 * 2^-177
  {{0x4c2ebe687989a9b4ULL, 0xe69594bec44de15bULL}},
  // 10^-16 ~= e69594bec44de15b4c2ebe687989a9b4 * 2^-181
  {{0x09befeb9fad487c3ULL, 0xb877aa3236a4b449ULL}},
  // 10^-17 ~= b877aa3236a4b44909befeb9fad487c3 * 2^-184
  {{0x3aff322e62439fd0ULL, 0x9392ee8e921d5d07ULL}},
  // 10^-18 ~= 9392ee8e921d5d073aff322e62439fd0 * 2^-187
  {{0x2b31e9e3d06c32e6ULL, 0xec1e4a7db69561a5ULL}},
  // 10^-19 ~= ec1e4a7db69561a52b31e9e3d06c32e6 * 2^-191
  {{0x88f4bb1ca6bcf585ULL, 0xbce5086492111aeaULL}},
  // 10^-20 ~= bce5086492111aea88f4bb1ca6bcf585 * 2^-194
  {{0xd3f6fc16ebca5e04ULL, 0x971da05074da7beeULL}},
  // 10^-21 ~= 971da05074da7beed3f6fc16ebca5e04 * 2^-197
  {{0x5324c68b12dd6339ULL, 0xf1c90080baf72cb1ULL}},
  // 10^-22 ~= f1c90080baf72cb15324c68b12dd6339 * 2^-201
  {{0x75b7053c0f178294ULL, 0xc16d9a0095928a27ULL}},
  // 10^-23 ~= c16d9a0095928a2775b7053c0f178294 * 2^-204
  {{0xc4926a9672793543ULL, 0x9abe14cd44753b52ULL}},
  // 10^-24 ~= 9abe14cd44753b52c4926a9672793543 * 2^-207
  {{0x3a83ddbd83f52205ULL, 0xf79687aed3eec551ULL}},
  // 10^-25 ~= f79687aed3eec5513a83ddbd83f52205 * 2^-211
  {{0x95364afe032a819eULL, 0xc612062576589ddaULL}},
  // 10^-26 ~= c612062576589dda95364afe032a819e * 2^-214
  {{0x775ea264cf55347eULL, 0x9e74d1b791e07e48ULL}},
  // 10^-27 ~= 9e74d1b791e07e48775ea264cf55347e * 2^-217
  {{0x8bca9d6e188853fdULL, 0xfd87b5f28300ca0dULL}},
  // 10^-28 ~= fd87b5f28300ca0d8bca9d6e188853fd * 2^-221
  {{0x096ee45813a04331ULL, 0xcad2f7f5359a3b3eULL}},
  // 10^-29 ~= cad2f7f5359a3b3e096ee45813a04331 * 2^-224
  {{0xa1258379a94d028eULL, 0xa2425ff75e14fc31ULL}},
  // 10^-30 ~= a2425ff75e14fc31a1258379a94d028e * 2^-227
  {{0x80eacf948770ced8ULL, 0x81ceb32c4b43fcf4ULL}},
  // 10^-31 ~= 81ceb32c4b43fcf480eacf948770ced8 * 2^-230
  {{0x67de18eda5814af3ULL, 0xcfb11ead453994baULL}},
  // 10^-32 ~= cfb11ead453994ba67de18eda5814af3 * 2^-234
  {{0xecb1ad8aeacdd58fULL, 0xa6274bbdd0fadd61ULL}},
  // 10^-33 ~= a6274bbdd0fadd61ecb1ad8aeacdd58f * 2^-237
  {{0xbd5af13bef0b113fULL, 0x84ec3c97da624ab4ULL}},
  // 10^-34 ~= 84ec3c97da624ab4bd5af13bef0b113f * 2^-240
  {{0x955e4ec64b44e865ULL, 0xd4ad2dbfc3d07787ULL}},
  // 10^-35 ~= d4ad2dbfc3d07787955e4ec64b44e865 * 2^-244
  {{0xdde50bd1d5d0b9eaULL, 0xaa242499697392d2ULL}},
  // 10^-36 ~= aa242499697392d2dde50bd1d5d0b9ea * 2^-247
  {{0x7e50d64177da2e55ULL, 0x881cea14545c7575ULL}}
  // 10^-37 ~= 881cea14545c75757e50d64177da2e55 * 2^-250
};

// Ex-128 from 10^(-x) ~= Kx*2^(-Ex); Kx rounded up to 128 bits, 1 <= x <= 37
unsigned int bid_Ex128m128[] = {
  3,	// 131 - 128, Ex = 131
  6,	// 134 - 128, Ex = 134
  9,	// 137 - 128, Ex = 137
  13,	// 141 - 128, Ex = 141
  16,	// 144 - 128, Ex = 144
  19,	// 147 - 128, Ex = 147
  23,	// 151 - 128, Ex = 151
  26,	// 154 - 128, Ex = 154
  29,	// 157 - 128, Ex = 157
  33,	// 161 - 128, Ex = 161
  36,	// 164 - 128, Ex = 164
  39,	// 167 - 128, Ex = 167
  43,	// 171 - 128, Ex = 171
  46,	// 174 - 128, Ex = 174
  49,	// 177 - 128, Ex = 177
  53,	// 181 - 128, Ex = 181
  56,	// 184 - 128, Ex = 184
  59,	// 187 - 128, Ex = 187
  63,	// 191 - 128, Ex = 191
  2,	// 194 - 192, Ex = 194
  5,	// 197 - 192, Ex = 197
  9,	// 201 - 192, Ex = 201
  12,	// 204 - 192, Ex = 204
  15,	// 207 - 192, Ex = 207
  19,	// 211 - 192, Ex = 211
  22,	// 214 - 192, Ex = 214
  25,	// 217 - 192, Ex = 217
  29,	// 221 - 192, Ex = 221
  32,	// 224 - 192, Ex = 224
  35,	// 227 - 192, Ex = 227
  38,	// 230 - 192, Ex = 230
  42,	// 234 - 192, Ex = 234
  45,	// 237 - 192, Ex = 237
  48,	// 240 - 192, Ex = 240
  52,	// 244 - 192, Ex = 244
  55,	// 247 - 192, Ex = 247
  58	// 250 - 192, Ex = 250
};

// Values of 1/2 in the right position to be compared with the fraction from
// C * kx, 1 <= x <= 37; the fraction consists of the low Ex bits in C * kx
// (these values are aligned with the high 128 bits of the fraction)
BID_UINT64 bid_half128[] = {
  0x0000000000000004ULL,	// half / 2^128 = 4
  0x0000000000000020ULL,	// half / 2^128 = 20
  0x0000000000000100ULL,	// half / 2^128 = 100
  0x0000000000001000ULL,	// half / 2^128 = 1000
  0x0000000000008000ULL,	// half / 2^128 = 8000
  0x0000000000040000ULL,	// half / 2^128 = 40000
  0x0000000000400000ULL,	// half / 2^128 = 400000
  0x0000000002000000ULL,	// half / 2^128 = 2000000
  0x0000000010000000ULL,	// half / 2^128 = 10000000
  0x0000000100000000ULL,	// half / 2^128 = 100000000
  0x0000000800000000ULL,	// half / 2^128 = 800000000
  0x0000004000000000ULL,	// half / 2^128 = 4000000000
  0x0000040000000000ULL,	// half / 2^128 = 40000000000
  0x0000200000000000ULL,	// half / 2^128 = 200000000000
  0x0001000000000000ULL,	// half / 2^128 = 1000000000000
  0x0010000000000000ULL,	// half / 2^128 = 10000000000000
  0x0080000000000000ULL,	// half / 2^128 = 80000000000000
  0x0400000000000000ULL,	// half / 2^128 = 400000000000000
  0x4000000000000000ULL,	// half / 2^128 = 4000000000000000
  0x0000000000000002ULL,	// half / 2^192 = 2
  0x0000000000000010ULL,	// half / 2^192 = 10
  0x0000000000000100ULL,	// half / 2^192 = 100
  0x0000000000000800ULL,	// half / 2^192 = 800
  0x0000000000004000ULL,	// half / 2^192 = 4000
  0x0000000000040000ULL,	// half / 2^192 = 40000
  0x0000000000200000ULL,	// half / 2^192 = 200000
  0x0000000001000000ULL,	// half / 2^192 = 1000000
  0x0000000010000000ULL,	// half / 2^192 = 10000000
  0x0000000080000000ULL,	// half / 2^192 = 80000000
  0x0000000400000000ULL,	// half / 2^192 = 400000000
  0x0000002000000000ULL,	// half / 2^192 = 2000000000
  0x0000020000000000ULL,	// half / 2^192 = 20000000000
  0x0000100000000000ULL,	// half / 2^192 = 100000000000
  0x0000800000000000ULL,	// half / 2^192 = 800000000000
  0x0008000000000000ULL,	// half / 2^192 = 8000000000000
  0x0040000000000000ULL,	// half / 2^192 = 40000000000000
  0x0200000000000000ULL	// half / 2^192 = 200000000000000
};

// Values of mask in the right position to obtain the high Ex - 128 or Ex - 192
// bits of the fraction from C * kx, 1 <= x <= 37; the fraction consists of
// the low Ex bits in C * kx
BID_UINT64 bid_mask128[] = {
  0x0000000000000007ULL,	// mask / 2^128
  0x000000000000003fULL,	// mask / 2^128
  0x00000000000001ffULL,	// mask / 2^128
  0x0000000000001fffULL,	// mask / 2^128
  0x000000000000ffffULL,	// mask / 2^128
  0x000000000007ffffULL,	// mask / 2^128
  0x00000000007fffffULL,	// mask / 2^128
  0x0000000003ffffffULL,	// mask / 2^128
  0x000000001fffffffULL,	// mask / 2^128
  0x00000001ffffffffULL,	// mask / 2^128
  0x0000000fffffffffULL,	// mask / 2^128
  0x0000007fffffffffULL,	// mask / 2^128
  0x000007ffffffffffULL,	// mask / 2^128
  0x00003fffffffffffULL,	// mask / 2^128
  0x0001ffffffffffffULL,	// mask / 2^128
  0x001fffffffffffffULL,	// mask / 2^128
  0x00ffffffffffffffULL,	// mask / 2^128
  0x07ffffffffffffffULL,	// mask / 2^128
  0x7fffffffffffffffULL,	// mask / 2^128
  0x0000000000000003ULL,	// mask / 2^192
  0x000000000000001fULL,	// mask / 2^192
  0x00000000000001ffULL,	// mask / 2^192
  0x0000000000000fffULL,	// mask / 2^192
  0x0000000000007fffULL,	// mask / 2^192
  0x000000000007ffffULL,	// mask / 2^192
  0x00000000003fffffULL,	// mask / 2^192
  0x0000000001ffffffULL,	// mask / 2^192
  0x000000001fffffffULL,	// mask / 2^192
  0x00000000ffffffffULL,	// mask / 2^192
  0x00000007ffffffffULL,	// mask / 2^192
  0x0000003fffffffffULL,	// mask / 2^192
  0x000003ffffffffffULL,	// mask / 2^192
  0x00001fffffffffffULL,	// mask / 2^192
  0x0000ffffffffffffULL,	// mask / 2^192
  0x000fffffffffffffULL,	// mask / 2^192
  0x007fffffffffffffULL,	// mask / 2^192
  0x03ffffffffffffffULL	// mask / 2^192
};

// Values of 10^(-x) trancated to Ex bits beyond the binary point, and
// in the right position to be compared with the fraction from C * kx,
// 1 <= x <= 37; the fraction consists of the low Ex bits in C * kx
// (these values are aligned with the low 128 bits of the fraction)
BID_UINT128 bid_ten2mxtrunc128[] = {
  {{0xccccccccccccccccULL, 0xccccccccccccccccULL}},
  // (ten2mx >> 128) = cccccccccccccccccccccccccccccccc
  {{0x3d70a3d70a3d70a3ULL, 0xa3d70a3d70a3d70aULL}},
  // (ten2mx >> 128) = a3d70a3d70a3d70a3d70a3d70a3d70a3
  {{0x645a1cac083126e9ULL, 0x83126e978d4fdf3bULL}},
  // (ten2mx >> 128) = 83126e978d4fdf3b645a1cac083126e9
  {{0xd3c36113404ea4a8ULL, 0xd1b71758e219652bULL}},
  // (ten2mx >> 128) = d1b71758e219652bd3c36113404ea4a8
  {{0x0fcf80dc33721d53ULL, 0xa7c5ac471b478423ULL}},
  // (ten2mx >> 128) = a7c5ac471b4784230fcf80dc33721d53
  {{0xa63f9a49c2c1b10fULL, 0x8637bd05af6c69b5ULL}},
  // (ten2mx >> 128) = 8637bd05af6c69b5a63f9a49c2c1b10f
  {{0x3d32907604691b4cULL, 0xd6bf94d5e57a42bcULL}},
  // (ten2mx >> 128) = d6bf94d5e57a42bc3d32907604691b4c
  {{0xfdc20d2b36ba7c3dULL, 0xabcc77118461cefcULL}},
  // (ten2mx >> 128) = abcc77118461cefcfdc20d2b36ba7c3d
  {{0x31680a88f8953030ULL, 0x89705f4136b4a597ULL}},
  // (ten2mx >> 128) = 89705f4136b4a59731680a88f8953030
  {{0xb573440e5a884d1bULL, 0xdbe6fecebdedd5beULL}},
  // (ten2mx >> 128) = dbe6fecebdedd5beb573440e5a884d1b
  {{0xf78f69a51539d748ULL, 0xafebff0bcb24aafeULL}},
  // (ten2mx >> 128) = afebff0bcb24aafef78f69a51539d748
  {{0xf93f87b7442e45d3ULL, 0x8cbccc096f5088cbULL}},
  // (ten2mx >> 128) = 8cbccc096f5088cbf93f87b7442e45d3
  {{0x2865a5f206b06fb9ULL, 0xe12e13424bb40e13ULL}},
  // (ten2mx >> 128) = e12e13424bb40e132865a5f206b06fb9
  {{0x538484c19ef38c94ULL, 0xb424dc35095cd80fULL}},
  // (ten2mx >> 128) = b424dc35095cd80f538484c19ef38c94
  {{0x0f9d37014bf60a10ULL, 0x901d7cf73ab0acd9ULL}},
  // (ten2mx >> 128) = 901d7cf73ab0acd90f9d37014bf60a10
  {{0x4c2ebe687989a9b3ULL, 0xe69594bec44de15bULL}},
  // (ten2mx >> 128) = e69594bec44de15b4c2ebe687989a9b3
  {{0x09befeb9fad487c2ULL, 0xb877aa3236a4b449ULL}},
  // (ten2mx >> 128) = b877aa3236a4b44909befeb9fad487c2
  {{0x3aff322e62439fcfULL, 0x9392ee8e921d5d07ULL}},
  // (ten2mx >> 128) = 9392ee8e921d5d073aff322e62439fcf
  {{0x2b31e9e3d06c32e5ULL, 0xec1e4a7db69561a5ULL}},
  // (ten2mx >> 128) = ec1e4a7db69561a52b31e9e3d06c32e5
  {{0x88f4bb1ca6bcf584ULL, 0xbce5086492111aeaULL}},
  // (ten2mx >> 128) = bce5086492111aea88f4bb1ca6bcf584
  {{0xd3f6fc16ebca5e03ULL, 0x971da05074da7beeULL}},
  // (ten2mx >> 128) = 971da05074da7beed3f6fc16ebca5e03
  {{0x5324c68b12dd6338ULL, 0xf1c90080baf72cb1ULL}},
  // (ten2mx >> 128) = f1c90080baf72cb15324c68b12dd6338
  {{0x75b7053c0f178293ULL, 0xc16d9a0095928a27ULL}},
  // (ten2mx >> 128) = c16d9a0095928a2775b7053c0f178293
  {{0xc4926a9672793542ULL, 0x9abe14cd44753b52ULL}},
  // (ten2mx >> 128) = 9abe14cd44753b52c4926a9672793542
  {{0x3a83ddbd83f52204ULL, 0xf79687aed3eec551ULL}},
  // (ten2mx >> 128) = f79687aed3eec5513a83ddbd83f52204
  {{0x95364afe032a819dULL, 0xc612062576589ddaULL}},
  // (ten2mx >> 128) = c612062576589dda95364afe032a819d
  {{0x775ea264cf55347dULL, 0x9e74d1b791e07e48ULL}},
  // (ten2mx >> 128) = 9e74d1b791e07e48775ea264cf55347d
  {{0x8bca9d6e188853fcULL, 0xfd87b5f28300ca0dULL}},
  // (ten2mx >> 128) = fd87b5f28300ca0d8bca9d6e188853fc
  {{0x096ee45813a04330ULL, 0xcad2f7f5359a3b3eULL}},
  // (ten2mx >> 128) = cad2f7f5359a3b3e096ee45813a04330
  {{0xa1258379a94d028dULL, 0xa2425ff75e14fc31ULL}},
  // (ten2mx >> 128) = a2425ff75e14fc31a1258379a94d028d
  {{0x80eacf948770ced7ULL, 0x81ceb32c4b43fcf4ULL}},
  // (ten2mx >> 128) = 81ceb32c4b43fcf480eacf948770ced7
  {{0x67de18eda5814af2ULL, 0xcfb11ead453994baULL}},
  // (ten2mx >> 128) = cfb11ead453994ba67de18eda5814af2
  {{0xecb1ad8aeacdd58eULL, 0xa6274bbdd0fadd61ULL}},
  // (ten2mx >> 128) = a6274bbdd0fadd61ecb1ad8aeacdd58e
  {{0xbd5af13bef0b113eULL, 0x84ec3c97da624ab4ULL}},
  // (ten2mx >> 128) = 84ec3c97da624ab4bd5af13bef0b113e
  {{0x955e4ec64b44e864ULL, 0xd4ad2dbfc3d07787ULL}},
  // (ten2mx >> 128) = d4ad2dbfc3d07787955e4ec64b44e864
  {{0xdde50bd1d5d0b9e9ULL, 0xaa242499697392d2ULL}},
  // (ten2mx >> 128) = aa242499697392d2dde50bd1d5d0b9e9
  {{0x7e50d64177da2e54ULL, 0x881cea14545c7575ULL}}
  // (ten2mx >> 128) = 881cea14545c75757e50d64177da2e54
};

BID_UINT192 bid_Kx192[] = {
  {{0xcccccccccccccccdULL, 0xccccccccccccccccULL,
    0xccccccccccccccccULL}},
  // 10^-1 ~= cccccccccccccccccccccccccccccccccccccccccccccccd * 2^-195
  {{0xd70a3d70a3d70a3eULL, 0x3d70a3d70a3d70a3ULL,
    0xa3d70a3d70a3d70aULL}},
  // 10^-2 ~= a3d70a3d70a3d70a3d70a3d70a3d70a3d70a3d70a3d70a3e * 2^-198
  {{0x78d4fdf3b645a1cbULL, 0x645a1cac083126e9ULL,
    0x83126e978d4fdf3bULL}},
  // 10^-3 ~= 83126e978d4fdf3b645a1cac083126e978d4fdf3b645a1cb * 2^-201
  {{0xc154c985f06f6945ULL, 0xd3c36113404ea4a8ULL,
    0xd1b71758e219652bULL}},
  // 10^-4 ~= d1b71758e219652bd3c36113404ea4a8c154c985f06f6945 * 2^-205
  {{0xcddd6e04c0592104ULL, 0x0fcf80dc33721d53ULL,
    0xa7c5ac471b478423ULL}},
  // 10^-5 ~= a7c5ac471b4784230fcf80dc33721d53cddd6e04c0592104 * 2^-208
  {{0xd7e45803cd141a6aULL, 0xa63f9a49c2c1b10fULL,
    0x8637bd05af6c69b5ULL}},
  // 10^-6 ~= 8637bd05af6c69b5a63f9a49c2c1b10fd7e45803cd141a6a * 2^-211
  {{0x8ca08cd2e1b9c3dcULL, 0x3d32907604691b4cULL,
    0xd6bf94d5e57a42bcULL}},
  // 10^-7 ~= d6bf94d5e57a42bc3d32907604691b4c8ca08cd2e1b9c3dc * 2^-215
  {{0x3d4d3d758161697dULL, 0xfdc20d2b36ba7c3dULL,
    0xabcc77118461cefcULL}},
  // 10^-8 ~= abcc77118461cefcfdc20d2b36ba7c3d3d4d3d758161697d * 2^-218
  {{0xfdd7645e011abacaULL, 0x31680a88f8953030ULL,
    0x89705f4136b4a597ULL}},
  // 10^-9 ~= 89705f4136b4a59731680a88f8953030fdd7645e011abaca * 2^-221
  {{0x2fbf06fcce912addULL, 0xb573440e5a884d1bULL,
    0xdbe6fecebdedd5beULL}},
  // 10^-10 ~= dbe6fecebdedd5beb573440e5a884d1b2fbf06fcce912add * 2^-225
  {{0xf2ff38ca3eda88b1ULL, 0xf78f69a51539d748ULL,
    0xafebff0bcb24aafeULL}},
  // 10^-11 ~= afebff0bcb24aafef78f69a51539d748f2ff38ca3eda88b1 * 2^-228
  {{0xf598fa3b657ba08eULL, 0xf93f87b7442e45d3ULL,
    0x8cbccc096f5088cbULL}},
  // 10^-12 ~= 8cbccc096f5088cbf93f87b7442e45d3f598fa3b657ba08e * 2^-231
  {{0x88f4c3923bf900e3ULL, 0x2865a5f206b06fb9ULL,
    0xe12e13424bb40e13ULL}},
  // 10^-13 ~= e12e13424bb40e132865a5f206b06fb988f4c3923bf900e3 * 2^-235
  {{0x6d909c74fcc733e9ULL, 0x538484c19ef38c94ULL,
    0xb424dc35095cd80fULL}},
  // 10^-14 ~= b424dc35095cd80f538484c19ef38c946d909c74fcc733e9 * 2^-238
  {{0x57a6e390ca38f654ULL, 0x0f9d37014bf60a10ULL,
    0x901d7cf73ab0acd9ULL}},
  // 10^-15 ~= 901d7cf73ab0acd90f9d37014bf60a1057a6e390ca38f654 * 2^-241
  {{0xbf716c1add27f086ULL, 0x4c2ebe687989a9b3ULL,
    0xe69594bec44de15bULL}},
  // 10^-16 ~= e69594bec44de15b4c2ebe687989a9b3bf716c1add27f086 * 2^-245
  {{0xff8df0157db98d38ULL, 0x09befeb9fad487c2ULL,
    0xb877aa3236a4b449ULL}},
  // 10^-17 ~= b877aa3236a4b44909befeb9fad487c2ff8df0157db98d38 * 2^-248
  {{0x32d7f344649470faULL, 0x3aff322e62439fcfULL,
    0x9392ee8e921d5d07ULL}},
  // 10^-18 ~= 9392ee8e921d5d073aff322e62439fcf32d7f344649470fa * 2^-251
  {{0x1e2652070753e7f5ULL, 0x2b31e9e3d06c32e5ULL,
    0xec1e4a7db69561a5ULL}},
  // 10^-19 ~= ec1e4a7db69561a52b31e9e3d06c32e51e2652070753e7f5 * 2^-255
  {{0x181ea8059f76532bULL, 0x88f4bb1ca6bcf584ULL,
    0xbce5086492111aeaULL}},
  // 10^-20 ~= bce5086492111aea88f4bb1ca6bcf584181ea8059f76532b * 2^-258
  {{0x467eecd14c5ea8efULL, 0xd3f6fc16ebca5e03ULL,
    0x971da05074da7beeULL}},
  // 10^-21 ~= 971da05074da7beed3f6fc16ebca5e03467eecd14c5ea8ef * 2^-261
  {{0x70cb148213caa7e5ULL, 0x5324c68b12dd6338ULL,
    0xf1c90080baf72cb1ULL}},
  // 10^-22 ~= f1c90080baf72cb15324c68b12dd633870cb148213caa7e5 * 2^-265
  {{0x8d6f439b43088651ULL, 0x75b7053c0f178293ULL,
    0xc16d9a0095928a27ULL}},
  // 10^-23 ~= c16d9a0095928a2775b7053c0f1782938d6f439b43088651 * 2^-268
  {{0xd78c3615cf3a050dULL, 0xc4926a9672793542ULL,
    0x9abe14cd44753b52ULL}},
  // 10^-24 ~= 9abe14cd44753b52c4926a9672793542d78c3615cf3a050d * 2^-271
  {{0x8c1389bc7ec33b48ULL, 0x3a83ddbd83f52204ULL,
    0xf79687aed3eec551ULL}},
  // 10^-25 ~= f79687aed3eec5513a83ddbd83f522048c1389bc7ec33b48 * 2^-275
  {{0x3cdc6e306568fc3aULL, 0x95364afe032a819dULL,
    0xc612062576589ddaULL}},
  // 10^-26 ~= c612062576589dda95364afe032a819d3cdc6e306568fc3a * 2^-278
  {{0xca49f1c05120c9c8ULL, 0x775ea264cf55347dULL,
    0x9e74d1b791e07e48ULL}},
  // 10^-27 ~= 9e74d1b791e07e48775ea264cf55347dca49f1c05120c9c8 * 2^-281
  {{0x76dcb60081ce0fa6ULL, 0x8bca9d6e188853fcULL,
    0xfd87b5f28300ca0dULL}},
  // 10^-28 ~= fd87b5f28300ca0d8bca9d6e188853fc76dcb60081ce0fa6 * 2^-285
  {{0x5f16f80067d80c85ULL, 0x096ee45813a04330ULL,
    0xcad2f7f5359a3b3eULL}},
  // 10^-29 ~= cad2f7f5359a3b3e096ee45813a043305f16f80067d80c85 * 2^-288
  {{0x18df2ccd1fe00a04ULL, 0xa1258379a94d028dULL,
    0xa2425ff75e14fc31ULL}},
  // 10^-30 ~= a2425ff75e14fc31a1258379a94d028d18df2ccd1fe00a04 * 2^-291
  {{0x4718f0a419800803ULL, 0x80eacf948770ced7ULL,
    0x81ceb32c4b43fcf4ULL}},
  // 10^-31 ~= 81ceb32c4b43fcf480eacf948770ced74718f0a419800803 * 2^-294
  {{0x0b5b1aa028ccd99fULL, 0x67de18eda5814af2ULL,
    0xcfb11ead453994baULL}},
  // 10^-32 ~= cfb11ead453994ba67de18eda5814af20b5b1aa028ccd99f * 2^-298
  {{0x6f7c154ced70ae19ULL, 0xecb1ad8aeacdd58eULL,
    0xa6274bbdd0fadd61ULL}},
  // 10^-33 ~= a6274bbdd0fadd61ecb1ad8aeacdd58e6f7c154ced70ae19 * 2^-301
  {{0xbf967770bdf3be7aULL, 0xbd5af13bef0b113eULL,
    0x84ec3c97da624ab4ULL}},
  // 10^-34 ~= 84ec3c97da624ab4bd5af13bef0b113ebf967770bdf3be7a * 2^-304
  {{0x65bd8be79652ca5dULL, 0x955e4ec64b44e864ULL,
    0xd4ad2dbfc3d07787ULL}},
  // 10^-35 ~= d4ad2dbfc3d07787955e4ec64b44e86465bd8be79652ca5d * 2^-308
  {{0xeafe098611dbd517ULL, 0xdde50bd1d5d0b9e9ULL,
    0xaa242499697392d2ULL}},
  // 10^-36 ~= aa242499697392d2dde50bd1d5d0b9e9eafe098611dbd517 * 2^-311
  {{0xbbfe6e04db164413ULL, 0x7e50d64177da2e54ULL,
    0x881cea14545c7575ULL}},
  // 10^-37 ~= 881cea14545c75757e50d64177da2e54bbfe6e04db164413 * 2^-314
  {{0x2cca49a15e8a0684ULL, 0x96e7bd358c904a21ULL,
    0xd9c7dced53c72255ULL}},
  // 10^-38 ~= d9c7dced53c7225596e7bd358c904a212cca49a15e8a0684 * 2^-318
  {{0x8a3b6e1ab2080537ULL, 0xabec975e0a0d081aULL,
    0xae397d8aa96c1b77ULL}},
  // 10^-39 ~= ae397d8aa96c1b77abec975e0a0d081a8a3b6e1ab2080537 * 2^-321
  {{0x3b62be7bc1a0042cULL, 0x2323ac4b3b3da015ULL,
    0x8b61313bbabce2c6ULL}},
  // 10^-40 ~= 8b61313bbabce2c62323ac4b3b3da0153b62be7bc1a0042c * 2^-324
  {{0x5f0463f935ccd379ULL, 0x6b6c46dec52f6688ULL,
    0xdf01e85f912e37a3ULL}},
  // 10^-41 ~= df01e85f912e37a36b6c46dec52f66885f0463f935ccd379 * 2^-328
  {{0x7f36b660f7d70f94ULL, 0x55f038b237591ed3ULL,
    0xb267ed1940f1c61cULL}},
  // 10^-42 ~= b267ed1940f1c61c55f038b237591ed37f36b660f7d70f94 * 2^-331
  {{0xcc2bc51a5fdf3faaULL, 0x77f3608e92adb242ULL,
    0x8eb98a7a9a5b04e3ULL}},
  // 10^-43 ~= 8eb98a7a9a5b04e377f3608e92adb242cc2bc51a5fdf3faa * 2^-334
  {{0xe046082a32fecc42ULL, 0x8cb89a7db77c506aULL,
    0xe45c10c42a2b3b05ULL}},
  // 10^-44 ~= e45c10c42a2b3b058cb89a7db77c506ae046082a32fecc42 * 2^-338
  {{0x4d04d354f598a368ULL, 0x3d607b97c5fd0d22ULL,
    0xb6b00d69bb55c8d1ULL}},
  // 10^-45 ~= b6b00d69bb55c8d13d607b97c5fd0d224d04d354f598a368 * 2^-341
  {{0x3d9d75dd9146e920ULL, 0xcab3961304ca70e8ULL,
    0x9226712162ab070dULL}},
  // 10^-46 ~= 9226712162ab070dcab3961304ca70e83d9d75dd9146e920 * 2^-344
  {{0xc8fbefc8e8717500ULL, 0xaab8f01e6e10b4a6ULL,
    0xe9d71b689dde71afULL}},
  // 10^-47 ~= e9d71b689dde71afaab8f01e6e10b4a6c8fbefc8e8717500 * 2^-348
  {{0x3a63263a538df734ULL, 0x5560c018580d5d52ULL,
    0xbb127c53b17ec159ULL}},
  // 10^-48 ~= bb127c53b17ec1595560c018580d5d523a63263a538df734 * 2^-351
  {{0x2eb5b82ea93e5f5dULL, 0xdde7001379a44aa8ULL,
    0x95a8637627989aadULL}},
  // 10^-49 ~= 95a8637627989aaddde7001379a44aa82eb5b82ea93e5f5d * 2^-354
  {{0x4abc59e441fd6561ULL, 0x963e66858f6d4440ULL,
    0xef73d256a5c0f77cULL}},
  // 10^-50 ~= ef73d256a5c0f77c963e66858f6d44404abc59e441fd6561 * 2^-358
  {{0x6efd14b69b311de7ULL, 0xde98520472bdd033ULL,
    0xbf8fdb78849a5f96ULL}},
  // 10^-51 ~= bf8fdb78849a5f96de98520472bdd0336efd14b69b311de7 * 2^-361
  {{0x259743c548f417ecULL, 0xe546a8038efe4029ULL,
    0x993fe2c6d07b7fabULL}},
  // 10^-52 ~= 993fe2c6d07b7fabe546a8038efe4029259743c548f417ec * 2^-364
  {{0x3c25393ba7ecf313ULL, 0xd53dd99f4b3066a8ULL,
    0xf53304714d9265dfULL}},
  // 10^-53 ~= f53304714d9265dfd53dd99f4b3066a83c25393ba7ecf313 * 2^-368
  {{0x96842dc95323f5a9ULL, 0xaa97e14c3c26b886ULL,
    0xc428d05aa4751e4cULL}},
  // 10^-54 ~= c428d05aa4751e4caa97e14c3c26b88696842dc95323f5a9 * 2^-371
  {{0xab9cf16ddc1cc487ULL, 0x55464dd69685606bULL,
    0x9ced737bb6c4183dULL}},
  // 10^-55 ~= 9ced737bb6c4183d55464dd69685606bab9cf16ddc1cc487 * 2^-374
  {{0xac2e4f162cfad40bULL, 0xeed6e2f0f0d56712ULL, 0xfb158592be068d2eULL}}
  // 10^-56 ~= fb158592be068d2eeed6e2f0f0d56712ac2e4f162cfad40b * 2^-378
};

unsigned int bid_Ex192m192[] = {
  3,	// 195 - 192, Ex = 195
  6,	// 198 - 192, Ex = 198
  9,	// 201 - 192, Ex = 201
  13,	// 205 - 192, Ex = 205
  16,	// 208 - 192, Ex = 208
  19,	// 211 - 192, Ex = 211
  23,	// 215 - 192, Ex = 215
  26,	// 218 - 192, Ex = 218
  29,	// 221 - 192, Ex = 221
  33,	// 225 - 192, Ex = 225
  36,	// 228 - 192, Ex = 228
  39,	// 231 - 192, Ex = 231
  43,	// 235 - 192, Ex = 235
  46,	// 238 - 192, Ex = 238
  49,	// 241 - 192, Ex = 241
  53,	// 245 - 192, Ex = 245
  56,	// 248 - 192, Ex = 248
  59,	// 251 - 192, Ex = 251
  63,	// 255 - 192, Ex = 255
  2,	// 258 - 256, Ex = 258
  5,	// 261 - 256, Ex = 261
  9,	// 265 - 256, Ex = 265
  12,	// 268 - 256, Ex = 268
  15,	// 271 - 256, Ex = 271
  19,	// 275 - 256, Ex = 275
  22,	// 278 - 256, Ex = 278
  25,	// 281 - 256, Ex = 281
  29,	// 285 - 256, Ex = 285
  32,	// 288 - 256, Ex = 288
  35,	// 291 - 256, Ex = 291
  38,	// 294 - 256, Ex = 294
  42,	// 298 - 256, Ex = 298
  45,	// 301 - 256, Ex = 301
  48,	// 304 - 256, Ex = 304
  52,	// 308 - 256, Ex = 308
  55,	// 311 - 256, Ex = 311
  58,	// 314 - 256, Ex = 314
  62,	// 318 - 256, Ex = 318
  1,	// 321 - 320, Ex = 321
  4,	// 324 - 320, Ex = 324
  8,	// 328 - 320, Ex = 328
  11,	// 331 - 320, Ex = 331
  14,	// 334 - 320, Ex = 334
  18,	// 338 - 320, Ex = 338
  21,	// 341 - 320, Ex = 341
  24,	// 344 - 320, Ex = 344
  28,	// 348 - 320, Ex = 348
  31,	// 351 - 320, Ex = 351
  34,	// 354 - 320, Ex = 354
  38,	// 358 - 320, Ex = 358
  41,	// 361 - 320, Ex = 361
  44,	// 364 - 320, Ex = 364
  48,	// 368 - 320, Ex = 368
  51,	// 371 - 320, Ex = 371
  54,	// 374 - 320, Ex = 374
  58	// 378 - 320, Ex = 378
};

BID_UINT64 bid_half192[] = {
  0x0000000000000004ULL,	// half / 2^192 = 4
  0x0000000000000020ULL,	// half / 2^192 = 20
  0x0000000000000100ULL,	// half / 2^192 = 100
  0x0000000000001000ULL,	// half / 2^192 = 1000
  0x0000000000008000ULL,	// half / 2^192 = 8000
  0x0000000000040000ULL,	// half / 2^192 = 40000
  0x0000000000400000ULL,	// half / 2^192 = 400000
  0x0000000002000000ULL,	// half / 2^192 = 2000000
  0x0000000010000000ULL,	// half / 2^192 = 10000000
  0x0000000100000000ULL,	// half / 2^192 = 100000000
  0x0000000800000000ULL,	// half / 2^192 = 800000000
  0x0000004000000000ULL,	// half / 2^192 = 4000000000
  0x0000040000000000ULL,	// half / 2^192 = 40000000000
  0x0000200000000000ULL,	// half / 2^192 = 200000000000
  0x0001000000000000ULL,	// half / 2^192 = 1000000000000
  0x0010000000000000ULL,	// half / 2^192 = 10000000000000
  0x0080000000000000ULL,	// half / 2^192 = 80000000000000
  0x0400000000000000ULL,	// half / 2^192 = 400000000000000
  0x4000000000000000ULL,	// half / 2^192 = 4000000000000000
  0x0000000000000002ULL,	// half / 2^256 = 2
  0x0000000000000010ULL,	// half / 2^256 = 10
  0x0000000000000100ULL,	// half / 2^256 = 100
  0x0000000000000800ULL,	// half / 2^256 = 800
  0x0000000000004000ULL,	// half / 2^256 = 4000
  0x0000000000040000ULL,	// half / 2^256 = 40000
  0x0000000000200000ULL,	// half / 2^256 = 200000
  0x0000000001000000ULL,	// half / 2^256 = 1000000
  0x0000000010000000ULL,	// half / 2^256 = 10000000
  0x0000000080000000ULL,	// half / 2^256 = 80000000
  0x0000000400000000ULL,	// half / 2^256 = 400000000
  0x0000002000000000ULL,	// half / 2^256 = 2000000000
  0x0000020000000000ULL,	// half / 2^256 = 20000000000
  0x0000100000000000ULL,	// half / 2^256 = 100000000000
  0x0000800000000000ULL,	// half / 2^256 = 800000000000
  0x0008000000000000ULL,	// half / 2^256 = 8000000000000
  0x0040000000000000ULL,	// half / 2^256 = 40000000000000
  0x0200000000000000ULL,	// half / 2^256 = 200000000000000
  0x2000000000000000ULL,	// half / 2^256 = 2000000000000000
  0x0000000000000001ULL,	// half / 2^320 = 1
  0x0000000000000008ULL,	// half / 2^320 = 8
  0x0000000000000080ULL,	// half / 2^320 = 80
  0x0000000000000400ULL,	// half / 2^320 = 400
  0x0000000000002000ULL,	// half / 2^320 = 2000
  0x0000000000020000ULL,	// half / 2^320 = 20000
  0x0000000000100000ULL,	// half / 2^320 = 100000
  0x0000000000800000ULL,	// half / 2^320 = 800000
  0x0000000008000000ULL,	// half / 2^320 = 8000000
  0x0000000040000000ULL,	// half / 2^320 = 40000000
  0x0000000200000000ULL,	// half / 2^320 = 200000000
  0x0000002000000000ULL,	// half / 2^320 = 2000000000
  0x0000010000000000ULL,	// half / 2^320 = 10000000000
  0x0000080000000000ULL,	// half / 2^320 = 80000000000
  0x0000800000000000ULL,	// half / 2^320 = 800000000000
  0x0004000000000000ULL,	// half / 2^320 = 4000000000000
  0x0020000000000000ULL,	// half / 2^320 = 20000000000000
  0x0200000000000000ULL	// half / 2^320 = 200000000000000
};

BID_UINT64 bid_mask192[] = {
  0x0000000000000007ULL,	// mask / 2^192
  0x000000000000003fULL,	// mask / 2^192
  0x00000000000001ffULL,	// mask / 2^192
  0x0000000000001fffULL,	// mask / 2^192
  0x000000000000ffffULL,	// mask / 2^192
  0x000000000007ffffULL,	// mask / 2^192
  0x00000000007fffffULL,	// mask / 2^192
  0x0000000003ffffffULL,	// mask / 2^192
  0x000000001fffffffULL,	// mask / 2^192
  0x00000001ffffffffULL,	// mask / 2^192
  0x0000000fffffffffULL,	// mask / 2^192
  0x0000007fffffffffULL,	// mask / 2^192
  0x000007ffffffffffULL,	// mask / 2^192
  0x00003fffffffffffULL,	// mask / 2^192
  0x0001ffffffffffffULL,	// mask / 2^192
  0x001fffffffffffffULL,	// mask / 2^192
  0x00ffffffffffffffULL,	// mask / 2^192
  0x07ffffffffffffffULL,	// mask / 2^192
  0x7fffffffffffffffULL,	// mask / 2^192
  0x0000000000000003ULL,	// mask / 2^256
  0x000000000000001fULL,	// mask / 2^256
  0x00000000000001ffULL,	// mask / 2^256
  0x0000000000000fffULL,	// mask / 2^256
  0x0000000000007fffULL,	// mask / 2^256
  0x000000000007ffffULL,	// mask / 2^256
  0x00000000003fffffULL,	// mask / 2^256
  0x0000000001ffffffULL,	// mask / 2^256
  0x000000001fffffffULL,	// mask / 2^256
  0x00000000ffffffffULL,	// mask / 2^256
  0x00000007ffffffffULL,	// mask / 2^256
  0x0000003fffffffffULL,	// mask / 2^256
  0x000003ffffffffffULL,	// mask / 2^256
  0x00001fffffffffffULL,	// mask / 2^256
  0x0000ffffffffffffULL,	// mask / 2^256
  0x000fffffffffffffULL,	// mask / 2^256
  0x007fffffffffffffULL,	// mask / 2^256
  0x03ffffffffffffffULL,	// mask / 2^256
  0x3fffffffffffffffULL,	// mask / 2^256
  0x0000000000000001ULL,	// mask / 2^320
  0x000000000000000fULL,	// mask / 2^320
  0x00000000000000ffULL,	// mask / 2^320
  0x00000000000007ffULL,	// mask / 2^320
  0x0000000000003fffULL,	// mask / 2^320
  0x000000000003ffffULL,	// mask / 2^320
  0x00000000001fffffULL,	// mask / 2^320
  0x0000000000ffffffULL,	// mask / 2^320
  0x000000000fffffffULL,	// mask / 2^320
  0x000000007fffffffULL,	// mask / 2^320
  0x00000003ffffffffULL,	// mask / 2^320
  0x0000003fffffffffULL,	// mask / 2^320
  0x000001ffffffffffULL,	// mask / 2^320
  0x00000fffffffffffULL,	// mask / 2^320
  0x0000ffffffffffffULL,	// mask / 2^320
  0x0007ffffffffffffULL,	// mask / 2^320
  0x003fffffffffffffULL,	// mask / 2^320
  0x03ffffffffffffffULL	// mask / 2^320
};

BID_UINT192 bid_ten2mxtrunc192[] = {
  {{0xccccccccccccccccULL, 0xccccccccccccccccULL,
    0xccccccccccccccccULL}},
  // (ten2mx >> 192) = cccccccccccccccccccccccccccccccccccccccccccccccc
  {{0xd70a3d70a3d70a3dULL, 0x3d70a3d70a3d70a3ULL,
    0xa3d70a3d70a3d70aULL}},
  // (ten2mx >> 192) = a3d70a3d70a3d70a3d70a3d70a3d70a3d70a3d70a3d70a3d
  {{0x78d4fdf3b645a1caULL, 0x645a1cac083126e9ULL,
    0x83126e978d4fdf3bULL}},
  // (ten2mx >> 192) = 83126e978d4fdf3b645a1cac083126e978d4fdf3b645a1ca
  {{0xc154c985f06f6944ULL, 0xd3c36113404ea4a8ULL,
    0xd1b71758e219652bULL}},
  // (ten2mx >> 192) = d1b71758e219652bd3c36113404ea4a8c154c985f06f6944
  {{0xcddd6e04c0592103ULL, 0x0fcf80dc33721d53ULL,
    0xa7c5ac471b478423ULL}},
  // (ten2mx >> 192) = a7c5ac471b4784230fcf80dc33721d53cddd6e04c0592103
  {{0xd7e45803cd141a69ULL, 0xa63f9a49c2c1b10fULL,
    0x8637bd05af6c69b5ULL}},
  // (ten2mx >> 192) = 8637bd05af6c69b5a63f9a49c2c1b10fd7e45803cd141a69
  {{0x8ca08cd2e1b9c3dbULL, 0x3d32907604691b4cULL,
    0xd6bf94d5e57a42bcULL}},
  // (ten2mx >> 192) = d6bf94d5e57a42bc3d32907604691b4c8ca08cd2e1b9c3db
  {{0x3d4d3d758161697cULL, 0xfdc20d2b36ba7c3dULL,
    0xabcc77118461cefcULL}},
  // (ten2mx >> 192) = abcc77118461cefcfdc20d2b36ba7c3d3d4d3d758161697c
  {{0xfdd7645e011abac9ULL, 0x31680a88f8953030ULL,
    0x89705f4136b4a597ULL}},
  // (ten2mx >> 192) = 89705f4136b4a59731680a88f8953030fdd7645e011abac9
  {{0x2fbf06fcce912adcULL, 0xb573440e5a884d1bULL,
    0xdbe6fecebdedd5beULL}},
  // (ten2mx >> 192) = dbe6fecebdedd5beb573440e5a884d1b2fbf06fcce912adc
  {{0xf2ff38ca3eda88b0ULL, 0xf78f69a51539d748ULL,
    0xafebff0bcb24aafeULL}},
  // (ten2mx >> 192) = afebff0bcb24aafef78f69a51539d748f2ff38ca3eda88b0
  {{0xf598fa3b657ba08dULL, 0xf93f87b7442e45d3ULL,
    0x8cbccc096f5088cbULL}},
  // (ten2mx >> 192) = 8cbccc096f5088cbf93f87b7442e45d3f598fa3b657ba08d
  {{0x88f4c3923bf900e2ULL, 0x2865a5f206b06fb9ULL,
    0xe12e13424bb40e13ULL}},
  // (ten2mx >> 192) = e12e13424bb40e132865a5f206b06fb988f4c3923bf900e2
  {{0x6d909c74fcc733e8ULL, 0x538484c19ef38c94ULL,
    0xb424dc35095cd80fULL}},
  // (ten2mx >> 192) = b424dc35095cd80f538484c19ef38c946d909c74fcc733e8
  {{0x57a6e390ca38f653ULL, 0x0f9d37014bf60a10ULL,
    0x901d7cf73ab0acd9ULL}},
  // (ten2mx >> 192) = 901d7cf73ab0acd90f9d37014bf60a1057a6e390ca38f653
  {{0xbf716c1add27f085ULL, 0x4c2ebe687989a9b3ULL,
    0xe69594bec44de15bULL}},
  // (ten2mx >> 192) = e69594bec44de15b4c2ebe687989a9b3bf716c1add27f085
  {{0xff8df0157db98d37ULL, 0x09befeb9fad487c2ULL,
    0xb877aa3236a4b449ULL}},
  // (ten2mx >> 192) = b877aa3236a4b44909befeb9fad487c2ff8df0157db98d37
  {{0x32d7f344649470f9ULL, 0x3aff322e62439fcfULL,
    0x9392ee8e921d5d07ULL}},
  // (ten2mx >> 192) = 9392ee8e921d5d073aff322e62439fcf32d7f344649470f9
  {{0x1e2652070753e7f4ULL, 0x2b31e9e3d06c32e5ULL,
    0xec1e4a7db69561a5ULL}},
  // (ten2mx >> 192) = ec1e4a7db69561a52b31e9e3d06c32e51e2652070753e7f4
  {{0x181ea8059f76532aULL, 0x88f4bb1ca6bcf584ULL,
    0xbce5086492111aeaULL}},
  // (ten2mx >> 192) = bce5086492111aea88f4bb1ca6bcf584181ea8059f76532a
  {{0x467eecd14c5ea8eeULL, 0xd3f6fc16ebca5e03ULL,
    0x971da05074da7beeULL}},
  // (ten2mx >> 192) = 971da05074da7beed3f6fc16ebca5e03467eecd14c5ea8ee
  {{0x70cb148213caa7e4ULL, 0x5324c68b12dd6338ULL,
    0xf1c90080baf72cb1ULL}},
  // (ten2mx >> 192) = f1c90080baf72cb15324c68b12dd633870cb148213caa7e4
  {{0x8d6f439b43088650ULL, 0x75b7053c0f178293ULL,
    0xc16d9a0095928a27ULL}},
  // (ten2mx >> 192) = c16d9a0095928a2775b7053c0f1782938d6f439b43088650
  {{0xd78c3615cf3a050cULL, 0xc4926a9672793542ULL,
    0x9abe14cd44753b52ULL}},
  // (ten2mx >> 192) = 9abe14cd44753b52c4926a9672793542d78c3615cf3a050c
  {{0x8c1389bc7ec33b47ULL, 0x3a83ddbd83f52204ULL,
    0xf79687aed3eec551ULL}},
  // (ten2mx >> 192) = f79687aed3eec5513a83ddbd83f522048c1389bc7ec33b47
  {{0x3cdc6e306568fc39ULL, 0x95364afe032a819dULL,
    0xc612062576589ddaULL}},
  // (ten2mx >> 192) = c612062576589dda95364afe032a819d3cdc6e306568fc39
  {{0xca49f1c05120c9c7ULL, 0x775ea264cf55347dULL,
    0x9e74d1b791e07e48ULL}},
  // (ten2mx >> 192) = 9e74d1b791e07e48775ea264cf55347dca49f1c05120c9c7
  {{0x76dcb60081ce0fa5ULL, 0x8bca9d6e188853fcULL,
    0xfd87b5f28300ca0dULL}},
  // (ten2mx >> 192) = fd87b5f28300ca0d8bca9d6e188853fc76dcb60081ce0fa5
  {{0x5f16f80067d80c84ULL, 0x096ee45813a04330ULL,
    0xcad2f7f5359a3b3eULL}},
  // (ten2mx >> 192) = cad2f7f5359a3b3e096ee45813a043305f16f80067d80c84
  {{0x18df2ccd1fe00a03ULL, 0xa1258379a94d028dULL,
    0xa2425ff75e14fc31ULL}},
  // (ten2mx >> 192) = a2425ff75e14fc31a1258379a94d028d18df2ccd1fe00a03
  {{0x4718f0a419800802ULL, 0x80eacf948770ced7ULL,
    0x81ceb32c4b43fcf4ULL}},
  // (ten2mx >> 192) = 81ceb32c4b43fcf480eacf948770ced74718f0a419800802
  {{0x0b5b1aa028ccd99eULL, 0x67de18eda5814af2ULL,
    0xcfb11ead453994baULL}},
  // (ten2mx >> 192) = cfb11ead453994ba67de18eda5814af20b5b1aa028ccd99e
  {{0x6f7c154ced70ae18ULL, 0xecb1ad8aeacdd58eULL,
    0xa6274bbdd0fadd61ULL}},
  // (ten2mx >> 192) = a6274bbdd0fadd61ecb1ad8aeacdd58e6f7c154ced70ae18
  {{0xbf967770bdf3be79ULL, 0xbd5af13bef0b113eULL,
    0x84ec3c97da624ab4ULL}},
  // (ten2mx >> 192) = 84ec3c97da624ab4bd5af13bef0b113ebf967770bdf3be79
  {{0x65bd8be79652ca5cULL, 0x955e4ec64b44e864ULL,
    0xd4ad2dbfc3d07787ULL}},
  // (ten2mx >> 192) = d4ad2dbfc3d07787955e4ec64b44e86465bd8be79652ca5c
  {{0xeafe098611dbd516ULL, 0xdde50bd1d5d0b9e9ULL,
    0xaa242499697392d2ULL}},
  // (ten2mx >> 192) = aa242499697392d2dde50bd1d5d0b9e9eafe098611dbd516
  {{0xbbfe6e04db164412ULL, 0x7e50d64177da2e54ULL,
    0x881cea14545c7575ULL}},
  // (ten2mx >> 192) = 881cea14545c75757e50d64177da2e54bbfe6e04db164412
  {{0x2cca49a15e8a0683ULL, 0x96e7bd358c904a21ULL,
    0xd9c7dced53c72255ULL}},
  // (ten2mx >> 192) = d9c7dced53c7225596e7bd358c904a212cca49a15e8a0683
  {{0x8a3b6e1ab2080536ULL, 0xabec975e0a0d081aULL,
    0xae397d8aa96c1b77ULL}},
  // (ten2mx >> 192) = ae397d8aa96c1b77abec975e0a0d081a8a3b6e1ab2080536
  {{0x3b62be7bc1a0042bULL, 0x2323ac4b3b3da015ULL,
    0x8b61313bbabce2c6ULL}},
  // (ten2mx >> 192) = 8b61313bbabce2c62323ac4b3b3da0153b62be7bc1a0042b
  {{0x5f0463f935ccd378ULL, 0x6b6c46dec52f6688ULL,
    0xdf01e85f912e37a3ULL}},
  // (ten2mx >> 192) = df01e85f912e37a36b6c46dec52f66885f0463f935ccd378
  {{0x7f36b660f7d70f93ULL, 0x55f038b237591ed3ULL,
    0xb267ed1940f1c61cULL}},
  // (ten2mx >> 192) = b267ed1940f1c61c55f038b237591ed37f36b660f7d70f93
  {{0xcc2bc51a5fdf3fa9ULL, 0x77f3608e92adb242ULL,
    0x8eb98a7a9a5b04e3ULL}},
  // (ten2mx >> 192) = 8eb98a7a9a5b04e377f3608e92adb242cc2bc51a5fdf3fa9
  {{0xe046082a32fecc41ULL, 0x8cb89a7db77c506aULL,
    0xe45c10c42a2b3b05ULL}},
  // (ten2mx >> 192) = e45c10c42a2b3b058cb89a7db77c506ae046082a32fecc41
  {{0x4d04d354f598a367ULL, 0x3d607b97c5fd0d22ULL,
    0xb6b00d69bb55c8d1ULL}},
  // (ten2mx >> 192) = b6b00d69bb55c8d13d607b97c5fd0d224d04d354f598a367
  {{0x3d9d75dd9146e91fULL, 0xcab3961304ca70e8ULL,
    0x9226712162ab070dULL}},
  // (ten2mx >> 192) = 9226712162ab070dcab3961304ca70e83d9d75dd9146e91f
  {{0xc8fbefc8e87174ffULL, 0xaab8f01e6e10b4a6ULL,
    0xe9d71b689dde71afULL}},
  // (ten2mx >> 192) = e9d71b689dde71afaab8f01e6e10b4a6c8fbefc8e87174ff
  {{0x3a63263a538df733ULL, 0x5560c018580d5d52ULL,
    0xbb127c53b17ec159ULL}},
  // (ten2mx >> 192) = bb127c53b17ec1595560c018580d5d523a63263a538df733
  {{0x2eb5b82ea93e5f5cULL, 0xdde7001379a44aa8ULL,
    0x95a8637627989aadULL}},
  // (ten2mx >> 192) = 95a8637627989aaddde7001379a44aa82eb5b82ea93e5f5c
  {{0x4abc59e441fd6560ULL, 0x963e66858f6d4440ULL,
    0xef73d256a5c0f77cULL}},
  // (ten2mx >> 192) = ef73d256a5c0f77c963e66858f6d44404abc59e441fd6560
  {{0x6efd14b69b311de6ULL, 0xde98520472bdd033ULL,
    0xbf8fdb78849a5f96ULL}},
  // (ten2mx >> 192) = bf8fdb78849a5f96de98520472bdd0336efd14b69b311de6
  {{0x259743c548f417ebULL, 0xe546a8038efe4029ULL,
    0x993fe2c6d07b7fabULL}},
  // (ten2mx >> 192) = 993fe2c6d07b7fabe546a8038efe4029259743c548f417eb
  {{0x3c25393ba7ecf312ULL, 0xd53dd99f4b3066a8ULL,
    0xf53304714d9265dfULL}},
  // (ten2mx >> 192) = f53304714d9265dfd53dd99f4b3066a83c25393ba7ecf312
  {{0x96842dc95323f5a8ULL, 0xaa97e14c3c26b886ULL,
    0xc428d05aa4751e4cULL}},
  // (ten2mx >> 192) = c428d05aa4751e4caa97e14c3c26b88696842dc95323f5a8
  {{0xab9cf16ddc1cc486ULL, 0x55464dd69685606bULL,
    0x9ced737bb6c4183dULL}},
  // (ten2mx >> 192) = 9ced737bb6c4183d55464dd69685606bab9cf16ddc1cc486
  {{0xac2e4f162cfad40aULL, 0xeed6e2f0f0d56712ULL, 0xfb158592be068d2eULL}}
  // (ten2mx >> 192) = fb158592be068d2eeed6e2f0f0d56712ac2e4f162cfad40a
};

BID_UINT256 bid_Kx256[] = {
  {{0xcccccccccccccccdULL, 0xccccccccccccccccULL,
    0xccccccccccccccccULL, 0xccccccccccccccccULL}},
  // 10^-1 ~= cccccccccccccccc  cccccccccccccccc   
  //   cccccccccccccccccccccccccccccccd   * 2^-259
  {{0x70a3d70a3d70a3d8ULL, 0xd70a3d70a3d70a3dULL,
    0x3d70a3d70a3d70a3ULL, 0xa3d70a3d70a3d70aULL}},
  // 10^-2 ~= a3d70a3d70a3d70a  3d70a3d70a3d70a3   
  //   d70a3d70a3d70a3d70a3d70a3d70a3d8   * 2^-262
  {{0xc083126e978d4fe0ULL, 0x78d4fdf3b645a1caULL,
    0x645a1cac083126e9ULL, 0x83126e978d4fdf3bULL}},
  // 10^-3 ~= 83126e978d4fdf3b  645a1cac083126e9   
  //   78d4fdf3b645a1cac083126e978d4fe0   * 2^-265
  {{0x67381d7dbf487fccULL, 0xc154c985f06f6944ULL,
    0xd3c36113404ea4a8ULL, 0xd1b71758e219652bULL}},
  // 10^-4 ~= d1b71758e219652b  d3c36113404ea4a8   
  //   c154c985f06f694467381d7dbf487fcc   * 2^-269
  {{0x85c67dfe32a0663dULL, 0xcddd6e04c0592103ULL,
    0x0fcf80dc33721d53ULL, 0xa7c5ac471b478423ULL}},
  // 10^-5 ~= a7c5ac471b478423  fcf80dc33721d53   
  //   cddd6e04c059210385c67dfe32a0663d   * 2^-272
  {{0x37d1fe64f54d1e97ULL, 0xd7e45803cd141a69ULL,
    0xa63f9a49c2c1b10fULL, 0x8637bd05af6c69b5ULL}},
  // 10^-6 ~= 8637bd05af6c69b5  a63f9a49c2c1b10f   
  //   d7e45803cd141a6937d1fe64f54d1e97   * 2^-275
  {{0x8c8330a1887b6425ULL, 0x8ca08cd2e1b9c3dbULL,
    0x3d32907604691b4cULL, 0xd6bf94d5e57a42bcULL}},
  // 10^-7 ~= d6bf94d5e57a42bc  3d32907604691b4c   
  //   8ca08cd2e1b9c3db8c8330a1887b6425   * 2^-279
  {{0x7068f3b46d2f8351ULL, 0x3d4d3d758161697cULL,
    0xfdc20d2b36ba7c3dULL, 0xabcc77118461cefcULL}},
  // 10^-8 ~= abcc77118461cefc  fdc20d2b36ba7c3d   
  //   3d4d3d758161697c7068f3b46d2f8351   * 2^-282
  {{0xf387295d242602a7ULL, 0xfdd7645e011abac9ULL,
    0x31680a88f8953030ULL, 0x89705f4136b4a597ULL}},
  // 10^-9 ~= 89705f4136b4a597  31680a88f8953030   
  //   fdd7645e011abac9f387295d242602a7   * 2^-285
  {{0xb8d8422ea03cd10bULL, 0x2fbf06fcce912adcULL,
    0xb573440e5a884d1bULL, 0xdbe6fecebdedd5beULL}},
  // 10^-10 ~= dbe6fecebdedd5be  b573440e5a884d1b   
  //   2fbf06fcce912adcb8d8422ea03cd10b   * 2^-289
  {{0x93e034f219ca40d6ULL, 0xf2ff38ca3eda88b0ULL,
    0xf78f69a51539d748ULL, 0xafebff0bcb24aafeULL}},
  // 10^-11 ~= afebff0bcb24aafe  f78f69a51539d748   
  //   f2ff38ca3eda88b093e034f219ca40d6   * 2^-292
  {{0x4319c3f4e16e9a45ULL, 0xf598fa3b657ba08dULL,
    0xf93f87b7442e45d3ULL, 0x8cbccc096f5088cbULL}},
  // 10^-12 ~= 8cbccc096f5088cb  f93f87b7442e45d3   
  //   f598fa3b657ba08d4319c3f4e16e9a45   * 2^-295
  {{0x04f606549be42a07ULL, 0x88f4c3923bf900e2ULL,
    0x2865a5f206b06fb9ULL, 0xe12e13424bb40e13ULL}},
  // 10^-13 ~= e12e13424bb40e13  2865a5f206b06fb9   
  //   88f4c3923bf900e204f606549be42a07   * 2^-299
  {{0x03f805107cb68806ULL, 0x6d909c74fcc733e8ULL,
    0x538484c19ef38c94ULL, 0xb424dc35095cd80fULL}},
  // 10^-14 ~= b424dc35095cd80f  538484c19ef38c94   
  //   6d909c74fcc733e803f805107cb68806   * 2^-302
  {{0x3660040d3092066bULL, 0x57a6e390ca38f653ULL,
    0x0f9d37014bf60a10ULL, 0x901d7cf73ab0acd9ULL}},
  // 10^-15 ~= 901d7cf73ab0acd9  f9d37014bf60a10   
  //   57a6e390ca38f6533660040d3092066b   * 2^-305
  {{0x23ccd3484db670abULL, 0xbf716c1add27f085ULL,
    0x4c2ebe687989a9b3ULL, 0xe69594bec44de15bULL}},
  // 10^-16 ~= e69594bec44de15b  4c2ebe687989a9b3   
  //   bf716c1add27f08523ccd3484db670ab   * 2^-309
  {{0x4fd70f6d0af85a23ULL, 0xff8df0157db98d37ULL,
    0x09befeb9fad487c2ULL, 0xb877aa3236a4b449ULL}},
  // 10^-17 ~= b877aa3236a4b449  9befeb9fad487c2   
  //   ff8df0157db98d374fd70f6d0af85a23   * 2^-312
  {{0x0cac0c573bf9e1b6ULL, 0x32d7f344649470f9ULL,
    0x3aff322e62439fcfULL, 0x9392ee8e921d5d07ULL}},
  // 10^-18 ~= 9392ee8e921d5d07  3aff322e62439fcf   
  //   32d7f344649470f90cac0c573bf9e1b6   * 2^-315
  {{0xe11346f1f98fcf89ULL, 0x1e2652070753e7f4ULL,
    0x2b31e9e3d06c32e5ULL, 0xec1e4a7db69561a5ULL}},
  // 10^-19 ~= ec1e4a7db69561a5  2b31e9e3d06c32e5   
  //   1e2652070753e7f4e11346f1f98fcf89   * 2^-319
  {{0x4da9058e613fd93aULL, 0x181ea8059f76532aULL,
    0x88f4bb1ca6bcf584ULL, 0xbce5086492111aeaULL}},
  // 10^-20 ~= bce5086492111aea  88f4bb1ca6bcf584   
  //   181ea8059f76532a4da9058e613fd93a   * 2^-322
  {{0xa48737a51a997a95ULL, 0x467eecd14c5ea8eeULL,
    0xd3f6fc16ebca5e03ULL, 0x971da05074da7beeULL}},
  // 10^-21 ~= 971da05074da7bee  d3f6fc16ebca5e03   
  //   467eecd14c5ea8eea48737a51a997a95   * 2^-325
  {{0x3a71f2a1c428c421ULL, 0x70cb148213caa7e4ULL,
    0x5324c68b12dd6338ULL, 0xf1c90080baf72cb1ULL}},
  // 10^-22 ~= f1c90080baf72cb1  5324c68b12dd6338   
  //   70cb148213caa7e43a71f2a1c428c421   * 2^-329
  {{0x2ec18ee7d0209ce8ULL, 0x8d6f439b43088650ULL,
    0x75b7053c0f178293ULL, 0xc16d9a0095928a27ULL}},
  // 10^-23 ~= c16d9a0095928a27  75b7053c0f178293   
  //   8d6f439b430886502ec18ee7d0209ce8   * 2^-332
  {{0xf23472530ce6e3edULL, 0xd78c3615cf3a050cULL,
    0xc4926a9672793542ULL, 0x9abe14cd44753b52ULL}},
  // 10^-24 ~= 9abe14cd44753b52  c4926a9672793542   
  //   d78c3615cf3a050cf23472530ce6e3ed   * 2^-335
  {{0xe9ed83b814a49fe1ULL, 0x8c1389bc7ec33b47ULL,
    0x3a83ddbd83f52204ULL, 0xf79687aed3eec551ULL}},
  // 10^-25 ~= f79687aed3eec551  3a83ddbd83f52204   
  //   8c1389bc7ec33b47e9ed83b814a49fe1   * 2^-339
  {{0x87f1362cdd507fe7ULL, 0x3cdc6e306568fc39ULL,
    0x95364afe032a819dULL, 0xc612062576589ddaULL}},
  // 10^-26 ~= c612062576589dda  95364afe032a819d   
  //   3cdc6e306568fc3987f1362cdd507fe7   * 2^-342
  {{0x9ff42b5717739986ULL, 0xca49f1c05120c9c7ULL,
    0x775ea264cf55347dULL, 0x9e74d1b791e07e48ULL}},
  // 10^-27 ~= 9e74d1b791e07e48  775ea264cf55347d   
  //   ca49f1c05120c9c79ff42b5717739986   * 2^-345
  {{0xccb9def1bf1f5c09ULL, 0x76dcb60081ce0fa5ULL,
    0x8bca9d6e188853fcULL, 0xfd87b5f28300ca0dULL}},
  // 10^-28 ~= fd87b5f28300ca0d  8bca9d6e188853fc   
  //   76dcb60081ce0fa5ccb9def1bf1f5c09   * 2^-349
  {{0xa3c7e58e327f7cd4ULL, 0x5f16f80067d80c84ULL,
    0x096ee45813a04330ULL, 0xcad2f7f5359a3b3eULL}},
  // 10^-29 ~= cad2f7f5359a3b3e  96ee45813a04330   
  //   5f16f80067d80c84a3c7e58e327f7cd4   * 2^-352
  {{0xb6398471c1ff9710ULL, 0x18df2ccd1fe00a03ULL,
    0xa1258379a94d028dULL, 0xa2425ff75e14fc31ULL}},
  // 10^-30 ~= a2425ff75e14fc31  a1258379a94d028d   
  //   18df2ccd1fe00a03b6398471c1ff9710   * 2^-355
  {{0xf82e038e34cc78daULL, 0x4718f0a419800802ULL,
    0x80eacf948770ced7ULL, 0x81ceb32c4b43fcf4ULL}},
  // 10^-31 ~= 81ceb32c4b43fcf4  80eacf948770ced7   
  //   4718f0a419800802f82e038e34cc78da   * 2^-358
  {{0x59e338e387ad8e29ULL, 0x0b5b1aa028ccd99eULL,
    0x67de18eda5814af2ULL, 0xcfb11ead453994baULL}},
  // 10^-32 ~= cfb11ead453994ba  67de18eda5814af2   
  //   b5b1aa028ccd99e59e338e387ad8e29   * 2^-362
  {{0x47e8fa4f9fbe0b54ULL, 0x6f7c154ced70ae18ULL,
    0xecb1ad8aeacdd58eULL, 0xa6274bbdd0fadd61ULL}},
  // 10^-33 ~= a6274bbdd0fadd61  ecb1ad8aeacdd58e   
  //   6f7c154ced70ae1847e8fa4f9fbe0b54   * 2^-365
  {{0xd320c83fb2fe6f76ULL, 0xbf967770bdf3be79ULL,
    0xbd5af13bef0b113eULL, 0x84ec3c97da624ab4ULL}},
  // 10^-34 ~= 84ec3c97da624ab4  bd5af13bef0b113e   
  //   bf967770bdf3be79d320c83fb2fe6f76   * 2^-368
  {{0x85014065eb30b257ULL, 0x65bd8be79652ca5cULL,
    0x955e4ec64b44e864ULL, 0xd4ad2dbfc3d07787ULL}},
  // 10^-35 ~= d4ad2dbfc3d07787  955e4ec64b44e864   
  //   65bd8be79652ca5c85014065eb30b257   * 2^-372
  {{0xd0cdcd1e55c08eacULL, 0xeafe098611dbd516ULL,
    0xdde50bd1d5d0b9e9ULL, 0xaa242499697392d2ULL}},
  // 10^-36 ~= aa242499697392d2  dde50bd1d5d0b9e9   
  //   eafe098611dbd516d0cdcd1e55c08eac   * 2^-375
  {{0x40a4a418449a0bbdULL, 0xbbfe6e04db164412ULL,
    0x7e50d64177da2e54ULL, 0x881cea14545c7575ULL}},
  // 10^-37 ~= 881cea14545c7575  7e50d64177da2e54   
  //   bbfe6e04db16441240a4a418449a0bbd   * 2^-378
  {{0x9aa1068d3a9012c8ULL, 0x2cca49a15e8a0683ULL,
    0x96e7bd358c904a21ULL, 0xd9c7dced53c72255ULL}},
  // 10^-38 ~= d9c7dced53c72255  96e7bd358c904a21   
  //   2cca49a15e8a06839aa1068d3a9012c8   * 2^-382
  {{0x154d9ed7620cdbd3ULL, 0x8a3b6e1ab2080536ULL,
    0xabec975e0a0d081aULL, 0xae397d8aa96c1b77ULL}},
  // 10^-39 ~= ae397d8aa96c1b77  abec975e0a0d081a   
  //   8a3b6e1ab2080536154d9ed7620cdbd3   * 2^-385
  {{0x443e18ac4e70afdcULL, 0x3b62be7bc1a0042bULL,
    0x2323ac4b3b3da015ULL, 0x8b61313bbabce2c6ULL}},
  // 10^-40 ~= 8b61313bbabce2c6  2323ac4b3b3da015   
  //   3b62be7bc1a0042b443e18ac4e70afdc   * 2^-388
  {{0x6d30277a171ab2f9ULL, 0x5f0463f935ccd378ULL,
    0x6b6c46dec52f6688ULL, 0xdf01e85f912e37a3ULL}},
  // 10^-41 ~= df01e85f912e37a3  6b6c46dec52f6688   
  //   5f0463f935ccd3786d30277a171ab2f9   * 2^-392
  {{0x8a8cec61ac155bfbULL, 0x7f36b660f7d70f93ULL,
    0x55f038b237591ed3ULL, 0xb267ed1940f1c61cULL}},
  // 10^-42 ~= b267ed1940f1c61c  55f038b237591ed3   
  //   7f36b660f7d70f938a8cec61ac155bfb   * 2^-395
  {{0x3ba3f04e23444996ULL, 0xcc2bc51a5fdf3fa9ULL,
    0x77f3608e92adb242ULL, 0x8eb98a7a9a5b04e3ULL}},
  // 10^-43 ~= 8eb98a7a9a5b04e3  77f3608e92adb242   
  //   cc2bc51a5fdf3fa93ba3f04e23444996   * 2^-398
  {{0xf9064d49d206dc22ULL, 0xe046082a32fecc41ULL,
    0x8cb89a7db77c506aULL, 0xe45c10c42a2b3b05ULL}},
  // 10^-44 ~= e45c10c42a2b3b05  8cb89a7db77c506a   
  //   e046082a32fecc41f9064d49d206dc22   * 2^-402
  {{0xfa6b7107db38b01bULL, 0x4d04d354f598a367ULL,
    0x3d607b97c5fd0d22ULL, 0xb6b00d69bb55c8d1ULL}},
  // 10^-45 ~= b6b00d69bb55c8d1  3d607b97c5fd0d22   
  //   4d04d354f598a367fa6b7107db38b01b   * 2^-405
  {{0xfb8927397c2d59b0ULL, 0x3d9d75dd9146e91fULL,
    0xcab3961304ca70e8ULL, 0x9226712162ab070dULL}},
  // 10^-46 ~= 9226712162ab070d  cab3961304ca70e8   
  //   3d9d75dd9146e91ffb8927397c2d59b0   * 2^-408
  {{0xf8db71f5937bc2b2ULL, 0xc8fbefc8e87174ffULL,
    0xaab8f01e6e10b4a6ULL, 0xe9d71b689dde71afULL}},
  // 10^-47 ~= e9d71b689dde71af  aab8f01e6e10b4a6   
  //   c8fbefc8e87174fff8db71f5937bc2b2   * 2^-412
  {{0x2d7c5b2adc630228ULL, 0x3a63263a538df733ULL,
    0x5560c018580d5d52ULL, 0xbb127c53b17ec159ULL}},
  // 10^-48 ~= bb127c53b17ec159  5560c018580d5d52   
  //   3a63263a538df7332d7c5b2adc630228   * 2^-415
  {{0x24637c2249e8ce87ULL, 0x2eb5b82ea93e5f5cULL,
    0xdde7001379a44aa8ULL, 0x95a8637627989aadULL}},
  // 10^-49 ~= 95a8637627989aad  dde7001379a44aa8   
  //   2eb5b82ea93e5f5c24637c2249e8ce87   * 2^-418
  {{0x3a38c69d430e173eULL, 0x4abc59e441fd6560ULL,
    0x963e66858f6d4440ULL, 0xef73d256a5c0f77cULL}},
  // 10^-50 ~= ef73d256a5c0f77c  963e66858f6d4440   
  //   4abc59e441fd65603a38c69d430e173e   * 2^-422
  {{0x94fa387dcf3e78feULL, 0x6efd14b69b311de6ULL,
    0xde98520472bdd033ULL, 0xbf8fdb78849a5f96ULL}},
  // 10^-51 ~= bf8fdb78849a5f96  de98520472bdd033   
  //   6efd14b69b311de694fa387dcf3e78fe   * 2^-425
  {{0xaa61c6cb0c31fa65ULL, 0x259743c548f417ebULL,
    0xe546a8038efe4029ULL, 0x993fe2c6d07b7fabULL}},
  // 10^-52 ~= 993fe2c6d07b7fab  e546a8038efe4029   
  //   259743c548f417ebaa61c6cb0c31fa65   * 2^-428
  {{0xaa360ade79e990a2ULL, 0x3c25393ba7ecf312ULL,
    0xd53dd99f4b3066a8ULL, 0xf53304714d9265dfULL}},
  // 10^-53 ~= f53304714d9265df  d53dd99f4b3066a8   
  //   3c25393ba7ecf312aa360ade79e990a2   * 2^-432
  {{0x882b3be52e5473b5ULL, 0x96842dc95323f5a8ULL,
    0xaa97e14c3c26b886ULL, 0xc428d05aa4751e4cULL}},
  // 10^-54 ~= c428d05aa4751e4c  aa97e14c3c26b886   
  //   96842dc95323f5a8882b3be52e5473b5   * 2^-435
  {{0xd355c98425105c91ULL, 0xab9cf16ddc1cc486ULL,
    0x55464dd69685606bULL, 0x9ced737bb6c4183dULL}},
  // 10^-55 ~= 9ced737bb6c4183d  55464dd69685606b   
  //   ab9cf16ddc1cc486d355c98425105c91   * 2^-438
  {{0xebbc75a03b4d60e7ULL, 0xac2e4f162cfad40aULL,
    0xeed6e2f0f0d56712ULL, 0xfb158592be068d2eULL}},
  // 10^-56 ~= fb158592be068d2e  eed6e2f0f0d56712   
  //   ac2e4f162cfad40aebbc75a03b4d60e7   * 2^-442
  {{0x8963914cfc3de71fULL, 0x568b727823fbdcd5ULL,
    0xf245825a5a445275ULL, 0xc8de047564d20a8bULL}},
  // 10^-57 ~= c8de047564d20a8b  f245825a5a445275   
  //   568b727823fbdcd58963914cfc3de71f   * 2^-445
  {{0xd44fa770c9cb1f4cULL, 0x453c5b934ffcb0aaULL,
    0x5b6aceaeae9d0ec4ULL, 0xa0b19d2ab70e6ed6ULL}},
  // 10^-58 ~= a0b19d2ab70e6ed6  5b6aceaeae9d0ec4   
  //   453c5b934ffcb0aad44fa770c9cb1f4c   * 2^-448
  {{0xdd0c85f3d4a27f70ULL, 0x37637c75d996f3bbULL,
    0xe2bbd88bbee40bd0ULL, 0x808e17555f3ebf11ULL}},
  // 10^-59 ~= 808e17555f3ebf11  e2bbd88bbee40bd0   
  //   37637c75d996f3bbdd0c85f3d4a27f70   * 2^-451
  {{0x61ada31fba9d98b3ULL, 0x256bfa5628f185f9ULL,
    0x3792f412cb06794dULL, 0xcdb02555653131b6ULL}},
  // 10^-60 ~= cdb02555653131b6  3792f412cb06794d   
  //   256bfa5628f185f961ada31fba9d98b3   * 2^-455
  {{0xe7be1c196217ad5cULL, 0x51232eab53f46b2dULL,
    0x5fa8c3423c052dd7ULL, 0xa48ceaaab75a8e2bULL}},
  // 10^-61 ~= a48ceaaab75a8e2b  5fa8c3423c052dd7   
  //   51232eab53f46b2de7be1c196217ad5c   * 2^-458
  {{0x52fe7ce11b46244aULL, 0x40e8f222a99055beULL,
    0x1953cf68300424acULL, 0x83a3eeeef9153e89ULL}},
  // 10^-62 ~= 83a3eeeef9153e89  1953cf68300424ac   
  //   40e8f222a99055be52fe7ce11b46244a   * 2^-461
  {{0x51972e34f8703a10ULL, 0x34a7e9d10f4d55fdULL,
    0x8eec7f0d19a03aadULL, 0xd29fe4b18e88640eULL}},
  // 10^-63 ~= d29fe4b18e88640e  8eec7f0d19a03aad   
  //   34a7e9d10f4d55fd51972e34f8703a10   * 2^-465
  {{0x0e128b5d938cfb40ULL, 0x2a1fee40d90aab31ULL,
    0x3f2398d747b36224ULL, 0xa87fea27a539e9a5ULL}},
  // 10^-64 ~= a87fea27a539e9a5  3f2398d747b36224   
  //   2a1fee40d90aab310e128b5d938cfb40   * 2^-468
  {{0x3e753c4adc70c900ULL, 0xbb4cbe9a473bbc27ULL,
    0x98e947129fc2b4e9ULL, 0x86ccbb52ea94baeaULL}},
  // 10^-65 ~= 86ccbb52ea94baea  98e947129fc2b4e9   
  //   bb4cbe9a473bbc273e753c4adc70c900   * 2^-471
  {{0x30bb93aafa4e0e66ULL, 0x9214642a0b92c6a5ULL,
    0x5b0ed81dcc6abb0fULL, 0xd7adf884aa879177ULL}},
  // 10^-66 ~= d7adf884aa879177  5b0ed81dcc6abb0f   
  //   9214642a0b92c6a530bb93aafa4e0e66   * 2^-475
  {{0xc0960fbbfb71a51fULL, 0xa8105021a2dbd21dULL,
    0xe272467e3d222f3fULL, 0xac8b2d36eed2dac5ULL}},
  // 10^-67 ~= ac8b2d36eed2dac5  e272467e3d222f3f   
  //   a8105021a2dbd21dc0960fbbfb71a51f   * 2^-478
  {{0x66de72fcc927b74cULL, 0xb9a6a6814f1641b1ULL,
    0x1b8e9ecb641b58ffULL, 0x8a08f0f8bf0f156bULL}},
  // 10^-68 ~= 8a08f0f8bf0f156b  1b8e9ecb641b58ff   
  //   b9a6a6814f1641b166de72fcc927b74c   * 2^-481
  {{0xd7ca5194750c5879ULL, 0xf5d770cee4f0691bULL,
    0xf8e431456cf88e65ULL, 0xdcdb1b2798182244ULL}},
  // 10^-69 ~= dcdb1b2798182244  f8e431456cf88e65   
  //   f5d770cee4f0691bd7ca5194750c5879   * 2^-485
  {{0xdfd50e105da379faULL, 0x9179270bea59edafULL,
    0x2d835a9df0c6d851ULL, 0xb0af48ec79ace837ULL}},
  // 10^-70 ~= b0af48ec79ace837  2d835a9df0c6d851   
  //   9179270bea59edafdfd50e105da379fa   * 2^-488
  {{0x19773e737e1c6195ULL, 0x0dfa85a321e18af3ULL,
    0x579c487e5a38ad0eULL, 0x8d590723948a535fULL}},
  // 10^-71 ~= 8d590723948a535f  579c487e5a38ad0e   
  //   dfa85a321e18af319773e737e1c6195   * 2^-491
  {{0xf58b971f302d68efULL, 0x165da29e9c9c1184ULL,
    0x25c6da63c38de1b0ULL, 0xe2280b6c20dd5232ULL}},
  // 10^-72 ~= e2280b6c20dd5232  25c6da63c38de1b0   
  //   165da29e9c9c1184f58b971f302d68ef   * 2^-495
  {{0xc46fac18f3578725ULL, 0x4517b54bb07cdad0ULL,
    0x1e38aeb6360b1af3ULL, 0xb4ecd5f01a4aa828ULL}},
  // 10^-73 ~= b4ecd5f01a4aa828  1e38aeb6360b1af3   
  //   4517b54bb07cdad0c46fac18f3578725   * 2^-498
  {{0x36bfbce0c2ac6c1eULL, 0x9dac910959fd7bdaULL,
    0xb1c6f22b5e6f48c2ULL, 0x90bd77f3483bb9b9ULL}},
  // 10^-74 ~= 90bd77f3483bb9b9  b1c6f22b5e6f48c2   
  //   9dac910959fd7bda36bfbce0c2ac6c1e   * 2^-501
  {{0x2465fb01377a4696ULL, 0x2f7a81a88ffbf95dULL,
    0xb60b1d1230b20e04ULL, 0xe7958cb87392c2c2ULL}}
  // 10^-75 ~= e7958cb87392c2c2  b60b1d1230b20e04   
  //   2f7a81a88ffbf95d2465fb01377a4696   * 2^-505
};

unsigned int bid_Ex256m256[] = {
  3,	// 259 - 256, Ex = 259
  6,	// 262 - 256, Ex = 262
  9,	// 265 - 256, Ex = 265
  13,	// 269 - 256, Ex = 269
  16,	// 272 - 256, Ex = 272
  19,	// 275 - 256, Ex = 275
  23,	// 279 - 256, Ex = 279
  26,	// 282 - 256, Ex = 282
  29,	// 285 - 256, Ex = 285
  33,	// 289 - 256, Ex = 289
  36,	// 292 - 256, Ex = 292
  39,	// 295 - 256, Ex = 295
  43,	// 299 - 256, Ex = 299
  46,	// 302 - 256, Ex = 302
  49,	// 305 - 256, Ex = 305
  53,	// 309 - 256, Ex = 309
  56,	// 312 - 256, Ex = 312
  59,	// 315 - 256, Ex = 315
  63,	// 319 - 256, Ex = 319
  2,	// 322 - 320, Ex = 322
  5,	// 325 - 320, Ex = 325
  9,	// 329 - 320, Ex = 329
  12,	// 332 - 320, Ex = 332
  15,	// 335 - 320, Ex = 335
  19,	// 339 - 320, Ex = 339
  22,	// 342 - 320, Ex = 342
  25,	// 345 - 320, Ex = 345
  29,	// 349 - 320, Ex = 349
  32,	// 352 - 320, Ex = 352
  35,	// 355 - 320, Ex = 355
  38,	// 358 - 320, Ex = 358
  42,	// 362 - 320, Ex = 362
  45,	// 365 - 320, Ex = 365
  48,	// 368 - 320, Ex = 368
  52,	// 372 - 320, Ex = 372
  55,	// 375 - 320, Ex = 375
  58,	// 378 - 320, Ex = 378
  62,	// 382 - 320, Ex = 382
  1,	// 385 - 384, Ex = 385
  4,	// 388 - 384, Ex = 388
  8,	// 392 - 384, Ex = 392
  11,	// 395 - 384, Ex = 395
  14,	// 398 - 384, Ex = 398
  18,	// 402 - 384, Ex = 402
  21,	// 405 - 384, Ex = 405
  24,	// 408 - 384, Ex = 408
  28,	// 412 - 384, Ex = 412
  31,	// 415 - 384, Ex = 415
  34,	// 418 - 384, Ex = 418
  38,	// 422 - 384, Ex = 422
  41,	// 425 - 384, Ex = 425
  44,	// 428 - 384, Ex = 428
  48,	// 432 - 384, Ex = 432
  51,	// 435 - 384, Ex = 435
  54,	// 438 - 384, Ex = 438
  58,	// 442 - 384, Ex = 442
  61,	// 445 - 384, Ex = 445
  0,	// 448 - 448, Ex = 448
  3,	// 451 - 448, Ex = 451
  7,	// 455 - 448, Ex = 455
  10,	// 458 - 448, Ex = 458
  13,	// 461 - 448, Ex = 461
  17,	// 465 - 448, Ex = 465
  20,	// 468 - 448, Ex = 468
  23,	// 471 - 448, Ex = 471
  27,	// 475 - 448, Ex = 475
  30,	// 478 - 448, Ex = 478
  33,	// 481 - 448, Ex = 481
  37,	// 485 - 448, Ex = 485
  40,	// 488 - 448, Ex = 488
  43,	// 491 - 448, Ex = 491
  47,	// 495 - 448, Ex = 495
  50,	// 498 - 448, Ex = 498
  53,	// 501 - 448, Ex = 501
  57	// 505 - 448, Ex = 505
};

BID_UINT64 bid_half256[] = {
  0x0000000000000004ULL,	// half / 2^256 = 4
  0x0000000000000020ULL,	// half / 2^256 = 20
  0x0000000000000100ULL,	// half / 2^256 = 100
  0x0000000000001000ULL,	// half / 2^256 = 1000
  0x0000000000008000ULL,	// half / 2^256 = 8000
  0x0000000000040000ULL,	// half / 2^256 = 40000
  0x0000000000400000ULL,	// half / 2^256 = 400000
  0x0000000002000000ULL,	// half / 2^256 = 2000000
  0x0000000010000000ULL,	// half / 2^256 = 10000000
  0x0000000100000000ULL,	// half / 2^256 = 100000000
  0x0000000800000000ULL,	// half / 2^256 = 800000000
  0x0000004000000000ULL,	// half / 2^256 = 4000000000
  0x0000040000000000ULL,	// half / 2^256 = 40000000000
  0x0000200000000000ULL,	// half / 2^256 = 200000000000
  0x0001000000000000ULL,	// half / 2^256 = 1000000000000
  0x0010000000000000ULL,	// half / 2^256 = 10000000000000
  0x0080000000000000ULL,	// half / 2^256 = 80000000000000
  0x0400000000000000ULL,	// half / 2^256 = 400000000000000
  0x4000000000000000ULL,	// half / 2^256 = 4000000000000000
  0x0000000000000002ULL,	// half / 2^320 = 2
  0x0000000000000010ULL,	// half / 2^320 = 10
  0x0000000000000100ULL,	// half / 2^320 = 100
  0x0000000000000800ULL,	// half / 2^320 = 800
  0x0000000000004000ULL,	// half / 2^320 = 4000
  0x0000000000040000ULL,	// half / 2^320 = 40000
  0x0000000000200000ULL,	// half / 2^320 = 200000
  0x0000000001000000ULL,	// half / 2^320 = 1000000
  0x0000000010000000ULL,	// half / 2^320 = 10000000
  0x0000000080000000ULL,	// half / 2^320 = 80000000
  0x0000000400000000ULL,	// half / 2^320 = 400000000
  0x0000002000000000ULL,	// half / 2^320 = 2000000000
  0x0000020000000000ULL,	// half / 2^320 = 20000000000
  0x0000100000000000ULL,	// half / 2^320 = 100000000000
  0x0000800000000000ULL,	// half / 2^320 = 800000000000
  0x0008000000000000ULL,	// half / 2^320 = 8000000000000
  0x0040000000000000ULL,	// half / 2^320 = 40000000000000
  0x0200000000000000ULL,	// half / 2^320 = 200000000000000
  0x2000000000000000ULL,	// half / 2^320 = 2000000000000000
  0x0000000000000001ULL,	// half / 2^384 = 1
  0x0000000000000008ULL,	// half / 2^384 = 8
  0x0000000000000080ULL,	// half / 2^384 = 80
  0x0000000000000400ULL,	// half / 2^384 = 400
  0x0000000000002000ULL,	// half / 2^384 = 2000
  0x0000000000020000ULL,	// half / 2^384 = 20000
  0x0000000000100000ULL,	// half / 2^384 = 100000
  0x0000000000800000ULL,	// half / 2^384 = 800000
  0x0000000008000000ULL,	// half / 2^384 = 8000000
  0x0000000040000000ULL,	// half / 2^384 = 40000000
  0x0000000200000000ULL,	// half / 2^384 = 200000000
  0x0000002000000000ULL,	// half / 2^384 = 2000000000
  0x0000010000000000ULL,	// half / 2^384 = 10000000000
  0x0000080000000000ULL,	// half / 2^384 = 80000000000
  0x0000800000000000ULL,	// half / 2^384 = 800000000000
  0x0004000000000000ULL,	// half / 2^384 = 4000000000000
  0x0020000000000000ULL,	// half / 2^384 = 20000000000000
  0x0200000000000000ULL,	// half / 2^384 = 200000000000000
  0x1000000000000000ULL,	// half / 2^384 = 1000000000000000
  0x8000000000000000ULL,	// half / 2^384 = 8000000000000000
  0x0000000000000004ULL,	// half / 2^448 = 4
  0x0000000000000040ULL,	// half / 2^448 = 40
  0x0000000000000200ULL,	// half / 2^448 = 200
  0x0000000000001000ULL,	// half / 2^448 = 1000
  0x0000000000010000ULL,	// half / 2^448 = 10000
  0x0000000000080000ULL,	// half / 2^448 = 80000
  0x0000000000400000ULL,	// half / 2^448 = 400000
  0x0000000004000000ULL,	// half / 2^448 = 4000000
  0x0000000020000000ULL,	// half / 2^448 = 20000000
  0x0000000100000000ULL,	// half / 2^448 = 100000000
  0x0000001000000000ULL,	// half / 2^448 = 1000000000
  0x0000008000000000ULL,	// half / 2^448 = 8000000000
  0x0000040000000000ULL,	// half / 2^448 = 40000000000
  0x0000400000000000ULL,	// half / 2^448 = 400000000000
  0x0002000000000000ULL,	// half / 2^448 = 2000000000000
  0x0010000000000000ULL,	// half / 2^448 = 10000000000000
  0x0100000000000000ULL	// half / 2^448 = 100000000000000
};

BID_UINT64 bid_mask256[] = {
  0x0000000000000007ULL,	// mask / 2^256
  0x000000000000003fULL,	// mask / 2^256
  0x00000000000001ffULL,	// mask / 2^256
  0x0000000000001fffULL,	// mask / 2^256
  0x000000000000ffffULL,	// mask / 2^256
  0x000000000007ffffULL,	// mask / 2^256
  0x00000000007fffffULL,	// mask / 2^256
  0x0000000003ffffffULL,	// mask / 2^256
  0x000000001fffffffULL,	// mask / 2^256
  0x00000001ffffffffULL,	// mask / 2^256
  0x0000000fffffffffULL,	// mask / 2^256
  0x0000007fffffffffULL,	// mask / 2^256
  0x000007ffffffffffULL,	// mask / 2^256
  0x00003fffffffffffULL,	// mask / 2^256
  0x0001ffffffffffffULL,	// mask / 2^256
  0x001fffffffffffffULL,	// mask / 2^256
  0x00ffffffffffffffULL,	// mask / 2^256
  0x07ffffffffffffffULL,	// mask / 2^256
  0x7fffffffffffffffULL,	// mask / 2^256
  0x0000000000000003ULL,	// mask / 2^320
  0x000000000000001fULL,	// mask / 2^320
  0x00000000000001ffULL,	// mask / 2^320
  0x0000000000000fffULL,	// mask / 2^320
  0x0000000000007fffULL,	// mask / 2^320
  0x000000000007ffffULL,	// mask / 2^320
  0x00000000003fffffULL,	// mask / 2^320
  0x0000000001ffffffULL,	// mask / 2^320
  0x000000001fffffffULL,	// mask / 2^320
  0x00000000ffffffffULL,	// mask / 2^320
  0x00000007ffffffffULL,	// mask / 2^320
  0x0000003fffffffffULL,	// mask / 2^320
  0x000003ffffffffffULL,	// mask / 2^320
  0x00001fffffffffffULL,	// mask / 2^320
  0x0000ffffffffffffULL,	// mask / 2^320
  0x000fffffffffffffULL,	// mask / 2^320
  0x007fffffffffffffULL,	// mask / 2^320
  0x03ffffffffffffffULL,	// mask / 2^320
  0x3fffffffffffffffULL,	// mask / 2^320
  0x0000000000000001ULL,	// mask / 2^384
  0x000000000000000fULL,	// mask / 2^384
  0x00000000000000ffULL,	// mask / 2^384
  0x00000000000007ffULL,	// mask / 2^384
  0x0000000000003fffULL,	// mask / 2^384
  0x000000000003ffffULL,	// mask / 2^384
  0x00000000001fffffULL,	// mask / 2^384
  0x0000000000ffffffULL,	// mask / 2^384
  0x000000000fffffffULL,	// mask / 2^384
  0x000000007fffffffULL,	// mask / 2^384
  0x00000003ffffffffULL,	// mask / 2^384
  0x0000003fffffffffULL,	// mask / 2^384
  0x000001ffffffffffULL,	// mask / 2^384
  0x00000fffffffffffULL,	// mask / 2^384
  0x0000ffffffffffffULL,	// mask / 2^384
  0x0007ffffffffffffULL,	// mask / 2^384
  0x003fffffffffffffULL,	// mask / 2^384
  0x03ffffffffffffffULL,	// mask / 2^384
  0x1fffffffffffffffULL,	// mask / 2^384
  0xffffffffffffffffULL,	// mask / 2^384
  0x0000000000000007ULL,	// mask / 2^448
  0x000000000000007fULL,	// mask / 2^448
  0x00000000000003ffULL,	// mask / 2^448
  0x0000000000001fffULL,	// mask / 2^448
  0x000000000001ffffULL,	// mask / 2^448
  0x00000000000fffffULL,	// mask / 2^448
  0x00000000007fffffULL,	// mask / 2^448
  0x0000000007ffffffULL,	// mask / 2^448
  0x000000003fffffffULL,	// mask / 2^448
  0x00000001ffffffffULL,	// mask / 2^448
  0x0000001fffffffffULL,	// mask / 2^448
  0x000000ffffffffffULL,	// mask / 2^448
  0x000007ffffffffffULL,	// mask / 2^448
  0x00007fffffffffffULL,	// mask / 2^448
  0x0003ffffffffffffULL,	// mask / 2^448
  0x001fffffffffffffULL,	// mask / 2^448
  0x01ffffffffffffffULL	// mask / 2^448
};

BID_UINT256 bid_ten2mxtrunc256[] = {
  {{0xccccccccccccccccULL, 0xccccccccccccccccULL,
    0xccccccccccccccccULL, 0xccccccccccccccccULL}},
  // (ten2mx >> 256) = cccccccccccccccc  cccccccccccccccc   
  //   cccccccccccccccccccccccccccccccc
  {{0x70a3d70a3d70a3d7ULL, 0xd70a3d70a3d70a3dULL,
    0x3d70a3d70a3d70a3ULL, 0xa3d70a3d70a3d70aULL}},
  // (ten2mx >> 256) = a3d70a3d70a3d70a  3d70a3d70a3d70a3   
  //   d70a3d70a3d70a3d70a3d70a3d70a3d7
  {{0xc083126e978d4fdfULL, 0x78d4fdf3b645a1caULL,
    0x645a1cac083126e9ULL, 0x83126e978d4fdf3bULL}},
  // (ten2mx >> 256) = 83126e978d4fdf3b  645a1cac083126e9   
  //   78d4fdf3b645a1cac083126e978d4fdf
  {{0x67381d7dbf487fcbULL, 0xc154c985f06f6944ULL,
    0xd3c36113404ea4a8ULL, 0xd1b71758e219652bULL}},
  // (ten2mx >> 256) = d1b71758e219652b  d3c36113404ea4a8   
  //   c154c985f06f694467381d7dbf487fcb
  {{0x85c67dfe32a0663cULL, 0xcddd6e04c0592103ULL,
    0x0fcf80dc33721d53ULL, 0xa7c5ac471b478423ULL}},
  // (ten2mx >> 256) = a7c5ac471b478423  fcf80dc33721d53   
  //   cddd6e04c059210385c67dfe32a0663c
  {{0x37d1fe64f54d1e96ULL, 0xd7e45803cd141a69ULL,
    0xa63f9a49c2c1b10fULL, 0x8637bd05af6c69b5ULL}},
  // (ten2mx >> 256) = 8637bd05af6c69b5  a63f9a49c2c1b10f   
  //   d7e45803cd141a6937d1fe64f54d1e96
  {{0x8c8330a1887b6424ULL, 0x8ca08cd2e1b9c3dbULL,
    0x3d32907604691b4cULL, 0xd6bf94d5e57a42bcULL}},
  // (ten2mx >> 256) = d6bf94d5e57a42bc  3d32907604691b4c   
  //   8ca08cd2e1b9c3db8c8330a1887b6424
  {{0x7068f3b46d2f8350ULL, 0x3d4d3d758161697cULL,
    0xfdc20d2b36ba7c3dULL, 0xabcc77118461cefcULL}},
  // (ten2mx >> 256) = abcc77118461cefc  fdc20d2b36ba7c3d   
  //   3d4d3d758161697c7068f3b46d2f8350
  {{0xf387295d242602a6ULL, 0xfdd7645e011abac9ULL,
    0x31680a88f8953030ULL, 0x89705f4136b4a597ULL}},
  // (ten2mx >> 256) = 89705f4136b4a597  31680a88f8953030   
  //   fdd7645e011abac9f387295d242602a6
  {{0xb8d8422ea03cd10aULL, 0x2fbf06fcce912adcULL,
    0xb573440e5a884d1bULL, 0xdbe6fecebdedd5beULL}},
  // (ten2mx >> 256) = dbe6fecebdedd5be  b573440e5a884d1b   
  //   2fbf06fcce912adcb8d8422ea03cd10a
  {{0x93e034f219ca40d5ULL, 0xf2ff38ca3eda88b0ULL,
    0xf78f69a51539d748ULL, 0xafebff0bcb24aafeULL}},
  // (ten2mx >> 256) = afebff0bcb24aafe  f78f69a51539d748   
  //   f2ff38ca3eda88b093e034f219ca40d5
  {{0x4319c3f4e16e9a44ULL, 0xf598fa3b657ba08dULL,
    0xf93f87b7442e45d3ULL, 0x8cbccc096f5088cbULL}},
  // (ten2mx >> 256) = 8cbccc096f5088cb  f93f87b7442e45d3   
  //   f598fa3b657ba08d4319c3f4e16e9a44
  {{0x04f606549be42a06ULL, 0x88f4c3923bf900e2ULL,
    0x2865a5f206b06fb9ULL, 0xe12e13424bb40e13ULL}},
  // (ten2mx >> 256) = e12e13424bb40e13  2865a5f206b06fb9   
  //   88f4c3923bf900e204f606549be42a06
  {{0x03f805107cb68805ULL, 0x6d909c74fcc733e8ULL,
    0x538484c19ef38c94ULL, 0xb424dc35095cd80fULL}},
  // (ten2mx >> 256) = b424dc35095cd80f  538484c19ef38c94   
  //   6d909c74fcc733e803f805107cb68805
  {{0x3660040d3092066aULL, 0x57a6e390ca38f653ULL,
    0x0f9d37014bf60a10ULL, 0x901d7cf73ab0acd9ULL}},
  // (ten2mx >> 256) = 901d7cf73ab0acd9  f9d37014bf60a10   
  //   57a6e390ca38f6533660040d3092066a
  {{0x23ccd3484db670aaULL, 0xbf716c1add27f085ULL,
    0x4c2ebe687989a9b3ULL, 0xe69594bec44de15bULL}},
  // (ten2mx >> 256) = e69594bec44de15b  4c2ebe687989a9b3   
  //   bf716c1add27f08523ccd3484db670aa
  {{0x4fd70f6d0af85a22ULL, 0xff8df0157db98d37ULL,
    0x09befeb9fad487c2ULL, 0xb877aa3236a4b449ULL}},
  // (ten2mx >> 256) = b877aa3236a4b449  9befeb9fad487c2   
  //   ff8df0157db98d374fd70f6d0af85a22
  {{0x0cac0c573bf9e1b5ULL, 0x32d7f344649470f9ULL,
    0x3aff322e62439fcfULL, 0x9392ee8e921d5d07ULL}},
  // (ten2mx >> 256) = 9392ee8e921d5d07  3aff322e62439fcf   
  //   32d7f344649470f90cac0c573bf9e1b5
  {{0xe11346f1f98fcf88ULL, 0x1e2652070753e7f4ULL,
    0x2b31e9e3d06c32e5ULL, 0xec1e4a7db69561a5ULL}},
  // (ten2mx >> 256) = ec1e4a7db69561a5  2b31e9e3d06c32e5   
  //   1e2652070753e7f4e11346f1f98fcf88
  {{0x4da9058e613fd939ULL, 0x181ea8059f76532aULL,
    0x88f4bb1ca6bcf584ULL, 0xbce5086492111aeaULL}},
  // (ten2mx >> 256) = bce5086492111aea  88f4bb1ca6bcf584   
  //   181ea8059f76532a4da9058e613fd939
  {{0xa48737a51a997a94ULL, 0x467eecd14c5ea8eeULL,
    0xd3f6fc16ebca5e03ULL, 0x971da05074da7beeULL}},
  // (ten2mx >> 256) = 971da05074da7bee  d3f6fc16ebca5e03   
  //   467eecd14c5ea8eea48737a51a997a94
  {{0x3a71f2a1c428c420ULL, 0x70cb148213caa7e4ULL,
    0x5324c68b12dd6338ULL, 0xf1c90080baf72cb1ULL}},
  // (ten2mx >> 256) = f1c90080baf72cb1  5324c68b12dd6338   
  //   70cb148213caa7e43a71f2a1c428c420
  {{0x2ec18ee7d0209ce7ULL, 0x8d6f439b43088650ULL,
    0x75b7053c0f178293ULL, 0xc16d9a0095928a27ULL}},
  // (ten2mx >> 256) = c16d9a0095928a27  75b7053c0f178293   
  //   8d6f439b430886502ec18ee7d0209ce7
  {{0xf23472530ce6e3ecULL, 0xd78c3615cf3a050cULL,
    0xc4926a9672793542ULL, 0x9abe14cd44753b52ULL}},
  // (ten2mx >> 256) = 9abe14cd44753b52  c4926a9672793542   
  //   d78c3615cf3a050cf23472530ce6e3ec
  {{0xe9ed83b814a49fe0ULL, 0x8c1389bc7ec33b47ULL,
    0x3a83ddbd83f52204ULL, 0xf79687aed3eec551ULL}},
  // (ten2mx >> 256) = f79687aed3eec551  3a83ddbd83f52204   
  //   8c1389bc7ec33b47e9ed83b814a49fe0
  {{0x87f1362cdd507fe6ULL, 0x3cdc6e306568fc39ULL,
    0x95364afe032a819dULL, 0xc612062576589ddaULL}},
  // (ten2mx >> 256) = c612062576589dda  95364afe032a819d   
  //   3cdc6e306568fc3987f1362cdd507fe6
  {{0x9ff42b5717739985ULL, 0xca49f1c05120c9c7ULL,
    0x775ea264cf55347dULL, 0x9e74d1b791e07e48ULL}},
  // (ten2mx >> 256) = 9e74d1b791e07e48  775ea264cf55347d   
  //   ca49f1c05120c9c79ff42b5717739985
  {{0xccb9def1bf1f5c08ULL, 0x76dcb60081ce0fa5ULL,
    0x8bca9d6e188853fcULL, 0xfd87b5f28300ca0dULL}},
  // (ten2mx >> 256) = fd87b5f28300ca0d  8bca9d6e188853fc   
  //   76dcb60081ce0fa5ccb9def1bf1f5c08
  {{0xa3c7e58e327f7cd3ULL, 0x5f16f80067d80c84ULL,
    0x096ee45813a04330ULL, 0xcad2f7f5359a3b3eULL}},
  // (ten2mx >> 256) = cad2f7f5359a3b3e  96ee45813a04330   
  //   5f16f80067d80c84a3c7e58e327f7cd3
  {{0xb6398471c1ff970fULL, 0x18df2ccd1fe00a03ULL,
    0xa1258379a94d028dULL, 0xa2425ff75e14fc31ULL}},
  // (ten2mx >> 256) = a2425ff75e14fc31  a1258379a94d028d   
  //   18df2ccd1fe00a03b6398471c1ff970f
  {{0xf82e038e34cc78d9ULL, 0x4718f0a419800802ULL,
    0x80eacf948770ced7ULL, 0x81ceb32c4b43fcf4ULL}},
  // (ten2mx >> 256) = 81ceb32c4b43fcf4  80eacf948770ced7   
  //   4718f0a419800802f82e038e34cc78d9
  {{0x59e338e387ad8e28ULL, 0x0b5b1aa028ccd99eULL,
    0x67de18eda5814af2ULL, 0xcfb11ead453994baULL}},
  // (ten2mx >> 256) = cfb11ead453994ba  67de18eda5814af2   
  //   b5b1aa028ccd99e59e338e387ad8e28
  {{0x47e8fa4f9fbe0b53ULL, 0x6f7c154ced70ae18ULL,
    0xecb1ad8aeacdd58eULL, 0xa6274bbdd0fadd61ULL}},
  // (ten2mx >> 256) = a6274bbdd0fadd61  ecb1ad8aeacdd58e   
  //   6f7c154ced70ae1847e8fa4f9fbe0b53
  {{0xd320c83fb2fe6f75ULL, 0xbf967770bdf3be79ULL,
    0xbd5af13bef0b113eULL, 0x84ec3c97da624ab4ULL}},
  // (ten2mx >> 256) = 84ec3c97da624ab4  bd5af13bef0b113e   
  //   bf967770bdf3be79d320c83fb2fe6f75
  {{0x85014065eb30b256ULL, 0x65bd8be79652ca5cULL,
    0x955e4ec64b44e864ULL, 0xd4ad2dbfc3d07787ULL}},
  // (ten2mx >> 256) = d4ad2dbfc3d07787  955e4ec64b44e864   
  //   65bd8be79652ca5c85014065eb30b256
  {{0xd0cdcd1e55c08eabULL, 0xeafe098611dbd516ULL,
    0xdde50bd1d5d0b9e9ULL, 0xaa242499697392d2ULL}},
  // (ten2mx >> 256) = aa242499697392d2  dde50bd1d5d0b9e9   
  //   eafe098611dbd516d0cdcd1e55c08eab
  {{0x40a4a418449a0bbcULL, 0xbbfe6e04db164412ULL,
    0x7e50d64177da2e54ULL, 0x881cea14545c7575ULL}},
  // (ten2mx >> 256) = 881cea14545c7575  7e50d64177da2e54   
  //   bbfe6e04db16441240a4a418449a0bbc
  {{0x9aa1068d3a9012c7ULL, 0x2cca49a15e8a0683ULL,
    0x96e7bd358c904a21ULL, 0xd9c7dced53c72255ULL}},
  // (ten2mx >> 256) = d9c7dced53c72255  96e7bd358c904a21   
  //   2cca49a15e8a06839aa1068d3a9012c7
  {{0x154d9ed7620cdbd2ULL, 0x8a3b6e1ab2080536ULL,
    0xabec975e0a0d081aULL, 0xae397d8aa96c1b77ULL}},
  // (ten2mx >> 256) = ae397d8aa96c1b77  abec975e0a0d081a   
  //   8a3b6e1ab2080536154d9ed7620cdbd2
  {{0x443e18ac4e70afdbULL, 0x3b62be7bc1a0042bULL,
    0x2323ac4b3b3da015ULL, 0x8b61313bbabce2c6ULL}},
  // (ten2mx >> 256) = 8b61313bbabce2c6  2323ac4b3b3da015   
  //   3b62be7bc1a0042b443e18ac4e70afdb
  {{0x6d30277a171ab2f8ULL, 0x5f0463f935ccd378ULL,
    0x6b6c46dec52f6688ULL, 0xdf01e85f912e37a3ULL}},
  // (ten2mx >> 256) = df01e85f912e37a3  6b6c46dec52f6688   
  //   5f0463f935ccd3786d30277a171ab2f8
  {{0x8a8cec61ac155bfaULL, 0x7f36b660f7d70f93ULL,
    0x55f038b237591ed3ULL, 0xb267ed1940f1c61cULL}},
  // (ten2mx >> 256) = b267ed1940f1c61c  55f038b237591ed3   
  //   7f36b660f7d70f938a8cec61ac155bfa
  {{0x3ba3f04e23444995ULL, 0xcc2bc51a5fdf3fa9ULL,
    0x77f3608e92adb242ULL, 0x8eb98a7a9a5b04e3ULL}},
  // (ten2mx >> 256) = 8eb98a7a9a5b04e3  77f3608e92adb242   
  //   cc2bc51a5fdf3fa93ba3f04e23444995
  {{0xf9064d49d206dc21ULL, 0xe046082a32fecc41ULL,
    0x8cb89a7db77c506aULL, 0xe45c10c42a2b3b05ULL}},
  // (ten2mx >> 256) = e45c10c42a2b3b05  8cb89a7db77c506a   
  //   e046082a32fecc41f9064d49d206dc21
  {{0xfa6b7107db38b01aULL, 0x4d04d354f598a367ULL,
    0x3d607b97c5fd0d22ULL, 0xb6b00d69bb55c8d1ULL}},
  // (ten2mx >> 256) = b6b00d69bb55c8d1  3d607b97c5fd0d22   
  //   4d04d354f598a367fa6b7107db38b01a
  {{0xfb8927397c2d59afULL, 0x3d9d75dd9146e91fULL,
    0xcab3961304ca70e8ULL, 0x9226712162ab070dULL}},
  // (ten2mx >> 256) = 9226712162ab070d  cab3961304ca70e8   
  //   3d9d75dd9146e91ffb8927397c2d59af
  {{0xf8db71f5937bc2b1ULL, 0xc8fbefc8e87174ffULL,
    0xaab8f01e6e10b4a6ULL, 0xe9d71b689dde71afULL}},
  // (ten2mx >> 256) = e9d71b689dde71af  aab8f01e6e10b4a6   
  //   c8fbefc8e87174fff8db71f5937bc2b1
  {{0x2d7c5b2adc630227ULL, 0x3a63263a538df733ULL,
    0x5560c018580d5d52ULL, 0xbb127c53b17ec159ULL}},
  // (ten2mx >> 256) = bb127c53b17ec159  5560c018580d5d52   
  //   3a63263a538df7332d7c5b2adc630227
  {{0x24637c2249e8ce86ULL, 0x2eb5b82ea93e5f5cULL,
    0xdde7001379a44aa8ULL, 0x95a8637627989aadULL}},
  // (ten2mx >> 256) = 95a8637627989aad  dde7001379a44aa8   
  //   2eb5b82ea93e5f5c24637c2249e8ce86
  {{0x3a38c69d430e173dULL, 0x4abc59e441fd6560ULL,
    0x963e66858f6d4440ULL, 0xef73d256a5c0f77cULL}},
  // (ten2mx >> 256) = ef73d256a5c0f77c  963e66858f6d4440   
  //   4abc59e441fd65603a38c69d430e173d
  {{0x94fa387dcf3e78fdULL, 0x6efd14b69b311de6ULL,
    0xde98520472bdd033ULL, 0xbf8fdb78849a5f96ULL}},
  // (ten2mx >> 256) = bf8fdb78849a5f96  de98520472bdd033   
  //   6efd14b69b311de694fa387dcf3e78fd
  {{0xaa61c6cb0c31fa64ULL, 0x259743c548f417ebULL,
    0xe546a8038efe4029ULL, 0x993fe2c6d07b7fabULL}},
  // (ten2mx >> 256) = 993fe2c6d07b7fab  e546a8038efe4029   
  //   259743c548f417ebaa61c6cb0c31fa64
  {{0xaa360ade79e990a1ULL, 0x3c25393ba7ecf312ULL,
    0xd53dd99f4b3066a8ULL, 0xf53304714d9265dfULL}},
  // (ten2mx >> 256) = f53304714d9265df  d53dd99f4b3066a8   
  //   3c25393ba7ecf312aa360ade79e990a1
  {{0x882b3be52e5473b4ULL, 0x96842dc95323f5a8ULL,
    0xaa97e14c3c26b886ULL, 0xc428d05aa4751e4cULL}},
  // (ten2mx >> 256) = c428d05aa4751e4c  aa97e14c3c26b886   
  //   96842dc95323f5a8882b3be52e5473b4
  {{0xd355c98425105c90ULL, 0xab9cf16ddc1cc486ULL,
    0x55464dd69685606bULL, 0x9ced737bb6c4183dULL}},
  // (ten2mx >> 256) = 9ced737bb6c4183d  55464dd69685606b   
  //   ab9cf16ddc1cc486d355c98425105c90
  {{0xebbc75a03b4d60e6ULL, 0xac2e4f162cfad40aULL,
    0xeed6e2f0f0d56712ULL, 0xfb158592be068d2eULL}},
  // (ten2mx >> 256) = fb158592be068d2e  eed6e2f0f0d56712   
  //   ac2e4f162cfad40aebbc75a03b4d60e6
  {{0x8963914cfc3de71eULL, 0x568b727823fbdcd5ULL,
    0xf245825a5a445275ULL, 0xc8de047564d20a8bULL}},
  // (ten2mx >> 256) = c8de047564d20a8b  f245825a5a445275   
  //   568b727823fbdcd58963914cfc3de71e
  {{0xd44fa770c9cb1f4bULL, 0x453c5b934ffcb0aaULL,
    0x5b6aceaeae9d0ec4ULL, 0xa0b19d2ab70e6ed6ULL}},
  // (ten2mx >> 256) = a0b19d2ab70e6ed6  5b6aceaeae9d0ec4   
  //   453c5b934ffcb0aad44fa770c9cb1f4b
  {{0xdd0c85f3d4a27f6fULL, 0x37637c75d996f3bbULL,
    0xe2bbd88bbee40bd0ULL, 0x808e17555f3ebf11ULL}},
  // (ten2mx >> 256) = 808e17555f3ebf11  e2bbd88bbee40bd0   
  //   37637c75d996f3bbdd0c85f3d4a27f6f
  {{0x61ada31fba9d98b2ULL, 0x256bfa5628f185f9ULL,
    0x3792f412cb06794dULL, 0xcdb02555653131b6ULL}},
  // (ten2mx >> 256) = cdb02555653131b6  3792f412cb06794d   
  //   256bfa5628f185f961ada31fba9d98b2
  {{0xe7be1c196217ad5bULL, 0x51232eab53f46b2dULL,
    0x5fa8c3423c052dd7ULL, 0xa48ceaaab75a8e2bULL}},
  // (ten2mx >> 256) = a48ceaaab75a8e2b  5fa8c3423c052dd7   
  //   51232eab53f46b2de7be1c196217ad5b
  {{0x52fe7ce11b462449ULL, 0x40e8f222a99055beULL,
    0x1953cf68300424acULL, 0x83a3eeeef9153e89ULL}},
  // (ten2mx >> 256) = 83a3eeeef9153e89  1953cf68300424ac   
  //   40e8f222a99055be52fe7ce11b462449
  {{0x51972e34f8703a0fULL, 0x34a7e9d10f4d55fdULL,
    0x8eec7f0d19a03aadULL, 0xd29fe4b18e88640eULL}},
  // (ten2mx >> 256) = d29fe4b18e88640e  8eec7f0d19a03aad   
  //   34a7e9d10f4d55fd51972e34f8703a0f
  {{0x0e128b5d938cfb3fULL, 0x2a1fee40d90aab31ULL,
    0x3f2398d747b36224ULL, 0xa87fea27a539e9a5ULL}},
  // (ten2mx >> 256) = a87fea27a539e9a5  3f2398d747b36224   
  //   2a1fee40d90aab310e128b5d938cfb3f
  {{0x3e753c4adc70c8ffULL, 0xbb4cbe9a473bbc27ULL,
    0x98e947129fc2b4e9ULL, 0x86ccbb52ea94baeaULL}},
  // (ten2mx >> 256) = 86ccbb52ea94baea  98e947129fc2b4e9   
  //   bb4cbe9a473bbc273e753c4adc70c8ff
  {{0x30bb93aafa4e0e65ULL, 0x9214642a0b92c6a5ULL,
    0x5b0ed81dcc6abb0fULL, 0xd7adf884aa879177ULL}},
  // (ten2mx >> 256) = d7adf884aa879177  5b0ed81dcc6abb0f   
  //   9214642a0b92c6a530bb93aafa4e0e65
  {{0xc0960fbbfb71a51eULL, 0xa8105021a2dbd21dULL,
    0xe272467e3d222f3fULL, 0xac8b2d36eed2dac5ULL}},
  // (ten2mx >> 256) = ac8b2d36eed2dac5  e272467e3d222f3f   
  //   a8105021a2dbd21dc0960fbbfb71a51e
  {{0x66de72fcc927b74bULL, 0xb9a6a6814f1641b1ULL,
    0x1b8e9ecb641b58ffULL, 0x8a08f0f8bf0f156bULL}},
  // (ten2mx >> 256) = 8a08f0f8bf0f156b  1b8e9ecb641b58ff   
  //   b9a6a6814f1641b166de72fcc927b74b
  {{0xd7ca5194750c5878ULL, 0xf5d770cee4f0691bULL,
    0xf8e431456cf88e65ULL, 0xdcdb1b2798182244ULL}},
  // (ten2mx >> 256) = dcdb1b2798182244  f8e431456cf88e65   
  //   f5d770cee4f0691bd7ca5194750c5878
  {{0xdfd50e105da379f9ULL, 0x9179270bea59edafULL,
    0x2d835a9df0c6d851ULL, 0xb0af48ec79ace837ULL}},
  // (ten2mx >> 256) = b0af48ec79ace837  2d835a9df0c6d851   
  //   9179270bea59edafdfd50e105da379f9
  {{0x19773e737e1c6194ULL, 0x0dfa85a321e18af3ULL,
    0x579c487e5a38ad0eULL, 0x8d590723948a535fULL}},
  // (ten2mx >> 256) = 8d590723948a535f  579c487e5a38ad0e   
  //   dfa85a321e18af319773e737e1c6194
  {{0xf58b971f302d68eeULL, 0x165da29e9c9c1184ULL,
    0x25c6da63c38de1b0ULL, 0xe2280b6c20dd5232ULL}},
  // (ten2mx >> 256) = e2280b6c20dd5232  25c6da63c38de1b0   
  //   165da29e9c9c1184f58b971f302d68ee
  {{0xc46fac18f3578724ULL, 0x4517b54bb07cdad0ULL,
    0x1e38aeb6360b1af3ULL, 0xb4ecd5f01a4aa828ULL}},
  // (ten2mx >> 256) = b4ecd5f01a4aa828  1e38aeb6360b1af3   
  //   4517b54bb07cdad0c46fac18f3578724
  {{0x36bfbce0c2ac6c1dULL, 0x9dac910959fd7bdaULL,
    0xb1c6f22b5e6f48c2ULL, 0x90bd77f3483bb9b9ULL}},
  // (ten2mx >> 256) = 90bd77f3483bb9b9  b1c6f22b5e6f48c2   
  //   9dac910959fd7bda36bfbce0c2ac6c1d
  {{0x2465fb01377a4695ULL, 0x2f7a81a88ffbf95dULL,
    0xb60b1d1230b20e04ULL, 0xe7958cb87392c2c2ULL}}
  // (ten2mx >> 256) = e7958cb87392c2c2  b60b1d1230b20e04   
  //   2f7a81a88ffbf95d2465fb01377a4695
};
