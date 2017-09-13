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

	/* floor class-to-action-mapping */
	/* 000 */ DATA_1x2( 0x00410408, 0x44106910 ),

	/* ceil class-to-action-mapping */
	/* 008 */ DATA_1x2( 0x00410408, 0x34106520 ),

	/* trunc, nint, rint and modf class-to-action-mapping */
	/* 016 */ DATA_1x2( 0x00410408, 0x24104510 ),

	/* this class-to-action-mapping used by modf only */
	/* 024 */ DATA_1x2( 0x00451408, 0x14104100 ),

	/* data for the above class to action mappings */
	/* 032 */ DATA_1x2( 0x00000000, 0x00000000 ),
	/* 040 */ DATA_1x2( 0x00000001, 0x00000000 ),
	};

#define	FLOOR_CLASS_TO_ACTION_MAP	((U_WORD const *) ((char *) TABLE_NAME + 0))
#define	CEIL_CLASS_TO_ACTION_MAP	((U_WORD const *) ((char *) TABLE_NAME + 8))
#define	TRUNC_CLASS_TO_ACTION_MAP	((U_WORD const *) ((char *) TABLE_NAME + 16))
