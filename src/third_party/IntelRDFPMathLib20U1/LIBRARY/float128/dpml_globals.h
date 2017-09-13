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



#if !defined(GLOBALS_TABLE)
#   define GLOBALS_TABLE	__INTERNAL_NAME(globals_table)
#endif

#ifdef GLOBAL_TABLE_VALUES

    const unsigned int GLOBALS_TABLE[] = { 
	S_NAN_HI, 0, 
	NAN_LO, T_NAN_HI, 
	NAN_LO, NAN_LO, NAN_LO, X_NAN_HI, 
	0x00000000, 0, 
	DATA_1x2( 0x00000000, 0x00000000 ), 
	DATA_2x2( 0x00000000, 0x00000000, 0x00000000, 0x00000000 ), 
	0x80000000, 0, 
	DATA_1x2( 0x00000000, 0x80000000 ), 
	DATA_4R( 0x00000000, 0x00000000, 0x00000000, 0x80000000 ), 
	0x00000001, 0, 
	DATA_1x2( 0x00000001, 0x00000000 ), 
	DATA_4R( 0x00000001, 0x00000000, 0x00000000, 0x00000000 ), 
	0x80000001, 0, 
	DATA_1x2( 0x00000001, 0x80000000 ), 
	DATA_4R( 0x00000001, 0x00000000, 0x00000000, 0x80000000 ), 
	0x7f7fffff, 0, 
	DATA_1x2( 0xffffffff, 0x7fefffff ), 
	DATA_4R( 0xffffffff, 0xffffffff, 0xffffffff, 0x7ffeffff ), 
	0xff7fffff, 0, 
	DATA_1x2( 0xffffffff, 0xffefffff ), 
	DATA_4R( 0xffffffff, 0xffffffff, 0xffffffff, 0xfffeffff ), 
	0x7f800000, 0, 
	DATA_1x2( 0x00000000, 0x7ff00000 ), 
	DATA_4R( 0x00000000, 0x00000000, 0x00000000, 0x7fff0000 ), 
	0xff800000, 0, 
	DATA_1x2( 0x00000000, 0xfff00000 ), 
	DATA_4R( 0x00000000, 0x00000000, 0x00000000, 0xffff0000 ), 
	0x34000000, 0, 
	DATA_1x2( 0x00000000, 0x3cb00000 ), 
	DATA_4R( 0x00000000, 0x00000000, 0x00000000, 0x3f8f0000 ), 
	0xb4000000, 0, 
	DATA_1x2( 0x00000000, 0xbcb00000 ), 
	DATA_4R( 0x00000000, 0x00000000, 0x00000000, 0xbf8f0000 ), 
	0x3f800000, 0, 
	DATA_1x2( 0x00000000, 0x3ff00000 ), 
	DATA_4R( 0x00000000, 0x00000000, 0x00000000, 0x3fff0000 ), 
	0xbf800000, 0, 
	DATA_1x2( 0x00000000, 0xbff00000 ), 
	DATA_4R( 0x00000000, 0x00000000, 0x00000000, 0xbfff0000 ), 
};

#else

	extern TABLE_UNION GLOBALS_TABLE[];
#endif


#define _s_TYPE 0
#define _t_TYPE 1
#define _x_TYPE 2
#define	NAN_INDEX	0
#define	POS_ZERO_INDEX	1
#define	NEG_ZERO_INDEX	2
#define	POS_TINY_INDEX	3
#define	NEG_TINY_INDEX	4
#define	POS_HUGE_INDEX	5
#define	NEG_HUGE_INDEX	6
#define	POS_INFINITY_INDEX	7
#define	NEG_INFINITY_INDEX	8
#define	POS_ULP_FACTOR_INDEX	9
#define	NEG_ULP_FACTOR_INDEX	10
#define	POS_ONE_INDEX	11
#define	NEG_ONE_INDEX	12
#define	F_TYPE_ENUM	PASTE_3(_, F_CHAR, _TYPE)
#define GLOBALS_OFFSET( t, n ) ( ( t << 3 ) + ( n << 5 ) )
#define	GLOBAL(n)	*((F_TYPE *) ((char *) GLOBALS_TABLE + GLOBALS_OFFSET(F_TYPE_ENUM,n) ))
#define	GLOBAL_ADDR(t,n)	((void *) ((char *) GLOBALS_TABLE + GLOBALS_OFFSET(t,n) ))
#define	NAN	GLOBAL(NAN_INDEX)
#define	POS_ZERO	GLOBAL(POS_ZERO_INDEX)
#define	NEG_ZERO	GLOBAL(NEG_ZERO_INDEX)
#define	POS_TINY	GLOBAL(POS_TINY_INDEX)
#define	NEG_TINY	GLOBAL(NEG_TINY_INDEX)
#define	POS_HUGE	GLOBAL(POS_HUGE_INDEX)
#define	NEG_HUGE	GLOBAL(NEG_HUGE_INDEX)
#define	POS_INFINITY	GLOBAL(POS_INFINITY_INDEX)
#define	NEG_INFINITY	GLOBAL(NEG_INFINITY_INDEX)
#define	POS_ULP_FACTOR	GLOBAL(POS_ULP_FACTOR_INDEX)
#define	NEG_ULP_FACTOR	GLOBAL(NEG_ULP_FACTOR_INDEX)
#define	POS_ONE	GLOBAL(POS_ONE_INDEX)
#define	NEG_ONE	GLOBAL(NEG_ONE_INDEX)
