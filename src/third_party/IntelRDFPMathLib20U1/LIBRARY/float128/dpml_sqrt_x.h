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

	/* Square root class-to-action-mapping */
	/* 000 */ DATA_1x2( 0x40e50408, 0x2410e40e ),

	/* Reciprocal square root class-to-action-mapping */
	/* 008 */ DATA_1x2( 0x40e54408, 0x1efae40e ),

	/* Data for the above mapping */
	/* 016 */ DATA_1x2( 0x00000067, 0x00000000 ),
	/* 024 */ DATA_1x2( 0x00000068, 0x00000000 ),
	/* 032 */ DATA_1x2( 0x00000069, 0x00000000 ),
	/* 040 */ DATA_1x2( 0x00000000, 0x00000000 ),

	/* Hypot(x,y) root class-to-action-mapping */
	/* 048 */ DATA_1x2( 0x00000408, 0x10000000 ),
	/* 056 */ DATA_1x2( 0x33334400, 0x00000022 ),
	/* 064 */ DATA_1x2( 0x61861449, 0x18618618 ),
	/* 072 */ DATA_1x2( 0x41861449, 0x18200410 ),
	/* 080 */ DATA_1x2( 0x20820449, 0x18208208 ),

	/* 2^n, n = .5, 0, 24, 75, -24, -77 in double precision */
	/* 088 */ DATA_1x2( 0x667f3bcd, 0x3ff6a09e ),
	/* 096 */ DATA_1x2( 0x00000000, 0x3ff00000 ),
	/* 104 */ DATA_1x2( 0x00000000, 0x41700000 ),
	/* 112 */ DATA_1x2( 0x00000000, 0x44a00000 ),
	/* 120 */ DATA_1x2( 0x00000000, 0x3e700000 ),
	/* 128 */ DATA_1x2( 0x00000000, 0x3b200000 ),

	/* Rsqrt iteration (double precision) constants: 7/8 and 3/8 */
	/* 136 */ DATA_1x2( 0x00000000, 0x3fec0000 ),
	/* 144 */ DATA_1x2( 0x00000000, 0x3fd80000 ),

	/* 3 in unpacked format */
	/* 152 */ POS, 0002, DATA_2x2( 0x00000000, 0xc0000000, 0x00000000, 0x00000000 ),
	};

#define	SQRT_CLASS_TO_ACTION_MAP	((U_WORD const *) ((char *) TABLE_NAME + 0))
#define	RSQRT_CLASS_TO_ACTION_MAP	((U_WORD const *) ((char *) TABLE_NAME + 8))
#define	HYPOT_CLASS_TO_ACTION_MAP	((U_WORD const *) ((char *) TABLE_NAME + 48))
#define	D_SQRT_TWO		*((double *) ((char *) TABLE_NAME + 88))
#define	D_ONE			*((double *) ((char *) TABLE_NAME + 96))
#define	D_TWO_POW_24		*((double *) ((char *) TABLE_NAME + 104))
#define	D_TWO_POW_75		*((double *) ((char *) TABLE_NAME + 112))
#define	D_RECIP_TWO_POW_24	*((double *) ((char *) TABLE_NAME + 120))
#define	D_RECIP_TWO_POW_77	*((double *) ((char *) TABLE_NAME + 128))
#define	D_SEVEN_EIGHTS	*((double *) ((char *) TABLE_NAME + 136))
#define	D_THREE_EIGHTS	*((double *) ((char *) TABLE_NAME + 144))
#define	UX_THREE			((UX_FLOAT *) ((char *) TABLE_NAME + 152))
