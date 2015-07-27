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



    static const TABLE_UNION __lgamma_t_table[] = { 

	/* Miscelaneous constants */
	/* 000 */ DATA_1x2( 0x278b51a7, 0x7f5754d9 ),
	/* 008 */ DATA_1x2( 0x93c1bb66, 0x41630232 ),
	/* 016 */ DATA_1x2( 0xc864beb5, 0x3fed67f1 ),
	/* 024 */ DATA_1x2( 0x25aa1316, 0x3fcce6bb ),
	/* 032 */ DATA_1x2( 0x54442d18, 0x400921fb ),

	/* Rational Coefficents for Q(x) */
	/* 040 */ DATA_1x2( 0xe37db0c8, 0xbfb3c467 ),
	/* 048 */ DATA_1x2( 0x736b184d, 0x3fca92d0 ),
	/* 056 */ DATA_1x2( 0xd307f71e, 0x3fd64276 ),
	/* 064 */ DATA_1x2( 0xe0c4c055, 0x3fc6220c ),
	/* 072 */ DATA_1x2( 0x2b5a189c, 0x3fa23e88 ),
	/* 080 */ DATA_1x2( 0xd793fd1b, 0x3f67a815 ),
	/* 088 */ DATA_1x2( 0x062913a2, 0x3f104902 ),

	/* 096 */ DATA_1x2( 0x00000000, 0x3ff00000 ),
	/* 104 */ DATA_1x2( 0xd55bd332, 0x3ff7ccf9 ),
	/* 112 */ DATA_1x2( 0x220544da, 0x3feabd93 ),
	/* 120 */ DATA_1x2( 0xa77dd24b, 0x3fcc168b ),
	/* 128 */ DATA_1x2( 0xe906da19, 0x3f9b84eb ),
	/* 136 */ DATA_1x2( 0x853dd82d, 0x3f556e14 ),
	/* 144 */ DATA_1x2( 0x14128311, 0x3eef83a4 ),

	/* Polynomial Coefficents phi(x) */
	/* 152 */ DATA_1x2( 0x55555555, 0x3fb55555 ),
	/* 160 */ DATA_1x2( 0x16c14af4, 0xbf66c16c ),
	/* 168 */ DATA_1x2( 0x17e88ee8, 0x3f4a01a0 ),
	/* 176 */ DATA_1x2( 0xe14af2a1, 0xbf438134 ),
	/* 184 */ DATA_1x2( 0x2d63c94c, 0x3f4b92c9 ),
	/* 192 */ DATA_1x2( 0xa8dfce6f, 0xbf5ef97f ),
	/* 200 */ DATA_1x2( 0x735455ae, 0x3f74c37d ),
};


#define	OVERFLOW_THRESHOLD	*((double *) ((char *)__lgamma_t_table + 0))
#define	REAL_BIG	*((double *) ((char *)__lgamma_t_table + 8))
#define	HALF_LN_2_PI	*((double *) ((char *)__lgamma_t_table + 16))
#define	HALF_LN_PI_OVER_2	*((double *) ((char *)__lgamma_t_table + 24))
#define	PI	*((double *) ((char *)__lgamma_t_table + 32))
#define	P_COEFS	((double *) ((char *)__lgamma_t_table + 40))
#define	Q_COEFS	((double *) ((char *)__lgamma_t_table + 96))
#define	PHI_COEFS	((double *) ((char *)__lgamma_t_table + 152))
#define PHI(a,u)         u = a*a; u = a*POLY6(PHI_COEFS, u)
#define Q(x)             (POLY6(P_COEFS, x)/POLY6(Q_COEFS, x))