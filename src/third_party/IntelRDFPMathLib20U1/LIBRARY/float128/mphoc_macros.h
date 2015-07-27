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

#ifndef MPHOC_MACROS_H
#define MPHOC_MACROS_H



#include "mp.h"


#ifndef MPHOC_EXECUTABLE
#   ifndef ENDIANESS
#       error  ENDIANESS is not defined
#   else
#	if (ENDIANESS == big_endian)
#		define MPHOC_EXECUTABLE mphoc -b
#	else
#		define MPHOC_EXECUTABLE mphoc
#	endif
#   endif
#endif

#ifndef GENPOLY_EXECUTABLE

#       define GENPOLY_EXECUTABLE genpoly

#endif



/* "MAX" and "MIN" denote a combination of magnitude and sign attributes. */

#define MPHOC_MAX_CHAR  ((2 ^ (BITS_PER_CHAR - 1)) - 1)
#define MPHOC_MAX_SHORT ((2 ^ (BITS_PER_SHORT - 1)) - 1)
#define MPHOC_MAX_INT   ((2 ^ (BITS_PER_INT - 1)) - 1)
#define MPHOC_MAX_LONG  ((2 ^ (BITS_PER_LONG - 1)) - 1)
#define MPHOC_MAX_WORD  ((2 ^ (BITS_PER_WORD - 1)) - 1)

#define MPHOC_MIN_CHAR  (-(2 ^ (BITS_PER_CHAR - 1)))
#define MPHOC_MIN_SHORT (-(2 ^ (BITS_PER_SHORT - 1)))
#define MPHOC_MIN_INT   (-(2 ^ (BITS_PER_INT - 1)))
#define MPHOC_MIN_LONG  (-(2 ^ (BITS_PER_LONG - 1)))
#define MPHOC_MIN_WORD  (-(2 ^ (BITS_PER_WORD - 1)))

#define MPHOC_MAX_U_CHAR  ((2 ^ BITS_PER_CHAR) - 1)
#define MPHOC_MAX_U_SHORT ((2 ^ BITS_PER_SHORT) - 1)
#define MPHOC_MAX_U_INT   ((2 ^ BITS_PER_INT) - 1)
#define MPHOC_MAX_U_LONG  ((2 ^ BITS_PER_LONG) - 1)
#define MPHOC_MAX_U_WORD  ((2 ^ BITS_PER_WORD) - 1)



/* "TINY" and "HUGE" denote only a magnitude attribute. */

#define MPHOC_F_POS_ZERO 0.0

#define MPHOC_F_POS_NORMAL_TINY (2 ^ (F_MIN_BIN_EXP + F_NORM))
#define MPHOC_F_NEG_NORMAL_TINY (-MPHOC_F_POS_NORMAL_TINY)

#if IEEE_FLOATING
#	define MPHOC_F_POS_TINY (MPHOC_F_POS_NORMAL_TINY / 2 ^ (F_PRECISION - 1))
#else
#	define MPHOC_F_POS_TINY MPHOC_F_POS_NORMAL_TINY
#endif
#define MPHOC_F_NEG_TINY (-MPHOC_F_POS_TINY)

#define MPHOC_F_POS_HUGE (2 ^ (F_MAX_BIN_EXP + F_NORM + 1) * (1 - 1 / (2 ^ F_PRECISION)))
#define MPHOC_F_NEG_HUGE (-MPHOC_F_POS_HUGE)



#if   (F_FORMAT == f_floating)

#	define MPHOC_F_NAN          f:00008000
#	define MPHOC_F_NEG_ZERO     f:00000000
#	define MPHOC_F_POS_INFINITY f:ffff7fff
#	define MPHOC_F_NEG_INFINITY f:ffffffff

#elif (F_FORMAT == d_floating)

#	define MPHOC_F_NAN          d:0000000000008000
#	define MPHOC_F_NEG_ZERO     d:0000000000000000
#	define MPHOC_F_POS_INFINITY d:ffffffffffff7fff
#	define MPHOC_F_NEG_INFINITY d:ffffffffffffffff

#elif (F_FORMAT == g_floating)

#	define MPHOC_F_NAN          g:0000000000008000
#	define MPHOC_F_NEG_ZERO     g:0000000000000000
#	define MPHOC_F_POS_INFINITY g:ffffffffffff7fff
#	define MPHOC_F_NEG_INFINITY g:ffffffffffffffff

#elif (F_FORMAT == h_floating)

#	error H_floating not supported.

#elif (F_FORMAT == s_floating)

#	define MPHOC_F_NAN          s:7fbfffff
#	define MPHOC_F_NEG_ZERO     s:80000000
#	define MPHOC_F_POS_INFINITY s:7f800000
#	define MPHOC_F_NEG_INFINITY s:ff800000


#elif (F_FORMAT == t_floating)

#	define MPHOC_F_NAN          t:7ff7ffffffffffff
#	define MPHOC_F_NEG_ZERO     t:8000000000000000
#	define MPHOC_F_POS_INFINITY t:7ff0000000000000
#	define MPHOC_F_NEG_INFINITY t:fff0000000000000

#elif (F_FORMAT == x_floating)

#	define MPHOC_F_NAN          x:7fff7fffffffffffffffffffffffffff
#	define MPHOC_F_NEG_ZERO     x:80000000000000000000000000000000
#	define MPHOC_F_POS_INFINITY x:7fff0000000000000000000000000000
#	define MPHOC_F_NEG_INFINITY x:ffff0000000000000000000000000000

#else

#	error Unsupported floating format.

#endif



#define MPHOC_F_POS_PI	 3.1415926535897932384626433832795028841972
#define MPHOC_F_NEG_PI	-3.1415926535897932384626433832795028841972

#define	MPHOC_F_POS_PI_OVER_2	 1.5707963267948966192313216916397514420986
#define	MPHOC_F_NEG_PI_OVER_2	-1.5707963267948966192313216916397514420986



/* Obsolete definitions */

#define	MP_MAX_POS_SIGNED_INT	(2^(BITS_PER_WORD - 1) - 1)
#define	MP_MAX_UNSIGNED_INT	(2^BITS_PER_WORD - 1)

#define MP_MIN_NORMAL_FLOAT	(2^(F_MIN_BIN_EXP + F_NORM))
#define MP_MAX_FLOAT		(2^(F_MAX_BIN_EXP + F_NORM + 1) * (1 - 1/2^F_PRECISION))

#ifdef VAX_FLOATING
#    define     MP_MIN_FLOAT    MP_MIN_NORMAL_FLOAT
#else
#    define     MP_MIN_FLOAT    (MP_MIN_NORMAL_FLOAT/2^(F_PRECISION - 1))
#endif

#define MPHOC_S_POS_NORMAL_TINY (2 ^ (S_MIN_BIN_EXP + F_NORM))
#define MPHOC_D_POS_NORMAL_TINY (2 ^ (D_MIN_BIN_EXP + F_NORM))

#if IEEE_FLOATING
#    define MPHOC_S_POS_TINY \
        ( MPHOC_S_POS_NORMAL_TINY / 2 ^ (S_PRECISION - 1) )
#    define MPHOC_D_POS_TINY \
        ( MPHOC_D_POS_NORMAL_TINY / 2 ^ (D_PRECISION - 1) )
#else
#    define MPHOC_S_POS_TINY   MPHOC_S_POS_NORMAL_TINY
#    define MPHOC_D_POS_TINY   MPHOC_D_POS_NORMAL_TINY
#endif

#define MPHOC_S_POS_HUGE \
   (2 ^ (S_MAX_BIN_EXP + F_NORM + 1) * (1 - 1 / (2 ^ S_PRECISION)))
#define MPHOC_D_POS_HUGE \
   (2 ^ (D_MAX_BIN_EXP + F_NORM + 1) * (1 - 1 / (2 ^ D_PRECISION)))


#define	BYTES(n)		((n) >> 3)

#define	START_STATIC_TABLE(name, offset) \
		printf("    static const TABLE_UNION " STR(name) "[] = { \n"); \
		offset = 0
#define	START_GLOBAL_TABLE(name, offset) \
		printf("    const " STR(TABLE_WORD) " " STR(name) "[] = { \n"); \
		offset = 0
#define	END_TABLE			printf("};\n\n")

#define	TABLE_COMMENT(s)		printf("\n\t/* " s " */\n")
#define	PRINT_1_TYPE_ENTRY(c,x,o)	printf("\t/* %3i */ %#.4" STR(c) ",\n", BYTES(o), x); \
		                        o += PASTE(BITS_PER_, c)
#define	PRINT_2_TYPE_ENTRY(c,x,y,o)	printf("\t/* %3i */ %#.4" STR(c) \
					  ", %#.4" STR(c) ", \n", BYTES(o), x, y); \
		                        o += 2*PASTE(BITS_PER_, c)

#define	PRINT_1_F_TYPE_ENTRY(x,o)	PRINT_1_TYPE_ENTRY(F_CHAR, x, o)
#define	PRINT_2_F_TYPE_ENTRY(x,y,o)	PRINT_2_TYPE_ENTRY(F_CHAR, x, y, o)
#define	PAD_IF_NEEDED(o, i)		while ((i)*floor(o/(i)) != o) { \
					    printf( "\t/* padding for alignment */ " \
					    "0x00000000,\n"); \
					    o += BITS_PER_TABLE_WORD; }
					

#define	BITS_PER_f		BITS_PER_FLOAT
#define	BITS_PER_s		BITS_PER_FLOAT
#define	BITS_PER_g		BITS_PER_DOUBLE
#define	BITS_PER_t		BITS_PER_DOUBLE
#define	BITS_PER_x		BITS_PER_LONG_DOUBLE
#define	BITS_PER_w		BITS_PER_WORD

#define	PRINT_TABLE_DEFINE(name,table,offset,type,xxx) \
		printf("#define\t" STR(name) "\t" xxx \
		"((" STR(type) " *) ((char *)" STR(table) \
		" + %i))\n", BYTES(offset))

#define	PRINT_TABLE_VALUE_DEFINE(name,table,offset,type) \
		PRINT_TABLE_DEFINE(name,table,offset,type,"*")

#define	PRINT_TABLE_ADDRESS_DEFINE(name,table,offset,type) \
		PRINT_TABLE_DEFINE(name,table,offset,type,"")

#define	BREAK_INTO_HI_LO(x,h,l,p)	h = TRUNCATE(x, p); l = x - h


/* Some global definitions for the remes program */

#define SET_REMES_ABSOLUTE_ERROR        remes_weight = 1
#define SET_REMES_RELATIVE_ERROR        remes_weight = 2
#define SET_REMES_GENERAL_ERROR         remes_weight = 3
#define SET_REMES_MODE_TO_STATIC        remes_mode = 1
#define SET_REMES_MODE_TO_FIND_POLY     remes_mode = 2

/*
 * Format specifiers for print integers in hex format
 */

#define	HEX_FORMAT_FOR_16_BITS	"0x%4.4.16i"
#define	HEX_FORMAT_FOR_32_BITS	"0x%8.8.16i"
#define	HEX_FORMAT_FOR_64_BITS	"0x%16.16.16i"


#if NEW_DPML_MACROS == 1

    /*
     * Set up default table name and offset for printing macros 
     */

#   if !defined(MP_TABLE_NAME)
#       define MP_TABLE_NAME	TABLE_NAME
#   endif

#   if !defined(MP_BIT_OFFSET)
#       define MP_BIT_OFFSET	offset
#   endif

#   if defined(MAKE_COMMON)
#       define _START_TABLE	START_GLOBAL_TABLE(MP_TABLE_NAME, MP_BIT_OFFSET)
#   else
#       define _START_TABLE	START_STATIC_TABLE(MP_TABLE_NAME, MP_BIT_OFFSET)
#   endif

#   if !defined(START_TABLE)
#       define START_TABLE	_START_TABLE
#   endif

#undef 	END_TABLE
#define	END_TABLE		printf("\t};\n\n")

#   if !defined(MP_T_TYPE)
#       define MP_T_TYPE	F_TYPE
#       define MP_T_CHAR	F_CHAR
#       define MP_T_PRECISION	F_PRECISION
#   endif

#   define	W_CHAR		w
#   define	U_CHAR		u

#   define BITS_PER_u	BITS_PER_WORD

#   define	f_TYPE		float
#   define	s_TYPE		float
#   define	g_TYPE		double
#   define	t_TYPE		double
#   define	x_TYPE		long double
#   define	w_TYPE		WORD
#   define	u_TYPE		U_WORD

#   define	f_FMT		"%#.4f"
#   define	s_FMT		"%#.4s"
#   define	g_FMT		"%#.4g"
#   define	t_FMT		"%#.4t"
#   define	x_FMT		"%#.4x"
#   if (BITS_PER_WORD <= 32)
#       define	w_FMT		"%#8.4.16i"
#       define	u_FMT		"%#8.4.16i"
#   else
#       define	w_FMT		"%#16.4.16i"
#       define	u_FMT		"%#16.4.16i"
#   endif

#   define	CHAR_TO_TYPE(tchar)	PASTE(tchar,_TYPE)
#   define	CHAR_TO_BITS(tchar)	PASTE(BITS_PER_, tchar)
#   define	CHAR_TO_FMT(tchar)	PASTE(tchar, _FMT)

#   define PRINT_TBL_DEF(name, table, offset, tchar, xxx) \
		 printf("#define\t" name "\t" xxx "((" STR(CHAR_TO_TYPE(tchar)) \
		    " *) ((char *) " STR(table) " + %i))\n", BYTES(offset))

#   define PRINT_TYPED_TBL_ITEM(v,tchar)			\
		printf( "\t/* %3i */ " CHAR_TO_FMT(tchar)	\
		   ",\n", BYTES(MP_BIT_OFFSET), v);		\
		MP_BIT_OFFSET += CHAR_TO_BITS(tchar)

#   define PRINT_TYPED_TBL_VDEF(name, tchar) \
		PRINT_TBL_DEF(name, MP_TABLE_NAME, MP_BIT_OFFSET, \
		    tchar, "*")

#   define PRINT_TYPED_TBL_ADEF(name, tchar)	\
		PRINT_TBL_DEF(name, MP_TABLE_NAME, MP_BIT_OFFSET, \
		    tchar, "") 

#   define PRINT_TYPED_TBL_VDEF_ITEM(n,v,tchar) \
		PRINT_TYPED_TBL_VDEF(n, tchar); PRINT_TYPED_TBL_ITEM(v, tchar)

#   define PRINT_TYPED_TBL_ADEF_ITEM(n,v,tchar) \
		PRINT_TYPED_TBL_ADEF(n, tchar); PRINT_TYPED_TBL_ITEM(v, tchar)

#   define PRINT_TYPED_COM_VDEF(c,n,tchar) \
		TABLE_COMMENT(c); PRINT_TYPED_TBL_VDEF(n,tchar)

#   define PRINT_TYPED_COM_ADEF(c,n,tchar) \
		TABLE_COMMENT(c); PRINT_TYPED_TBL_ADEF(n,tchar)

#   define PRINT_TYPED_TBL_COM_VDEF_ITEM(c,n,v,tchar) \
		TABLE_COMMENT(c); PRINT_TYPED_TBL_VDEF_ITEM(n,v,tchar)

#   define PRINT_TYPED_TBL_COM_ADEF_ITEM(c,n,v,tchar) \
		TABLE_COMMENT(c); PRINT_TYPED_BL_ADEF_ITEM(n,v,tchar)

#   define PRINT_TBL_ITEM(v)                PRINT_TYPED_TBL_ITEM(v, MP_T_CHAR)
#   define PRINT_TBL_VDEF(n)                PRINT_TYPED_TBL_VDEF(n, MP_T_CHAR)
#   define PRINT_TBL_ADEF(n)                PRINT_TYPED_TBL_ADEF(n, MP_T_CHAR)
#   define PRINT_TBL_VDEF_ITEM(n,v)         PRINT_TYPED_TBL_VDEF_ITEM(n,v,MP_T_CHAR)
#   define PRINT_TBL_ADEF_ITEM(n,v)         PRINT_TYPED_TBL_ADEF_ITEM(n,v,MP_T_CHAR)
#   define PRINT_TBL_COM_VDEF(c,n)          PRINT_TYPED_COM_VDEF(c,n,MP_T_CHAR)
#   define PRINT_TBL_COM_ADEF(c,n)          PRINT_TYPED_COM_ADEF(c,n,MP_T_CHAR)
#   define PRINT_TBL_COM_VDEF_ITEM(c,n,v)   PRINT_TYPED_TBL_COM_VDEF_ITEM(c,n,v,MP_T_CHAR)
#   define PRINT_TBL_COM_ADEF_ITEM(c,n,v)   PRINT_TYPED_TBL_COM_ADEF_ITEM(c,n,v,MP_T_CHAR)

#   define PRINT_R_TBL_ITEM(v)              PRINT_TYPED_TBL_ITEM(v,R_CHAR)
#   define PRINT_R_TBL_VDEF(n)              PRINT_TYPED_TBL_VDEF(n,R_CHAR)
#   define PRINT_R_TBL_ADEF(n)              PRINT_TYPED_TBL_ADEF(n,R_CHAR)
#   define PRINT_R_TBL_VDEF_ITEM(n,v)       PRINT_TYPED_TBL_VDEF_ITEM(n,v,R_CHAR)
#   define PRINT_R_TBL_ADEF_ITEM(n,v)       PRINT_TYPED_TBL_ADEF_ITEM(n,v,R_CHAR)
#   define PRINT_R_TBL_COM_VDEF(c,n)        PRINT_TYPED_COM_VDEF(c,n,R_CHAR)
#   define PRINT_R_TBL_COM_ADEF(c,n)        PRINT_TYPED_COM_ADEF(c,n,R_CHAR)
#   define PRINT_R_TBL_COM_VDEF_ITEM(c,n,v) PRINT_TYPED_TBL_COM_VDEF_ITEM(c,n,v,R_CHAR)
#   define PRINT_R_TBL_COM_ADEF_ITEM(c,n,v) PRINT_TYPED_TBL_COM_ADEF_ITEM(c,n,v,R_CHAR)

#   define PRINT_F_TBL_ITEM(v)              PRINT_TYPED_TBL_ITEM(v,F_CHAR)
#   define PRINT_F_TBL_VDEF(n)              PRINT_TYPED_TBL_VDEF(n,F_CHAR)
#   define PRINT_F_TBL_ADEF(n)              PRINT_TYPED_TBL_ADEF(n,F_CHAR)
#   define PRINT_F_TBL_VDEF_ITEM(n,v)       PRINT_TYPED_TBL_VDEF_ITEM(n,v,F_CHAR)
#   define PRINT_F_TBL_ADEF_ITEM(n,v)       PRINT_TYPED_TBL_ADEF_ITEM(n,v,F_CHAR)
#   define PRINT_F_TBL_COM_VDEF(c,n)        PRINT_TYPED_COM_VDEF(c,n,F_CHAR)
#   define PRINT_F_TBL_COM_ADEF(c,n)        PRINT_TYPED_COM_ADEF(c,n,F_CHAR)
#   define PRINT_F_TBL_COM_VDEF_ITEM(c,n,v) PRINT_TYPED_TBL_COM_VDEF_ITEM(c,n,v,F_CHAR)
#   define PRINT_F_TBL_COM_ADEF_ITEM(c,n,v) PRINT_TYPED_TBL_COM_ADEF_ITEM(c,n,v,F_CHAR)

#   define PRINT_B_TBL_ITEM(v)              PRINT_TYPED_TBL_ITEM(v,B_CHAR)
#   define PRINT_B_TBL_VDEF(n)              PRINT_TYPED_TBL_VDEF(n,B_CHAR)
#   define PRINT_B_TBL_ADEF(n)              PRINT_TYPED_TBL_ADEF(n,B_CHAR)
#   define PRINT_B_TBL_VDEF_ITEM(n,v)       PRINT_TYPED_TBL_VDEF_ITEM(n,v,B_CHAR)
#   define PRINT_B_TBL_ADEF_ITEM(n,v)       PRINT_TYPED_TBL_ADEF_ITEM(n,v,B_CHAR)
#   define PRINT_B_COM_VDEF(c,n)            PRINT_TYPED_COM_VDEF(c,n,B_CHAR)
#   define PRINT_B_COM_ADEF(c,n)            PRINT_TYPED_COM_ADEF(c,n,B_CHAR)
#   define PRINT_B_TBL_COM_VDEF_ITEM(c,n,v) PRINT_TYPED_TBL_COM_VDEF_ITEM(c,n,v,B_CHAR)
#   define PRINT_B_TBL_COM_ADEF_ITEM(c,n,v) PRINT_TYPED_TBL_COM_ADEF_ITEM(c,n,v,B_CHAR)

#   define PRINT_W_TBL_ITEM(v)              PRINT_TYPED_TBL_ITEM(v,W_CHAR)
#   define PRINT_W_TBL_VDEF(n)              PRINT_TYPED_TBL_VDEF(n,W_CHAR)
#   define PRINT_W_TBL_ADEF(n)              PRINT_TYPED_TBL_ADEF(n,W_CHAR)
#   define PRINT_W_TBL_VDEF_ITEM(n,v)       PRINT_TYPED_TBL_VDEF_ITEM(n,v,W_CHAR)
#   define PRINT_W_TBL_ADEF_ITEM(n,v)       PRINT_TYPED_TBL_ADEF_ITEM(n,v,W_CHAR)
#   define PRINT_W_COM_VDEF(c,n)            PRINT_TYPED_COM_VDEF(c,n,W_CHAR)
#   define PRINT_W_COM_ADEF(c,n)            PRINT_TYPED_COM_ADEF(c,n,W_CHAR)
#   define PRINT_W_TBL_COM_VDEF_ITEM(c,n,v) PRINT_TYPED_TBL_COM_VDEF_ITEM(c,n,v,W_CHAR)
#   define PRINT_W_TBL_COM_ADEF_ITEM(c,n,v) PRINT_TYPED_TBL_COM_ADEF_ITEM(c,n,v,W_CHAR)

#   define PRINT_U_TBL_ITEM(v)              PRINT_TYPED_TBL_ITEM(v,U_CHAR)
#   define PRINT_U_TBL_VDEF(n)              PRINT_TYPED_TBL_VDEF(n,U_CHAR)
#   define PRINT_U_TBL_ADEF(n)              PRINT_TYPED_TBL_ADEF(n,U_CHAR)
#   define PRINT_U_TBL_VDEF_ITEM(n,v)       PRINT_TYPED_TBL_VDEF_ITEM(n,v,U_CHAR)
#   define PRINT_U_TBL_ADEF_ITEM(n,v)       PRINT_TYPED_TBL_ADEF_ITEM(n,v,U_CHAR)
#   define PRINT_U_COM_VDEF(c,n)            PRINT_TYPED_COM_VDEF(c,n,U_CHAR)
#   define PRINT_U_COM_ADEF(c,n)            PRINT_TYPED_COM_ADEF(c,n,U_CHAR)
#   define PRINT_U_TBL_COM_VDEF_ITEM(c,n,v) PRINT_TYPED_TBL_COM_VDEF_ITEM(c,n,v,U_CHAR)
#   define PRINT_U_TBL_COM_ADEF_ITEM(c,n,v) PRINT_TYPED_TBL_COM_ADEF_ITEM(c,n,v,U_CHAR)

#   define I16_HEX_FORMAT	"0x%4.4.16i"
#   define I32_HEX_FORMAT	"0x%8.8.16i"
#   define I64_HEX_FORMAT	"0x%16.16.16i"

#   define WORD_HEX_FORMAT	PASTE_3(I, BITS_PER_WORD, _HEX_FORMAT)

#   define PRINT_ITYPE_DEF(name, value, itype, format) \
		printf("#define\t" name "\t(( " STR(itype) " ) " format " )\n", value)

#   define PRINT_WORD_DEF(n, v)		PRINT_ITYPE_DEF(n, v, WORD,   WORD_HEX_FORMAT)
#   define PRINT_I32_DEF( n, v)		PRINT_ITYPE_DEF(n, v, INT_32, I32_HEX_FORMAT)
#   define PRINT_I64_DEF( n, v)		PRINT_ITYPE_DEF(n, v, INT_64, I64_HEX_FORMAT)

#   define PRINT_U_WORD_DEF(n, v)	PRINT_ITYPE_DEF(n, v, U_WORD,   WORD_HEX_FORMAT)
#   define PRINT_U32_DEF(  n, v)	PRINT_ITYPE_DEF(n, v, U_INT_32, I32_HEX_FORMAT)
#   define PRINT_U64_DEF(  n, v)	PRINT_ITYPE_DEF(n, v, U_INT_64, I64_HEX_FORMAT)

#   define MP_RN	0  /* ieee round to nearest           */
#   define MP_RZ	1  /* ieee round to zero (i.e. chop)  */
#   define MP_RP	2  /* ieee round to positive infinity */
#   define MP_RM	3  /* ieee round to minus infinity    */

#   define PRINT_TYPED_ARRAY(array, first, last, scale, type) \
		{								\
		auto i, tmp;							\
		tmp = scale^first;						\
		for (i = first; i <= last; i++)					\
		    {								\
		    PRINT_1_TYPE_ENTRY(type, array[i]*tmp, MP_BIT_OFFSET);	\
		    tmp *= scale;						\
		    }								\
		}

#   define PRINT_R_ARRAY(a,f,l,s)	PRINT_TYPED_ARRAY(a,f,l,s,R_CHAR)
#   define PRINT_F_ARRAY(a,f,l,s)	PRINT_TYPED_ARRAY(a,f,l,s,F_CHAR)
#   define PRINT_B_ARRAY(a,f,l,s)	PRINT_TYPED_ARRAY(a,f,l,s,B_CHAR)


#if IEEE_FLOATING
#   define MPHOC_R_DENORM_FACTOR	2^(1 - R_PRECISION)
#   define MPHOC_F_DENORM_FACTOR	2^(1 - F_PRECISION)
#   define MPHOC_B_DENORM_FACTOR	2^(1 - B_PRECISION)
#else
#   define MPHOC_R_DENORM_FACTOR 1
#   define MPHOC_F_DENORM_FACTOR 1
#   define MPHOC_B_DENORM_FACTOR 1
#endif

#define MPHOC_R_POS_NORMAL_TINY	(2 ^ (R_MIN_BIN_EXP + R_NORM))
#define MPHOC_R_POS_TINY	(MPHOC_R_POS_NORMAL_TINY*MPHOC_R_DENORM_FACTOR)
#define MPHOC_R_POS_HUGE	(2^(R_MAX_BIN_EXP + R_NORM + 1)*(1 - 2^(-R_PRECISION)))
#define MPHOC_R_NEG_NORMAL_TINY	(-MPHOC_R_POS_NORMAL_TINY)
#define MPHOC_R_NEG_TINY	(-MPHOC_R_POS_TINY)
#define MPHOC_R_NEG_HUGE 	(-MPHOC_R_POS_HUGE)

#define MPHOC_B_POS_NORMAL_TINY	(2 ^ (B_MIN_BIN_EXP + B_NORM))
#define MPHOC_B_POS_TINY	(MPHOC_B_POS_NORMAL_TINY*MPHOC_B_DENORM_FACTOR)
#define MPHOC_B_POS_HUGE	(2^(B_MAX_BIN_EXP + B_NORM + 1)*(1 - 2^(-B_PRECISION)))
#define MPHOC_B_NEG_NORMAL_TINY	(-MPHOC_B_POS_NORMAL_TINY)
#define MPHOC_B_NEG_TINY	(-MPHOC_B_POS_TINY)
#define MPHOC_B_NEG_HUGE 	(-MPHOC_B_POS_HUGE)

#define _GENPOLY(coef, name, _offset, options, _degree)		\
	printf(STR(GENPOLY_EXECUTABLE one degree=%i cn=), _degree);	\
	printf(STR(STR(coef) define=));					\
	printf(STR(STR(name) offset=%i options), _offset);		\
	printf(" ; echo \"\"\n" )

#define GENPOLY(coef, name, _degree)				\
	printf(STR(GENPOLY_EXECUTABLE one degree=%i cn=), _degree);	\
	printf(STR(STR(coef) define=));					\
	printf(STR(STR(name)));						\
	printf(" ; echo \"\"\n" )


#endif /* defined(NEW_MPHOC_MACROS) */

#endif  /* MPHOC_MACROS_H */
