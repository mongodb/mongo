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




#include "dpml_private.h"

#define CBRT_TABLE_NAME __dpml_bid_cbrt_t_table

#if !TABLE_IS_EXTERNAL

    const unsigned int CBRT_TABLE_NAME[] = { 

	/* 1.0 in double precision */
	/* 000 */ DATA_1x2( 0x00000000, 0x3ff00000 ),

	/* coeffs to approx cbrt(f) */
	/* 008 */ DATA_1x2( 0x39cf22de, 0x3fd929ac ),
	/* 016 */ DATA_1x2( 0x87730846, 0x3ff40b90 ),
	/* 024 */ DATA_1x2( 0xd7a3e848, 0xbff68ee7 ),
	/* 032 */ DATA_1x2( 0x80541937, 0x3ff6fbd1 ),
	/* 040 */ DATA_1x2( 0xdf5da719, 0xbff16685 ),
	/* 048 */ DATA_1x2( 0x1e6f904c, 0x3fe2d9c2 ),
	/* 056 */ DATA_1x2( 0xe1c69901, 0xbfcc5a29 ),
	/* 064 */ DATA_1x2( 0xda48850f, 0x3fac1c77 ),
	/* 072 */ DATA_1x2( 0xdc0d2f07, 0xbf8086db ),
	/* 080 */ DATA_1x2( 0x917c7fe0, 0x3f417636 ),

	/* coeffs to approx 1/cbrt(f)^2 */
	/* 088 */ DATA_1x2( 0x881b89ed, 0x400e1506 ),
	/* 096 */ DATA_1x2( 0xc23a3b85, 0xc020ed35 ),
	/* 104 */ DATA_1x2( 0x7264fecc, 0x4029d89a ),
	/* 112 */ DATA_1x2( 0xae7a02bd, 0xc02a0b85 ),
	/* 120 */ DATA_1x2( 0xb037ffa0, 0x402196ed ),
	/* 128 */ DATA_1x2( 0xec98f3cd, 0xc00fa3c4 ),
	/* 136 */ DATA_1x2( 0x7b6b49c4, 0x3ff2394e ),
	/* 144 */ DATA_1x2( 0xc4f387a6, 0xbfc85e9d ),
	/* 152 */ DATA_1x2( 0x3810ace9, 0x3f8cce2f ),

	/* cube roots of 2^i, i = 0,1,2  in full and lo */

	/* 160 */ DATA_1x2( 0x00000000, 0x3ff00000 ),
	/* 168 */ DATA_1x2( 0x00000000, 0x20000000 ),
	/* 176 */ DATA_1x2( 0xf98d728b, 0x3ff428a2 ),
	/* 184 */ DATA_1x2( 0xc4400000, 0x3deae515 ),
	/* 192 */ DATA_1x2( 0xa53d6e3d, 0x3ff965fe ),
	/* 200 */ DATA_1x2( 0x82b00000, 0x3dfd6e3c ),

	/* Numerical constants */
	/* 208 */ DATA_1x2( 0x00000000, 0x42d00000 ),
	/* 216 */ DATA_1x2( 0x38e38e39, 0x3fe8e38e ),
	/* 224 */ DATA_1x2( 0x92492492, 0x3fc24924 ),
	/* 232 */ DATA_1x2( 0xb6db6db7, 0x3fe6db6d ),
	/* 240 */ DATA_1x2( 0x00000000, 0x402c0000 ),
	/* 248 */ DATA_1x2( 0x00000000, 0x401c0000 ),
	/* 256 */ DATA_1x2( 0x1c71c71c, 0x3fbc71c7 ),
	};


#else

 extern const TABLE_UNION CBRT_TABLE_NAME[66]; 

#endif

#define	ONE_D	*((double *) ((char *)CBRT_TABLE_NAME + 0))
#define	CBRT_POLY_ADDR	((double *) ((char *)CBRT_TABLE_NAME + 8))
#define	REC_CBRT_POLY_ADDR	((double *) ((char *)CBRT_TABLE_NAME + 88))
#define OFFSET_OF_CBRTS_OF_2  160 
#define	BIG_QUAD	*((double *) ((char *)CBRT_TABLE_NAME + 208))
#define	SEVEN_NINTHS	*((double *) ((char *)CBRT_TABLE_NAME + 216))
#define	ONE_SEVENTH	*((double *) ((char *)CBRT_TABLE_NAME + 224))
#define	FIVE_SEVENTHS	*((double *) ((char *)CBRT_TABLE_NAME + 232))
#define	FOURTEEN	*((double *) ((char *)CBRT_TABLE_NAME + 240))
#define	SEVEN	*((double *) ((char *)CBRT_TABLE_NAME + 248))
#define	NINTH	*((double *) ((char *)CBRT_TABLE_NAME + 256))

# define CBRT_POLY_M(x) ((((CBRT_POLY_ADDR[0]+x*CBRT_POLY_ADDR[1])+(x*x)*CBRT_POLY_ADDR[2])+(x*(x*x))*(CBRT_POLY_ADDR[3] \
	+x*CBRT_POLY_ADDR[4]))+((x*x)*(x*(x*x)))*(((CBRT_POLY_ADDR[5]+x*CBRT_POLY_ADDR[6])+(x*x)*CBRT_POLY_ADDR[7]) \
	+(x*(x*x))*(CBRT_POLY_ADDR[8]+x*CBRT_POLY_ADDR[9])))
# define CBRT_POLY_C(x) (CBRT_POLY_ADDR[0]+x*(CBRT_POLY_ADDR[1]+x*(CBRT_POLY_ADDR[2]+x*(CBRT_POLY_ADDR[3] \
	+x*(CBRT_POLY_ADDR[4]+x*(CBRT_POLY_ADDR[5]+x*(CBRT_POLY_ADDR[6]+x*(CBRT_POLY_ADDR[7] \
	+x*(CBRT_POLY_ADDR[8]+x*CBRT_POLY_ADDR[9])))))))))
# define CBRT_POLY SELECT_POLY(CBRT_POLY_)

# define RECIP_CBRT_POLY_M(x) (((REC_CBRT_POLY_ADDR[0]+x*REC_CBRT_POLY_ADDR[1])+(x*x)*(REC_CBRT_POLY_ADDR[2]+x*REC_CBRT_POLY_ADDR[3])) \
	+((x*x)*(x*x))*(((REC_CBRT_POLY_ADDR[4]+x*REC_CBRT_POLY_ADDR[5])+(x*x)*REC_CBRT_POLY_ADDR[6])+(x*(x*x))*(REC_CBRT_POLY_ADDR[7] \
	+x*REC_CBRT_POLY_ADDR[8])))
# define RECIP_CBRT_POLY_C(x) (REC_CBRT_POLY_ADDR[0]+x*(REC_CBRT_POLY_ADDR[1]+x*(REC_CBRT_POLY_ADDR[2]+x*(REC_CBRT_POLY_ADDR[3] \
	+x*(REC_CBRT_POLY_ADDR[4]+x*(REC_CBRT_POLY_ADDR[5]+x*(REC_CBRT_POLY_ADDR[6]+x*(REC_CBRT_POLY_ADDR[7] \
	+x*REC_CBRT_POLY_ADDR[8]))))))))
# define RECIP_CBRT_POLY SELECT_POLY(RECIP_CBRT_POLY_)

