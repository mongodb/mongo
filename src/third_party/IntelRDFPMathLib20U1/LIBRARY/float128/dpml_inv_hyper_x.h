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

	/* asinh class-to-action-mapping */
	/* 000 */ DATA_1x2( 0x00410408, 0x14104100 ),

	/* acosh class-to-action-mapping */
	/* 008 */ DATA_1x2( 0x40e50408, 0x1e79e79e ),
	/* 016 */ DATA_1x2( 0x00000002, 0x00000000 ),

	/* atanh class-to-action-mapping */
	/* 024 */ DATA_1x2( 0x00e79408, 0x14104100 ),
	/* 032 */ DATA_1x2( 0x00000005, 0x00000000 ),
	/* 040 */ DATA_1x2( 0xf9de6484, 0xb504f333 ),
	/* 048 */ DATA_1x2( 0xfb66cb63, 0x87c3b666 ),
	/* 056 */ DATA_1x2( 0x6219b7ba, 0xafb0ccc0 ),

	/* Unpacked constants 1 and ln2 */
	/* 064 */ POS, 0001, DATA_2x2( 0x00000000, 0x80000000, 0x00000000, 0x00000000 ),
	/* 088 */ POS, 0000, DATA_2x2( 0xd1cf79ab, 0xb17217f7, 0x03f2f6af, 0xc9e3b398 ),
	};

#define	ASINH_CLASS_TO_ACTION_MAP	((U_WORD const *) ((char *) TABLE_NAME + 0))
#define	ACOSH_CLASS_TO_ACTION_MAP	((U_WORD const *) ((char *) TABLE_NAME + 8))
#define	ATANH_CLASS_TO_ACTION_MAP	((U_WORD const *) ((char *) TABLE_NAME + 24))
#define	SQRT_2_OV_4			*((UX_FRACTION_DIGIT_TYPE *) ((char *) TABLE_NAME + 40))
#define	THREE_SQRT_2_OV_4		*((UX_FRACTION_DIGIT_TYPE *) ((char *) TABLE_NAME + 48))
#define	SQRT_2_M1_SQR			*((UX_FRACTION_DIGIT_TYPE *) ((char *) TABLE_NAME + 56))
#define	UX_ONE				((UX_FLOAT *) ((char *) TABLE_NAME + 64))
#define	UX_LN2				((UX_FLOAT *) ((char *) TABLE_NAME + 88))
