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

    static const TABLE_UNION __erf_t_table[] = { 

	/* 2/sqrt(pi) - 1, 8 and 1/8 */
	/* 000 */ DATA_1x2( 0x8214db69, 0x3fc06eba ),
	/* 008 */ DATA_1x2( 0x00000000, 0x40200000 ),
	/* 016 */ DATA_1x2( 0x00000000, 0x3fc00000 ),

	/* erf_poly_coefs */
	/* 024 */ DATA_1x2( 0x8214db68, 0x3fc06eba ),
	/* 032 */ DATA_1x2( 0x6b0379d7, 0xbfd81274 ),
	/* 040 */ DATA_1x2( 0x1a041744, 0x3fbce2f2 ),
	/* 048 */ DATA_1x2( 0x311dc6de, 0xbf9b82ce ),
	/* 056 */ DATA_1x2( 0xce0a2da1, 0x3f7565bc ),
	/* 064 */ DATA_1x2( 0x5ffe8f72, 0xbf4c02da ),
	/* 072 */ DATA_1x2( 0x9fa4ab37, 0x3f1f9a08 ),
	/* 080 */ DATA_1x2( 0xc7f1f6bd, 0xbeef484c ),
	/* 088 */ DATA_1x2( 0x9fc13017, 0x3ebb46e6 ),
	/* 096 */ DATA_1x2( 0xeb9ff2e4, 0xbe827e83 ),

	/* erfc_poly_coefs */
	/* 104 */ DATA_1x2( 0x8214db5f, 0x3fc06eba ),
	/* 112 */ DATA_1x2( 0x50428ad8, 0xbfe20dd7 ),
	/* 120 */ DATA_1x2( 0xf83622e8, 0x3feb14c2 ),
	/* 128 */ DATA_1x2( 0xcaa86b40, 0xc000ecf9 ),
	/* 136 */ DATA_1x2( 0x190ca828, 0x401d9eae ),
	/* 144 */ DATA_1x2( 0x347768e2, 0xc040a8c8 ),
	/* 152 */ DATA_1x2( 0xdd844934, 0x4066dd4b ),
	/* 160 */ DATA_1x2( 0xa98aebab, 0xc09242bc ),
	/* 168 */ DATA_1x2( 0x1c9651bb, 0x40bf1c16 ),
	/* 176 */ DATA_1x2( 0x0cb29ff7, 0xc0e74cd5 ),
	/* 184 */ DATA_1x2( 0x2a73c822, 0x4104919c ),

	/* exp_poly_coefs */
	/* 192 */ DATA_1x2( 0x00000000, 0x3ff00000 ),
	/* 200 */ DATA_1x2( 0xfffffffb, 0xbfdfffff ),
	/* 208 */ DATA_1x2( 0x5545c0c3, 0x3fc55555 ),
	/* 216 */ DATA_1x2( 0x29d0ed6b, 0xbfa55538 ),

	/* erfc_num_coefs */
	/* 224 */ DATA_1x2( 0xffe79cf3, 0x401bffff ),
	/* 232 */ DATA_1x2( 0xa989a44c, 0x40263831 ),
	/* 240 */ DATA_1x2( 0xd0373e5f, 0x4020c4f1 ),
	/* 248 */ DATA_1x2( 0x59b3d5b9, 0x400cae45 ),
	/* 256 */ DATA_1x2( 0x03e63df7, 0x3fe9cd95 ),
	/* 264 */ DATA_1x2( 0x441b612f, 0x3f926090 ),
	/* 272 */ DATA_1x2( 0x9728084a, 0xbfa60aad ),
	/* 280 */ DATA_1x2( 0x34324425, 0xbf8813c7 ),
	/* 288 */ DATA_1x2( 0x5f9d0ff9, 0xbf531d7a ),

	/* erfc_den_coefs */
	/* 296 */ DATA_1x2( 0x00000000, 0x3ff00000 ),
	/* 304 */ DATA_1x2( 0xd7801f25, 0x40070372 ),
	/* 312 */ DATA_1x2( 0xfb4b2616, 0x400e1e02 ),
	/* 320 */ DATA_1x2( 0x0e3e418e, 0x40078329 ),
	/* 328 */ DATA_1x2( 0xd8fa46c6, 0x3ff812aa ),
	/* 336 */ DATA_1x2( 0x94d3d4f6, 0x3fe0a7f9 ),
	/* 344 */ DATA_1x2( 0x030763e1, 0x3fbeb086 ),
	/* 352 */ DATA_1x2( 0x37f6be5c, 0x3f916e50 ),
	/* 360 */ DATA_1x2( 0x668fd10f, 0x3f531d7a ),
	};


#define	TWO_OVER_SQRT_PI_M1	*((double *) ((char *)__erf_t_table + 0))
#define	EIGHT	*((double *) ((char *)__erf_t_table + 8))
#define	ONE_EIGTH	*((double *) ((char *)__erf_t_table + 16))
#define	ERF_POLY_COEFS	((double *) ((char *)__erf_t_table + 24))
#define	ERFC_POLY_COEFS	((double *) ((char *)__erf_t_table + 104))
#define	EXP_POLY_COEFS	((double *) ((char *)__erf_t_table + 192))
#define	ERFC_NUM_COEFS	((double *) ((char *)__erf_t_table + 224))
#define	ERFC_DEN_COEFS	((double *) ((char *)__erf_t_table + 296))
#define ERF_POLY(t,z)		POLY_9_ALL(t, ERF_POLY_COEFS, z)
#define ERFC_POLY(t,z)		POLY_10_ALL(t, ERFC_POLY_COEFS, z)
#define EXP_POLY(t,z)		POLY_3_ALL(t, EXP_POLY_COEFS, z)
#define ERFC_NUM_POLY(t,z)	POLY_8_ALL(t, ERFC_NUM_COEFS, z)
#define ERFC_DEN_POLY(t,z)	POLY_8_ALL(t, ERFC_DEN_COEFS, z)
#define MAX_POLY_ARG		0x3fe3c21ff5156423
#define MIN_ERF_POLY_ARG	0x3e43988e144022d1
#define MIN_ERFC_POLY_ARG	0x3c8c5bf891b4ef6a
#define ERFC_MAX_CONSTANT_ARG	(0x4017afb48dc96626 - (U_WORD) 0x8000000000000000)
#define ERF_MIN_CONSTANT_ARG	0x4017afb48dc96626
#define MIN_ASYMTOTIC_ARG	0x4017afb48dc96626
#define MIN_UNDERFLOW_ARG	0x403b58df9656ccc3
