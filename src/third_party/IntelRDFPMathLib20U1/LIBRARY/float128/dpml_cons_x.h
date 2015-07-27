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



#if INSTANTIATE_DEFINES

#   define ZERO			0
#   define ONE			1
#   define TWO			2
#   define PI			3
#   define PI_OVER_2		4
#   define PI_OVER_4			5
#   define THREE_PI_OVER_4		6
#   define NINETY		7
#   define ONE_EIGHTY		8
#   define INF			9
#   define LAST_CONS_INDEX	10

#endif

#if INSTANTIATE_TABLE

    const TABLE_UNION PACKED_CONSTANT_TABLE[] = {

	/* 00 */ DATA_2x2( 0x00000000, 0x00000000, 0x00000000, 0x00000000 ),
	/* 01 */ DATA_4R( 0x00000000, 0x00000000, 0x00000000, 0x3fff0000 ),
	/* 02 */ DATA_4R( 0x00000000, 0x00000000, 0x00000000, 0x40000000 ),
	/* 03 */ DATA_4R( 0xc51701b8, 0x8469898c, 0xb54442d1, 0x4000921f ),
	/* 04 */ DATA_4R( 0xc51701b8, 0x8469898c, 0xb54442d1, 0x3fff921f ),
	/* 05 */ DATA_4R( 0xc51701b8, 0x8469898c, 0xb54442d1, 0x3ffe921f ),
	/* 06 */ DATA_4R( 0x93d1414a, 0x234f2729, 0xc7f3321d, 0x40002d97 ),
	/* 07 */ DATA_4R( 0x00000000, 0x00000000, 0x00000000, 0x40056800 ),
	/* 08 */ DATA_4R( 0x00000000, 0x00000000, 0x00000000, 0x40066800 ),
	/* 09 */ DATA_4R( 0x00000000, 0x00000000, 0x00000000, 0x7fff0000 ),
	};

#endif
