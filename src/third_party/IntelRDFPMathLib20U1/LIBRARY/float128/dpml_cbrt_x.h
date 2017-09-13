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


    static const TABLE_UNION TABLE_NAME[] = { 

	/* Cbrt root class-to-action-mapping */
	/* 000 */ DATA_1x2( 0x00410408, 0x14100000 ),

	/* coefs to approx 1/cbrt(f)^2 */
	/* 008 */ DATA_1x2( 0x2e803c66, 0x4006ed4d ),
	/* 016 */ DATA_1x2( 0xc6230110, 0xc0102e13 ),
	/* 024 */ DATA_1x2( 0xa71af473, 0x400c33ee ),
	/* 032 */ DATA_1x2( 0xa7679244, 0xbffc42ef ),
	/* 040 */ DATA_1x2( 0x896ad7da, 0x3fde3d1a ),
	/* 048 */ DATA_1x2( 0x367e9ba1, 0xbfaad21e ),

	/* cube roots of 2^i, i = 0, 1, 2 */
	/* 056 */ DATA_1x2( 0x00000000, 0x3ff00000 ),
	/* 064 */ DATA_1x2( 0xf98d728b, 0x3ff428a2 ),
	/* 072 */ DATA_1x2( 0xa53d6e3d, 0x3ff965fe ),

	/* 14/9, 7/9 and 2/9 in double precision */
	/* 080 */ DATA_1x2( 0x38e38e39, 0x3ff8e38e ),
	/* 088 */ DATA_1x2( 0x38e38e39, 0x3fe8e38e ),
	/* 096 */ DATA_1x2( 0x1c71c71c, 0x3fcc71c7 ),
	};

#define	CBRT_CLASS_TO_ACTION_MAP	((U_WORD const *) ((char *) TABLE_NAME + 0))
#define	COEFS				((double *) ((char *) TABLE_NAME + 8))
#define	POW_CBRT_2_TABLE		((double *) ((char *) TABLE_NAME + 56))
#define	FOURTEEN_NINTHS			*((double *) ((char *) TABLE_NAME + 80))
#define	SEVEN_NINTHS			*((double *) ((char *) TABLE_NAME + 88))
#define	TWO_NINTHS			*((double *) ((char *) TABLE_NAME + 96))

# define RECIP_CBRT_POLY_M(x) (((COEFS[0]+x*COEFS[1])+(x*x)*COEFS[2])+(x*(x*x))*((COEFS[3] \
	+x*COEFS[4])+(x*x)*COEFS[5]))
# define RECIP_CBRT_POLY_C(x) (COEFS[0]+x*(COEFS[1]+x*(COEFS[2]+x*(COEFS[3] \
	+x*(COEFS[4]+x*COEFS[5])))))
# define RECIP_CBRT_POLY SELECT_POLY(RECIP_CBRT_POLY_)

