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



    static const TABLE_UNION __powi_x_table[] = { 

	/* powi class-to-action-mapping */

	/* ... for n < 0, even and odd */
	/* 000 */ DATA_1x2( 0x00514408, 0x7eba0000 ),
	/* 008 */ DATA_1x2( 0x00714408, 0x6efa0000 ),

	/* ... for n = 0, 0^0 = 0 */
	/* 016 */ DATA_1x2( 0x55555408, 0x55145555 ),

	/* ... for n = 0, 0^0 = 1 */
	/* 024 */ DATA_1x2( 0x55555408, 0x45555555 ),

	/* ... for n = 0, 0^0 = error */
	/* 032 */ DATA_1x2( 0x55555408, 0x3fff5555 ),

	/* ... for n > 0, even and odd */
	/* 040 */ DATA_1x2( 0x00610408, 0x26100000 ),
	/* 048 */ DATA_1x2( 0x00410408, 0x14100000 ),

	/* Data for the above mappings */
	/* 056 */ DATA_1x2( 0x00000000, 0x00000000 ),
	/* 064 */ DATA_1x2( 0x00000057, 0x00000000 ),
	/* 072 */ DATA_1x2( 0x00000058, 0x00000000 ),
	/* 080 */ DATA_1x2( 0x00000000, 0x00000000 ),
	/* 088 */ DATA_1x2( 0x00000001, 0x00000000 ),
	/* 096 */ DATA_1x2( 0x00000009, 0x00000000 ),
	/* 104 */ DATA_1x2( 0x00000056, 0x00000000 ),
	};

#define	POWI_CLASS_TO_ACTION_MAP	((U_WORD const *) ((char *) __powi_x_table + 0))
#define NEG_EXPONENT_INDEX			0
#define ZERO_EXPONENT_RETURN_0_INDEX		2
#define ZERO_EXPONENT_RETURN_1_INDEX		3
#define ZERO_EXPONENT_RETURN_ERROR_INDEX	4
#define POS_EXPONENT_INDEX			5
#define EXPONENT_INDEX_FIELD_WIDTH		3
