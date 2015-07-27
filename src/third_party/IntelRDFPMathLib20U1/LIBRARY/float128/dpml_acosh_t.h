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


static const TABLE_UNION max_direct_x[] = { DATA_1x2( 0x02ccc470, 0x400fe8cf ) };

static const TABLE_UNION max_asym_x[] = { DATA_1x2( 0xd1a81cd8, 0x41a46ac2 ) };

#define EVALUATE_ASYM_RANGE_POLYNOMIAL(x,c,y) \
                         POLY_9(x,c,y)
static const TABLE_UNION asym_range_coef[] = { 
	DATA_1x2( 0x00000000, 0x3fd00000 ),
	DATA_1x2( 0xffffff07, 0x3fb7ffff ),
	DATA_1x2( 0xaaaddfbf, 0x3faaaaaa ),
	DATA_1x2( 0xfdf6faba, 0x3fa17fff ),
	DATA_1x2( 0x7de0c4ff, 0x3f993334 ),
	DATA_1x2( 0x3680db02, 0x3f933fc5 ),
	DATA_1x2( 0xa311158b, 0x3f8eb0ca ),
	DATA_1x2( 0x0a8b1f0a, 0x3f88674b ),
	DATA_1x2( 0x0da7b094, 0x3f8b082b ),
}; 

static const TABLE_UNION half_huge_x[] = { DATA_1x2( 0xffffffff, 0x7fdfffff ) };

static const TABLE_UNION log_2[] = {
        DATA_1x2( 0xfefa39ef, 0x3fe62e42 ) 
};

