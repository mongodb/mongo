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



    static const TABLE_UNION __mod_x_table[] = { 

	/* mod class-to-action-mapping */
	/* 000 */ DATA_1x2( 0x00000408, 0x60000000 ),
	/* 008 */ DATA_1x2( 0x44332200, 0x50000055 ),
	/* 016 */ DATA_1x2( 0xbaeba449, 0x4efbebae ),
	/* 024 */ DATA_1x2( 0x41410449, 0x3efb0410 ),
	/* 032 */ DATA_1x2( 0x10410449, 0x2efb0414 ),
	/* 040 */ DATA_1x2( 0x10410449, 0x1efb4104 ),
	/* 048 */ DATA_1x2( 0x00000000, 0x00000000 ),
	/* 056 */ DATA_1x2( 0x0000003e, 0x00000000 ),
	/* 064 */ DATA_1x2( 0x0000003d, 0x00000000 ),

	/* rem class-to-action-mapping */
	/* 072 */ DATA_1x2( 0x00000408, 0x60000000 ),
	/* 080 */ DATA_1x2( 0x44332200, 0x50000055 ),
	/* 088 */ DATA_1x2( 0xbaeba449, 0x4efbebae ),
	/* 096 */ DATA_1x2( 0x41410449, 0x3efb0410 ),
	/* 104 */ DATA_1x2( 0x10410449, 0x2efb0414 ),
	/* 112 */ DATA_1x2( 0x10410449, 0x1efb4104 ),
	/* 120 */ DATA_1x2( 0x00000000, 0x00000000 ),
	/* 128 */ DATA_1x2( 0x0000005d, 0x00000000 ),
	/* 136 */ DATA_1x2( 0x0000005c, 0x00000000 ),

	/* remquo class-to-action-mapping */
	/* 144 */ DATA_1x2( 0x00000408, 0x60000000 ),
	/* 152 */ DATA_1x2( 0x44332200, 0x50000055 ),
	/* 160 */ DATA_1x2( 0xbaeba449, 0x4efbebae ),
	/* 168 */ DATA_1x2( 0x41410449, 0x3efb0410 ),
	/* 176 */ DATA_1x2( 0x10410449, 0x2efb0414 ),
	/* 184 */ DATA_1x2( 0x10410449, 0x1efb4104 ),
	/* 192 */ DATA_1x2( 0x00000000, 0x00000000 ),
	/* 200 */ DATA_1x2( 0x000000a5, 0x00000000 ),
	/* 208 */ DATA_1x2( 0x000000a4, 0x00000000 ),

	/* Unpacked constants 1/2 */
	/* 216 */ POS, 0000, DATA_2x2( 0x00000000, 0x80000000, 0x00000000, 0x00000000 ),

	/* 2^n, for n = -64, -26, -23, 0, 23, 53, 78, k-53, 2k-1, in double */
	/* 240 */ DATA_1x2( 0x00000000, 0x3bf00000 ),
	/* 248 */ DATA_1x2( 0x00000000, 0x3e500000 ),
	/* 256 */ DATA_1x2( 0x00000000, 0x3e800000 ),
	/* 264 */ DATA_1x2( 0x00000000, 0x3ff00000 ),
	/* 272 */ DATA_1x2( 0x00000000, 0x41600000 ),
	/* 280 */ DATA_1x2( 0x00000000, 0x43400000 ),
	/* 288 */ DATA_1x2( 0x00000000, 0x44d00000 ),
	/* 296 */ DATA_1x2( 0x00000000, 0x40a00000 ),
	/* 304 */ DATA_1x2( 0x00000000, 0x47e00000 ),
	};

#define	MOD_CLASS_TO_ACTION_MAP	((U_WORD const *) ((char *) __mod_x_table + 0))
#define	REM_CLASS_TO_ACTION_MAP	((U_WORD const *) ((char *) __mod_x_table + 72))
#define	REMQUO_CLASS_TO_ACTION_MAP	((U_WORD const *) ((char *) __mod_x_table + 144))
#define	UX_HALF			((UX_FLOAT *) ((char *) __mod_x_table + 216))
#define	D_RECIP_TWO_POW_64	*((double *) ((char *) __mod_x_table + 240))
#define	D_RECIP_TWO_POW_26	*((double *) ((char *) __mod_x_table + 248))
#define	D_RECIP_TWO_POW_23	*((double *) ((char *) __mod_x_table + 256))
#define	D_ONE			*((double *) ((char *) __mod_x_table + 264))
#define	D_TWO_POW_23		*((double *) ((char *) __mod_x_table + 272))
#define	D_TWO_POW_53		*((double *) ((char *) __mod_x_table + 280))
#define	D_TWO_POW_78		*((double *) ((char *) __mod_x_table + 288))
#define	D_TWO_POW_Km53		*((double *) ((char *) __mod_x_table + 296))
#define	D_TWO_POW_2Km1		*((double *) ((char *) __mod_x_table + 304))
