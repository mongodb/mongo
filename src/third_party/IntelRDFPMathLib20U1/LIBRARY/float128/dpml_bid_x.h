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

	/* Negate class-to-action-mapping */
	/* 000 */ DATA_1x2( 0x18618618, 0x06186186 ),

	/* Fabs class-to-action-mapping */
	/* 008 */ DATA_1x2( 0x10610410, 0x06106106 ),

	/* Nextafter class-to-action-mapping */
	/* 016 */ DATA_1x2( 0x00000408, 0x00000000 ),
	/* 024 */ DATA_1x2( 0x22222200, 0x00000022 ),
	/* 032 */ DATA_1x2( 0x00000449, 0x00000000 ),

	/* Multiply class-to-action-mapping */
	/* 040 */ DATA_1x2( 0x00000408, 0x50000000 ),
	/* 048 */ DATA_1x2( 0x34345500, 0x00000022 ),
	/* 056 */ DATA_1x2( 0x10eba449, 0x36106106 ),
	/* 064 */ DATA_1x2( 0x41659449, 0x26590410 ),
	/* 072 */ DATA_1x2( 0x41451449, 0x14510410 ),
	/* 080 */ DATA_1x2( 0x10610449, 0x0eba6106 ),

	/* Data for the above mapping */
	/* 088 */ DATA_1x2( 0x00000067, 0x00000000 ),

	/* Divide class-to-action-mapping */
	/* 096 */ DATA_1x2( 0x00000408, 0x50000000 ),
	/* 104 */ DATA_1x2( 0x34345500, 0x00000022 ),
	/* 112 */ DATA_1x2( 0x10610449, 0x3efb6106 ),
	/* 120 */ DATA_1x2( 0x4149a449, 0x2f3d0410 ),
	/* 128 */ DATA_1x2( 0x41692449, 0x1f7c0410 ),
	/* 136 */ DATA_1x2( 0x10fbe449, 0x06106106 ),

	/* Data for the above mapping */
	/* 144 */ DATA_1x2( 0x00000000, 0x00000000 ),
	/* 152 */ DATA_1x2( 0x00000067, 0x00000000 ),
	/* 160 */ DATA_1x2( 0x00000018, 0x00000000 ),
	/* 168 */ DATA_1x2( 0x00000035, 0x00000000 ),
	/* 176 */ DATA_1x2( 0x00000067, 0x00000000 ),

	/* Addition class-to-action-mapping */
	/* 184 */ DATA_1x2( 0x00000408, 0x50000000 ),
	/* 192 */ DATA_1x2( 0x33334500, 0x00000022 ),
	/* 200 */ DATA_1x2( 0x51451449, 0x34114514 ),
	/* 208 */ DATA_1x2( 0x41451449, 0x24100410 ),
	/* 216 */ DATA_1x2( 0x1047a449, 0x14104104 ),
	/* 224 */ DATA_1x2( 0x10e90449, 0x04104104 ),

	/* Data for the above mapping */
	/* 232 */ DATA_1x2( 0x00000067, 0x00000000 ),

	/* Subtraction class-to-action-mapping */
	/* 240 */ DATA_1x2( 0x00000408, 0x50000000 ),
	/* 248 */ DATA_1x2( 0x33334500, 0x00000022 ),
	/* 256 */ DATA_1x2( 0x59659449, 0x36506596 ),
	/* 264 */ DATA_1x2( 0x41659449, 0x24100410 ),
	/* 272 */ DATA_1x2( 0x10e90449, 0x14104104 ),
	/* 280 */ DATA_1x2( 0x1043a449, 0x04104104 ),

	/* Data for the above mapping */
	/* 288 */ DATA_1x2( 0x00000067, 0x00000000 ),

	/* 2^n, n = .5, 0, 24, 75, -24, -77 in double precision */
	/* 296 */ DATA_1x2( 0x667f3bcd, 0x3ff6a09e ),
	/* 304 */ DATA_1x2( 0x00000000, 0x3ff00000 ),
	/* 312 */ DATA_1x2( 0x00000000, 0x41700000 ),
	/* 320 */ DATA_1x2( 0x00000000, 0x44a00000 ),
	/* 328 */ DATA_1x2( 0x00000000, 0x3e700000 ),
	/* 336 */ DATA_1x2( 0x00000000, 0x3b200000 ),

	/* Rsqrt iteration (double precision) constants: 7/8 and 3/8 */
	/* 344 */ DATA_1x2( 0x00000000, 0x3fec0000 ),
	/* 352 */ DATA_1x2( 0x00000000, 0x3fd80000 ),

	/* 3 in unpacked format */
	/* 360 */ POS, 0002, DATA_2x2( 0x00000000, 0xc0000000, 0x00000000, 0x00000000 ),
	};

#define	NEGATE_CLASS_TO_ACTION_MAP	((U_WORD const *) ((char *) TABLE_NAME + 0))
#define	FABS_CLASS_TO_ACTION_MAP	((U_WORD const *) ((char *) TABLE_NAME + 8))
#define	NEXTAFTER_CLASS_TO_ACTION_MAP	((U_WORD const *) ((char *) TABLE_NAME + 16))
#define	MUL_CLASS_TO_ACTION_MAP	((U_WORD const *) ((char *) TABLE_NAME + 40))
#define	DIV_CLASS_TO_ACTION_MAP	((U_WORD const *) ((char *) TABLE_NAME + 96))
#define	ADDITION_CLASS_TO_ACTION_MAP	((U_WORD const *) ((char *) TABLE_NAME + 184))
#define	SUBTRACTION_CLASS_TO_ACTION_MAP	((U_WORD const *) ((char *) TABLE_NAME + 240))
#define	D_SQRT_TWO		*((double *) ((char *) TABLE_NAME + 296))
#define	D_ONE			*((double *) ((char *) TABLE_NAME + 304))
#define	D_TWO_POW_24		*((double *) ((char *) TABLE_NAME + 312))
#define	D_TWO_POW_75		*((double *) ((char *) TABLE_NAME + 320))
#define	D_RECIP_TWO_POW_24	*((double *) ((char *) TABLE_NAME + 328))
#define	D_RECIP_TWO_POW_77	*((double *) ((char *) TABLE_NAME + 336))
#define	D_SEVEN_EIGHTS	*((double *) ((char *) TABLE_NAME + 344))
#define	D_THREE_EIGHTS	*((double *) ((char *) TABLE_NAME + 352))
#define	UX_THREE			((UX_FLOAT *) ((char *) TABLE_NAME + 360))
