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

#ifndef F_FORMAT_H
#define F_FORMAT_H


/* This file contains definitions that are related to floating formats.

We assume that the floating point number represention is based on the
fact that every non-zero real number, x, can be uniquely represented as

    x = (-1)^s * f * 2^e    where  2^NORM <= f < 2^(NORM + 1)

We also assume that the bit representation of floating point numbers is
divided into three disjoint fields defined by the following mnemonics:

      NAME        WIDTH      START POSITION    CONTIGUOUS?
    --------      -----      --------------    -----------
    Sign bit        1         SIGN_BIT_POS         Yes
    Exponent    EXP_WIDTH       EXP_POS            Yes
    Mantissa  PRECISION - 1     LSB_POS        Not for VAX

Ignoring denormalized numbers, NANs and infinities, the values of these
fields are defined as follows:

    Sign bit:   = 0 if x >= 0 
                = 1 if x < 0

    Exponent:   = 0 if x = 0 
                = (e + EXP_BIAS) otherwise

    Mantissa:   = 0 if x = 0 
                = rnd(f) - 2^NORM otherwise
                (rnd(f) is f limited to PRECISION bits)

*/


#undef F_FORMAT
#undef VAX_FLOATING
#undef IEEE_FLOATING
#undef SINGLE_PRECISION
#undef DOUBLE_PRECISION
#undef QUAD_PRECISION

#if   (defined(f_floating) || defined(F_FLOATING) || defined(f_float) || defined(F_FLOAT))

#   define VAX_FLOATING 1
#   define F_FORMAT f_floating
#   define SINGLE_PRECISION 1
#   define R_PREC_CHAR S
#   define F_PREC_CHAR S
#   define B_PREC_CHAR D

#elif (defined(d_floating) || defined(D_FLOATING) || defined(d_float) || defined(D_FLOAT))

#   define VAX_FLOATING 1
#   define F_FORMAT d_floating
#   define DOUBLE_PRECISION 1
#   define R_PREC_CHAR S
#   define F_PREC_CHAR D
#   define B_PREC_CHAR D

#elif (defined(g_floating) || defined(G_FLOATING) || defined(g_float) || defined(G_FLOAT))

#   define VAX_FLOATING 1
#   define F_FORMAT g_floating
#   define DOUBLE_PRECISION 1
#   define R_PREC_CHAR S
#   define F_PREC_CHAR D
#   define B_PREC_CHAR D

#elif (defined(h_floating) || defined(H_FLOATING) || defined(h_float) || defined(H_FLOAT))

#   define VAX_FLOATING 1
#   define F_FORMAT h_floating
#   define QUAD_PRECISION 1
#   define R_PREC_CHAR D
#   define F_PREC_CHAR Q
#   define B_PREC_CHAR Q

#elif (defined(s_floating) || defined(S_FLOATING) || defined(s_float) || defined(S_FLOAT))

#   define IEEE_FLOATING 1
#   define F_FORMAT s_floating
#   define SINGLE_PRECISION 1
#   define R_PREC_CHAR S
#   define F_PREC_CHAR S
#   define B_PREC_CHAR D

#elif (defined(t_floating) || defined(T_FLOATING) || defined(t_float) || defined(T_FLOAT))

#   define IEEE_FLOATING 1
#   define F_FORMAT t_floating
#   define DOUBLE_PRECISION 1
#   define R_PREC_CHAR S
#   define F_PREC_CHAR D
#   define B_PREC_CHAR D

#elif (defined(x_floating) || defined(X_FLOATING) || defined(x_float) || defined(X_FLOAT))

#   define IEEE_FLOATING 1
#   define F_FORMAT x_floating
#   define QUAD_PRECISION 1
#   define R_PREC_CHAR D
#   define F_PREC_CHAR Q
#   define B_PREC_CHAR Q

#else

#   define NO_FLOATING 1

#endif

#undef  f_floating
#undef  d_floating
#undef  g_floating
#undef  h_floating
#undef  s_floating
#undef  t_floating
#undef  x_floating

#define f_floating 1
#define d_floating 2
#define g_floating 3
#define h_floating 4
#define s_floating 5
#define t_floating 6
#define x_floating 7

#define VAX_TYPES   ((1 << f_floating) + (1 << g_floating) + (1 << h_floating))
#define IEEE_TYPES  ((1 << s_floating) + (1 << t_floating) + (1 << x_floating))



#define S_TYPE float
#define D_TYPE double
#if !defined(Q_TYPE)
#   if BITS_PER_LONG_DOUBLE == 128 && defined(LONG_DOUBLE_128_TYPE)
#       define Q_TYPE LONG_DOUBLE_128_TYPE
#   else
#       define Q_TYPE long double
#   endif
#endif

#define BITS_PER_S_TYPE BITS_PER_FLOAT
#define BITS_PER_D_TYPE BITS_PER_DOUBLE
#define BITS_PER_Q_TYPE BITS_PER_LONG_DOUBLE

/*
 * round up to next quad word boundrary where appropriate - Intel 
 * to get the alignment bits 
 */
#define ALIGNED_BITS_PER_Q_TYPE  ((BITS_PER_Q_TYPE - 1 + 32)/32)*32

#define I8S_PER_S_TYPE (BITS_PER_S_TYPE / 8)
#define I8S_PER_D_TYPE (BITS_PER_D_TYPE / 8)
#define I8S_PER_Q_TYPE (ALIGNED_BITS_PER_Q_TYPE / 8)

#define I16S_PER_S_TYPE (BITS_PER_S_TYPE / 16)
#define I16S_PER_D_TYPE (BITS_PER_D_TYPE / 16)
#define I16S_PER_Q_TYPE (ALIGNED_BITS_PER_Q_TYPE / 16)

#define I32S_PER_S_TYPE (BITS_PER_S_TYPE / 32)
#define I32S_PER_D_TYPE (BITS_PER_D_TYPE / 32)
#define I32S_PER_Q_TYPE (ALIGNED_BITS_PER_Q_TYPE / 32)

#define I64S_PER_S_TYPE (BITS_PER_S_TYPE / 64)
#define I64S_PER_D_TYPE (BITS_PER_D_TYPE / 64)
#define I64S_PER_Q_TYPE (ALIGNED_BITS_PER_Q_TYPE / 64)

#define WORDS_PER_S_TYPE (BITS_PER_S_TYPE / BITS_PER_WORD)
#define WORDS_PER_D_TYPE (BITS_PER_D_TYPE / BITS_PER_WORD)
#define WORDS_PER_Q_TYPE (ALIGNED_BITS_PER_Q_TYPE / BITS_PER_WORD)



typedef struct { 
    S_TYPE r, i; 
} S_COMPLEX;

typedef struct { 
    D_TYPE r, i; 
} D_COMPLEX;

typedef struct { 
    Q_TYPE r, i; 
} Q_COMPLEX;

#define F_COMPLEX_RETURN return


typedef union {
    S_TYPE f;
#if (I8S_PER_S_TYPE > 0)
    INT_8     i8[ I8S_PER_S_TYPE ];
    U_INT_8   u8[ I8S_PER_S_TYPE ];
#endif
#if (I16S_PER_S_TYPE > 0)
    INT_16   i16[ I16S_PER_S_TYPE ];
    U_INT_16 u16[ I16S_PER_S_TYPE ];
#endif
#if (I32S_PER_S_TYPE > 0)
    INT_32   i32[ I32S_PER_S_TYPE ];
    U_INT_32 u32[ I32S_PER_S_TYPE ];
#endif
#if (I64S_PER_S_TYPE > 0) && defined(INT_64)
    INT_64   i64[ I64S_PER_S_TYPE ];
    U_INT_64 u64[ I64S_PER_S_TYPE ];
#endif
#if (WORDS_PER_S_TYPE > 0)
    WORD      iw[ WORDS_PER_S_TYPE ];
    U_WORD    uw[ WORDS_PER_S_TYPE ];
#endif
} S_UNION;


typedef union {
    D_TYPE f;
#if (I8S_PER_D_TYPE > 0)
    INT_8     i8[ I8S_PER_D_TYPE ];
    U_INT_8   u8[ I8S_PER_D_TYPE ];
#endif
#if (I16S_PER_D_TYPE > 0)
    INT_16   i16[ I16S_PER_D_TYPE ];
    U_INT_16 u16[ I16S_PER_D_TYPE ];
#endif
#if (I32S_PER_D_TYPE > 0)
    INT_32   i32[ I32S_PER_D_TYPE ];
    U_INT_32 u32[ I32S_PER_D_TYPE ];
#endif
#if (I64S_PER_D_TYPE > 0) && defined(INT_64)
    INT_64   i64[ I64S_PER_D_TYPE ];
    U_INT_64 u64[ I64S_PER_D_TYPE ];
#endif
#if (WORDS_PER_D_TYPE > 0)
    WORD      iw[ WORDS_PER_D_TYPE ];
    U_WORD    uw[ WORDS_PER_D_TYPE ];
#endif
} D_UNION;


typedef union {
    Q_TYPE f;
#if (I8S_PER_Q_TYPE > 0)
    INT_8     i8[ I8S_PER_Q_TYPE ];
    U_INT_8   u8[ I8S_PER_Q_TYPE ];
#endif
#if (I16S_PER_Q_TYPE > 0)
    INT_16   i16[ I16S_PER_Q_TYPE ];
    U_INT_16 u16[ I16S_PER_Q_TYPE ];
#endif
#if (I32S_PER_Q_TYPE > 0)
    INT_32   i32[ I32S_PER_Q_TYPE ];
    U_INT_32 u32[ I32S_PER_Q_TYPE ];
#endif
#if (I64S_PER_Q_TYPE > 0) && defined(INT_64)
    INT_64   i64[ I64S_PER_Q_TYPE ];
    U_INT_64 u64[ I64S_PER_Q_TYPE ];
#endif
#if (WORDS_PER_Q_TYPE > 0)
    WORD      iw[ WORDS_PER_Q_TYPE ];
    U_WORD    uw[ WORDS_PER_Q_TYPE ];
#endif
} Q_UNION;
 
 
#if (!defined(TABLE_WORD) || !defined(TABLE_WORDS_PER_Q_TYPE))
#   define BITS_PER_TABLE_WORD		32
#   define TABLE_WORD    		U_INT_32
#   define TABLE_WORDS_PER_Q_TYPE	I32S_PER_Q_TYPE 
#endif

typedef union {
#if (TABLE_WORDS_PER_Q_TYPE > 0)
    TABLE_WORD    it[ TABLE_WORDS_PER_Q_TYPE ];
#else
    TABLE_WORD    it[ 1 ];
#endif
    Q_TYPE f;
} TABLE_UNION;



#if (VAX_FLOATING) || (ENDIANESS == big_endian)

#   define UNION_IX(i_per_type, k)  (k)

#else

#   define UNION_IX(i_per_type, k)  ( i_per_type - k - 1 )

#endif


#define S_SIGNED_HI_16    i16[ UNION_IX(I16S_PER_S_TYPE,0) ]
#define S_SIGNED_HI_32    i32[ UNION_IX(I32S_PER_S_TYPE,0) ]
#define S_SIGNED_HI_64    i64[ UNION_IX(I64S_PER_S_TYPE,0) ]
#define S_SIGNED_LO1_16   i16[ UNION_IX(I16S_PER_S_TYPE,1) ]
#define S_SIGNED_LO1_32   i32[ UNION_IX(I32S_PER_S_TYPE,1) ]
#define S_SIGNED_LO1_64   i64[ UNION_IX(I64S_PER_S_TYPE,1) ]
#define S_SIGNED_LO2_16   i16[ UNION_IX(I16S_PER_S_TYPE,2) ]
#define S_SIGNED_LO2_32   i32[ UNION_IX(I32S_PER_S_TYPE,2) ]
#define S_SIGNED_LO2_64   i64[ UNION_IX(I64S_PER_S_TYPE,2) ]
#define S_SIGNED_LO3_16   i16[ UNION_IX(I16S_PER_S_TYPE,3) ]
#define S_SIGNED_LO3_32   i32[ UNION_IX(I32S_PER_S_TYPE,3) ]
#define S_SIGNED_LO3_64   i64[ UNION_IX(I64S_PER_S_TYPE,3) ]
#define S_SIGNED_LO_16    S_SIGNED_LO1_16
#define S_SIGNED_LO_32    S_SIGNED_LO1_32
#define S_SIGNED_LO_64    S_SIGNED_LO1_64
#define S_UNSIGNED_HI_16  u16[ UNION_IX(I16S_PER_S_TYPE,0) ]
#define S_UNSIGNED_HI_32  u32[ UNION_IX(I32S_PER_S_TYPE,0) ]
#define S_UNSIGNED_HI_64  u64[ UNION_IX(I64S_PER_S_TYPE,0) ]
#define S_UNSIGNED_LO1_16 u16[ UNION_IX(I16S_PER_S_TYPE,1) ]
#define S_UNSIGNED_LO1_32 u32[ UNION_IX(I32S_PER_S_TYPE,1) ]
#define S_UNSIGNED_LO1_64 u64[ UNION_IX(I64S_PER_S_TYPE,1) ]
#define S_UNSIGNED_LO2_16 u16[ UNION_IX(I16S_PER_S_TYPE,2) ]
#define S_UNSIGNED_LO2_32 u32[ UNION_IX(I32S_PER_S_TYPE,2) ]
#define S_UNSIGNED_LO2_64 u64[ UNION_IX(I64S_PER_S_TYPE,2) ]
#define S_UNSIGNED_LO3_16 u16[ UNION_IX(I16S_PER_S_TYPE,3) ]
#define S_UNSIGNED_LO3_32 u32[ UNION_IX(I32S_PER_S_TYPE,3) ]
#define S_UNSIGNED_LO3_64 u64[ UNION_IX(I64S_PER_S_TYPE,3) ]
#define S_UNSIGNED_LO_16  S_UNSIGNED_LO1_16
#define S_UNSIGNED_LO_32  S_UNSIGNED_LO1_32
#define S_UNSIGNED_LO_64  S_UNSIGNED_LO1_64

#define D_SIGNED_HI_16    i16[ UNION_IX(I16S_PER_D_TYPE,0) ]
#define D_SIGNED_HI_32    i32[ UNION_IX(I32S_PER_D_TYPE,0) ]
#define D_SIGNED_HI_64    i64[ UNION_IX(I64S_PER_D_TYPE,0) ]
#define D_SIGNED_LO1_16   i16[ UNION_IX(I16S_PER_D_TYPE,1) ]
#define D_SIGNED_LO1_32   i32[ UNION_IX(I32S_PER_D_TYPE,1) ]
#define D_SIGNED_LO1_64   i64[ UNION_IX(I64S_PER_D_TYPE,1) ]
#define D_SIGNED_LO2_16   i16[ UNION_IX(I16S_PER_D_TYPE,2) ]
#define D_SIGNED_LO2_32   i32[ UNION_IX(I32S_PER_D_TYPE,2) ]
#define D_SIGNED_LO2_64   i64[ UNION_IX(I64S_PER_D_TYPE,2) ]
#define D_SIGNED_LO3_16   i16[ UNION_IX(I16S_PER_D_TYPE,3) ]
#define D_SIGNED_LO3_32   i32[ UNION_IX(I32S_PER_D_TYPE,3) ]
#define D_SIGNED_LO3_64   i64[ UNION_IX(I64S_PER_D_TYPE,3) ]
#define D_SIGNED_LO_16    D_SIGNED_LO1_16
#define D_SIGNED_LO_32    D_SIGNED_LO1_32
#define D_SIGNED_LO_64    D_SIGNED_LO1_64
#define D_UNSIGNED_HI_16  u16[ UNION_IX(I16S_PER_D_TYPE,0) ]
#define D_UNSIGNED_HI_32  u32[ UNION_IX(I32S_PER_D_TYPE,0) ]
#define D_UNSIGNED_HI_64  u64[ UNION_IX(I64S_PER_D_TYPE,0) ]
#define D_UNSIGNED_LO1_16 u16[ UNION_IX(I16S_PER_D_TYPE,1) ]
#define D_UNSIGNED_LO1_32 u32[ UNION_IX(I32S_PER_D_TYPE,1) ]
#define D_UNSIGNED_LO1_64 u64[ UNION_IX(I64S_PER_D_TYPE,1) ]
#define D_UNSIGNED_LO2_16 u16[ UNION_IX(I16S_PER_D_TYPE,2) ]
#define D_UNSIGNED_LO2_32 u32[ UNION_IX(I32S_PER_D_TYPE,2) ]
#define D_UNSIGNED_LO2_64 u64[ UNION_IX(I64S_PER_D_TYPE,2) ]
#define D_UNSIGNED_LO3_16 u16[ UNION_IX(I16S_PER_D_TYPE,3) ]
#define D_UNSIGNED_LO3_32 u32[ UNION_IX(I32S_PER_D_TYPE,3) ]
#define D_UNSIGNED_LO3_64 u64[ UNION_IX(I64S_PER_D_TYPE,3) ]
#define D_UNSIGNED_LO_16  D_UNSIGNED_LO1_16
#define D_UNSIGNED_LO_32  D_UNSIGNED_LO1_32
#define D_UNSIGNED_LO_64  D_UNSIGNED_LO1_64

#define Q_SIGNED_HI_16    i16[ UNION_IX(I16S_PER_Q_TYPE,0) ]
#define Q_SIGNED_HI_32    i32[ UNION_IX(I32S_PER_Q_TYPE,0) ]
#define Q_SIGNED_HI_64    i64[ UNION_IX(I64S_PER_Q_TYPE,0) ]
#define Q_SIGNED_LO1_16   i16[ UNION_IX(I16S_PER_Q_TYPE,1) ]
#define Q_SIGNED_LO1_32   i32[ UNION_IX(I32S_PER_Q_TYPE,1) ]
#define Q_SIGNED_LO1_64   i64[ UNION_IX(I64S_PER_Q_TYPE,1) ]
#define Q_SIGNED_LO2_16   i16[ UNION_IX(I16S_PER_Q_TYPE,2) ]
#define Q_SIGNED_LO2_32   i32[ UNION_IX(I32S_PER_Q_TYPE,2) ]
#define Q_SIGNED_LO2_64   i64[ UNION_IX(I64S_PER_Q_TYPE,2) ]
#define Q_SIGNED_LO3_16   i16[ UNION_IX(I16S_PER_Q_TYPE,3) ]
#define Q_SIGNED_LO3_32   i32[ UNION_IX(I32S_PER_Q_TYPE,3) ]
#define Q_SIGNED_LO3_64   i64[ UNION_IX(I64S_PER_Q_TYPE,3) ]
#define Q_SIGNED_LO_16    Q_SIGNED_LO1_16
#define Q_SIGNED_LO_32    Q_SIGNED_LO1_32
#define Q_SIGNED_LO_64    Q_SIGNED_LO1_64
#define Q_UNSIGNED_HI_16  u16[ UNION_IX(I16S_PER_Q_TYPE,0) ]
#define Q_UNSIGNED_HI_32  u32[ UNION_IX(I32S_PER_Q_TYPE,0) ]
#define Q_UNSIGNED_HI_64  u64[ UNION_IX(I64S_PER_Q_TYPE,0) ]
#define Q_UNSIGNED_LO1_16 u16[ UNION_IX(I16S_PER_Q_TYPE,1) ]
#define Q_UNSIGNED_LO1_32 u32[ UNION_IX(I32S_PER_Q_TYPE,1) ]
#define Q_UNSIGNED_LO1_64 u64[ UNION_IX(I64S_PER_Q_TYPE,1) ]
#define Q_UNSIGNED_LO2_16 u16[ UNION_IX(I16S_PER_Q_TYPE,2) ]
#define Q_UNSIGNED_LO2_32 u32[ UNION_IX(I32S_PER_Q_TYPE,2) ]
#define Q_UNSIGNED_LO2_64 u64[ UNION_IX(I64S_PER_Q_TYPE,2) ]
#define Q_UNSIGNED_LO3_16 u16[ UNION_IX(I16S_PER_Q_TYPE,3) ]
#define Q_UNSIGNED_LO3_32 u32[ UNION_IX(I32S_PER_Q_TYPE,3) ]
#define Q_UNSIGNED_LO3_64 u64[ UNION_IX(I64S_PER_Q_TYPE,3) ]
#define Q_UNSIGNED_LO_16  Q_UNSIGNED_LO1_16
#define Q_UNSIGNED_LO_32  Q_UNSIGNED_LO1_32
#define Q_UNSIGNED_LO_64  Q_UNSIGNED_LO1_64



#if (WORDS_PER_S_TYPE > 0)
#   define S_SIGNED_HI_WORD   iw[ UNION_IX(WORDS_PER_S_TYPE,0) ]
#   define S_UNSIGNED_HI_WORD uw[ UNION_IX(WORDS_PER_S_TYPE,0) ]
#else
#   define S_SIGNED_HI_WORD   PASTE_2(i, BITS_PER_S_TYPE)[0]
#   define S_UNSIGNED_HI_WORD PASTE_2(u, BITS_PER_S_TYPE)[0]
#endif

#if (WORDS_PER_S_TYPE > 1)
#   define S_SIGNED_LO1_WORD   iw[ UNION_IX(WORDS_PER_S_TYPE,1) ]
#   define S_UNSIGNED_LO1_WORD uw[ UNION_IX(WORDS_PER_S_TYPE,1) ]
#else
#   define S_SIGNED_LO1_WORD   S_SIGNED_HI_WORD
#   define S_UNSIGNED_LO1_WORD S_UNSIGNED_HI_WORD
#endif

#if (WORDS_PER_S_TYPE > 2)
#   define S_SIGNED_LO2_WORD   iw[ UNION_IX(WORDS_PER_S_TYPE,2) ]
#   define S_UNSIGNED_LO2_WORD uw[ UNION_IX(WORDS_PER_S_TYPE,2) ]
#else
#   define S_SIGNED_LO2_WORD   S_SIGNED_LO1_WORD
#   define S_UNSIGNED_LO2_WORD S_UNSIGNED_LO1_WORD
#endif

#if (WORDS_PER_S_TYPE > 3)
#   define S_SIGNED_LO3_WORD   iw[ UNION_IX(WORDS_PER_S_TYPE,3) ]
#   define S_UNSIGNED_LO3_WORD uw[ UNION_IX(WORDS_PER_S_TYPE,3) ]
#else
#   define S_SIGNED_LO3_WORD   S_SIGNED_LO2_WORD
#   define S_UNSIGNED_LO3_WORD S_UNSIGNED_LO2_WORD
#endif

#define S_HI_WORD  S_UNSIGNED_HI_WORD
#define S_LO1_WORD S_UNSIGNED_LO1_WORD
#define S_LO2_WORD S_UNSIGNED_LO2_WORD
#define S_LO3_WORD S_UNSIGNED_LO3_WORD
#define S_LO_WORD  S_LO1_WORD



#if (WORDS_PER_D_TYPE > 0)
#   define D_SIGNED_HI_WORD   iw[ UNION_IX(WORDS_PER_D_TYPE,0) ]
#   define D_UNSIGNED_HI_WORD uw[ UNION_IX(WORDS_PER_D_TYPE,0) ]
#else
#   define D_SIGNED_HI_WORD   PASTE_2(i, BITS_PER_D_TYPE)[0]
#   define D_UNSIGNED_HI_WORD PASTE_2(u, BITS_PER_D_TYPE)[0]
#endif

#if (WORDS_PER_D_TYPE > 1)
#   define D_SIGNED_LO1_WORD   iw[ UNION_IX(WORDS_PER_D_TYPE,1) ]
#   define D_UNSIGNED_LO1_WORD uw[ UNION_IX(WORDS_PER_D_TYPE,1) ]
#else
#   define D_SIGNED_LO1_WORD   D_SIGNED_HI_WORD
#   define D_UNSIGNED_LO1_WORD D_UNSIGNED_HI_WORD
#endif

#if (WORDS_PER_D_TYPE > 2)
#   define D_SIGNED_LO2_WORD   iw[ UNION_IX(WORDS_PER_D_TYPE,2) ]
#   define D_UNSIGNED_LO2_WORD uw[ UNION_IX(WORDS_PER_D_TYPE,2) ]
#else
#   define D_SIGNED_LO2_WORD   D_SIGNED_LO1_WORD
#   define D_UNSIGNED_LO2_WORD D_UNSIGNED_LO1_WORD
#endif

#if (WORDS_PER_D_TYPE > 3)
#   define D_SIGNED_LO3_WORD   iw[ UNION_IX(WORDS_PER_D_TYPE,3) ]
#   define D_UNSIGNED_LO3_WORD uw[ UNION_IX(WORDS_PER_D_TYPE,3) ]
#else
#   define D_SIGNED_LO3_WORD   D_SIGNED_LO2_WORD
#   define D_UNSIGNED_LO3_WORD D_UNSIGNED_LO2_WORD
#endif

#define D_HI_WORD  D_UNSIGNED_HI_WORD
#define D_LO1_WORD D_UNSIGNED_LO1_WORD
#define D_LO2_WORD D_UNSIGNED_LO2_WORD
#define D_LO3_WORD D_UNSIGNED_LO3_WORD
#define D_LO_WORD  D_LO1_WORD



#if (WORDS_PER_Q_TYPE > 0)
#   define Q_SIGNED_HI_WORD   iw[ UNION_IX(WORDS_PER_Q_TYPE,0) ]
#   define Q_UNSIGNED_HI_WORD uw[ UNION_IX(WORDS_PER_Q_TYPE,0) ]
#else
#   define Q_SIGNED_HI_WORD   PASTE_2(i, BITS_PER_Q_TYPE)[0]
#   define Q_UNSIGNED_HI_WORD PASTE_2(u, BITS_PER_Q_TYPE)[0]
#endif

#if (WORDS_PER_Q_TYPE > 1)
#   define Q_SIGNED_LO1_WORD   iw[ UNION_IX(WORDS_PER_Q_TYPE,1) ]
#   define Q_UNSIGNED_LO1_WORD uw[ UNION_IX(WORDS_PER_Q_TYPE,1) ]
#else
#   define Q_SIGNED_LO1_WORD   Q_SIGNED_HI_WORD
#   define Q_UNSIGNED_LO1_WORD Q_UNSIGNED_HI_WORD
#endif

#if (WORDS_PER_Q_TYPE > 2)
#   define Q_SIGNED_LO2_WORD   iw[ UNION_IX(WORDS_PER_Q_TYPE,2) ]
#   define Q_UNSIGNED_LO2_WORD uw[ UNION_IX(WORDS_PER_Q_TYPE,2) ]
#else
#   define Q_SIGNED_LO2_WORD   Q_SIGNED_LO1_WORD
#   define Q_UNSIGNED_LO2_WORD Q_UNSIGNED_LO1_WORD
#endif

#if (WORDS_PER_Q_TYPE > 3)
#   define Q_SIGNED_LO3_WORD   iw[ UNION_IX(WORDS_PER_Q_TYPE,3) ]
#   define Q_UNSIGNED_LO3_WORD uw[ UNION_IX(WORDS_PER_Q_TYPE,3) ]
#else
#   define Q_SIGNED_LO3_WORD   Q_SIGNED_LO2_WORD
#   define Q_UNSIGNED_LO3_WORD Q_UNSIGNED_LO2_WORD
#endif

#define Q_HI_WORD  Q_UNSIGNED_HI_WORD
#define Q_LO1_WORD Q_UNSIGNED_LO1_WORD
#define Q_LO2_WORD Q_UNSIGNED_LO2_WORD
#define Q_LO3_WORD Q_UNSIGNED_LO3_WORD
#define Q_LO_WORD  Q_LO1_WORD




/* The t_xyz_POS constants below are not yet sufficiently general.
Currently, they assume 32 or 64 bits words.  They should allow 16 bit
words, et cetera.  */


#if (VAX_FLOATING)

#   define S_FORMAT f_floating
#   define S_CHAR f
#   define S_NORM (-1)
#   define S_PRECISION 24
#   define S_LSB_POS 16
#   define S_MSB_POS 6
#   define S_EXP_POS 7
#   define S_EXP_BIAS 128
#   define S_EXP_WIDTH 8
#   define S_MIN_BIN_EXP -127
#   define S_MAX_BIN_EXP 127
#   define S_MIN_DEC_EXP -38
#   define S_MAX_DEC_EXP 39
#   define S_SIGN_BIT_POS 15
#   undef  S_25TH_BIT_POS

#   if (F_FORMAT == d_floating)

#       define D_FORMAT d_floating
#       define D_CHAR d
#       define D_NORM (-1)
#       define D_PRECISION 56
#       define D_LSB_POS (BITS_PER_WORD - 16)
#       define D_MSB_POS 6
#       define D_EXP_POS 7
#       define D_EXP_BIAS 128
#       define D_EXP_WIDTH 8
#       define D_MIN_BIN_EXP -127
#       define D_MAX_BIN_EXP 127
#       define D_MIN_DEC_EXP -38
#       define D_MAX_DEC_EXP 39
#       define D_SIGN_BIT_POS 15
#       define D_25TH_BIT_POS (BITS_PER_WORD - 17)

#   else

#       define D_FORMAT g_floating
#       define D_CHAR g
#       define D_NORM (-1)
#       define D_PRECISION 53
#       define D_LSB_POS (BITS_PER_WORD - 16)
#       define D_MSB_POS 3
#       define D_EXP_POS 4
#       define D_EXP_BIAS 1024
#       define D_EXP_WIDTH 11
#       define D_MIN_BIN_EXP -1023
#       define D_MAX_BIN_EXP 1023
#       define D_MIN_DEC_EXP -308
#       define D_MAX_DEC_EXP 308
#       define D_SIGN_BIT_POS 15
#       define D_25TH_BIT_POS (BITS_PER_WORD - 20)

#   endif

#   define Q_FORMAT h_floating
#   define Q_CHAR h
#   define Q_NORM (-1)
#   define Q_PRECISION 113
#   define Q_LSB_POS (BITS_PER_WORD - 16)
#   define Q_MSB_POS 31
#   define Q_EXP_POS 0
#   define Q_EXP_BIAS 16384
#   define Q_EXP_WIDTH 15
#   define Q_MIN_BIN_EXP -16383
#   define Q_MAX_BIN_EXP 16383
#   define Q_MIN_DEC_EXP -4932
#   define Q_MAX_DEC_EXP 4932
#   define Q_SIGN_BIT_POS 15


#elif IEEE_FLOATING

#   define S_FORMAT s_floating
#   define S_CHAR s
#   define S_NORM 0
#   define S_PRECISION 24
#   define S_LSB_POS 0
#   define S_MSB_POS (BITS_PER_S_TYPE - S_EXP_WIDTH - 2)
#   define S_EXP_POS (BITS_PER_S_TYPE - S_EXP_WIDTH - 1)
#   define S_EXP_BIAS 127
#   define S_EXP_WIDTH 8
#   define S_MIN_BIN_EXP -126
#   define S_MAX_BIN_EXP 127
#   define S_MIN_DEC_EXP -45
#   define S_MAX_DEC_EXP 39
#   define S_SIGN_BIT_POS (BITS_PER_S_TYPE - 1)
#   undef  S_25TH_BIT_POS

#   define D_FORMAT t_floating
#   define D_CHAR t
#   define D_NORM 0
#   define D_PRECISION 53
#   define D_LSB_POS 0
#   define D_MSB_POS (BITS_PER_WORD - D_EXP_WIDTH - 2)
#   define D_EXP_POS (BITS_PER_WORD - D_EXP_WIDTH - 1)
#   define D_EXP_BIAS 1023
#   define D_EXP_WIDTH 11
#   define D_MIN_BIN_EXP -1022
#   define D_MAX_BIN_EXP 1023
#   define D_MIN_DEC_EXP -323
#   define D_MAX_DEC_EXP 309
#   define D_SIGN_BIT_POS (BITS_PER_WORD - 1)
#   define D_25TH_BIT_POS 28

#   define Q_FORMAT x_floating
#   define Q_CHAR x
#   define Q_NORM 0
#   define Q_PRECISION 113
#   define Q_LSB_POS 0
#   define Q_MSB_POS (BITS_PER_WORD - Q_EXP_WIDTH - 2)
#   define Q_EXP_POS (BITS_PER_WORD - Q_EXP_WIDTH - 1)
#   define Q_EXP_BIAS 16383
#   define Q_EXP_WIDTH 15
#   define Q_MIN_BIN_EXP -16382
#   define Q_MAX_BIN_EXP 16383
#   define Q_MIN_DEC_EXP -4965
#   define Q_MAX_DEC_EXP 4933
#   define Q_SIGN_BIT_POS (BITS_PER_WORD - 1)

#endif

/* Don't define these symbols until the globals table is instantiated */

#if defined(GLOBAL_TABLE_VALUES)
#   if (ARCHITECTURE == mips)
#       define  X_NAN_HI	0x7fff7fff
#       define  T_NAN_HI	0x7ff7ffff
#       define  S_NAN_HI	0x7fbfffff
#       define  NAN_LO		0xffffffff
#   elif (ARCHITECTURE == hp_pa)
#       define  X_NAN_HI	0x7fff4000
#       define  T_NAN_HI	0x7ff40000
#       define  S_NAN_HI	0x7fa00000
#       define  NAN_LO		0x00000000
#   else
#       define  X_NAN_HI	0xffff8000
#       define  T_NAN_HI	0xfff80000
#       define  S_NAN_HI	0xffc00000
#       define  NAN_LO		0x00000000
#   endif
#endif


#if !defined(BITS_PER_WORD)
#   error "BITS_PER_WORD not defined"
#endif

#if (!NO_FLOATING)

#define S_HIDDEN_BIT_MASK MAKE_MASK(1, S_EXP_POS)
#define S_SIGN_BIT_MASK   MAKE_MASK(1, S_SIGN_BIT_POS)
#define S_EXP_MASK        MAKE_MASK(S_EXP_WIDTH, S_EXP_POS)
#define S_SIGN_EXP_MASK   (S_SIGN_BIT_MASK | S_EXP_MASK)
#define S_MANTISSA_MASK   ((~S_SIGN_EXP_MASK) & __F_TYPE_BIT_MASK)
#define S_MAX_BIASED_EXP  (S_EXP_BIAS + S_MAX_BIN_EXP)

#define D_HIDDEN_BIT_MASK MAKE_MASK(1, D_EXP_POS)
#define D_SIGN_BIT_MASK   MAKE_MASK(1, D_SIGN_BIT_POS)
#define D_EXP_MASK        MAKE_MASK(D_EXP_WIDTH, D_EXP_POS)
#define D_SIGN_EXP_MASK   (D_SIGN_BIT_MASK | D_EXP_MASK)
#define D_MANTISSA_MASK   (~D_SIGN_EXP_MASK & __F_TYPE_BIT_MASK)
#define D_MAX_BIASED_EXP  (D_EXP_BIAS + D_MAX_BIN_EXP)

#define Q_HIDDEN_BIT_MASK MAKE_MASK(1, Q_EXP_POS)
#define Q_SIGN_BIT_MASK   MAKE_MASK(1, Q_SIGN_BIT_POS)
#define Q_EXP_MASK        MAKE_MASK(Q_EXP_WIDTH, Q_EXP_POS)
#define Q_SIGN_EXP_MASK   (Q_SIGN_BIT_MASK | Q_EXP_MASK)
#define Q_MANTISSA_MASK   (~Q_SIGN_EXP_MASK & __F_TYPE_BIT_MASK)
#define Q_MAX_BIASED_EXP  (Q_EXP_BIAS + Q_MAX_BIN_EXP)



/* These macros are good for creating floating constants that are
powers of two up to 2^M where M = (4 * BITS_PER_WORD) - 5 */

#define	__MAX_F_POW_2_EXP	(4*BITS_PER_WORD - 5)

#define S1_POW_2(n) ((S_TYPE)((U_WORD)1 << (n)))
#define S2_POW_2(n) S1_POW_2(((n)+1)/2)*S1_POW_2((n)/2)
#define S4_POW_2(n) S2_POW_2(((n)+1)/2)*S2_POW_2((n)/2)
#define S_POW_2(n)  S4_POW_2(n)

#define D1_POW_2(n) ((D_TYPE)((U_WORD)1 << (n)))
#define D2_POW_2(n) D1_POW_2(((n)+1)/2)*D1_POW_2((n)/2)
#define D4_POW_2(n) D2_POW_2(((n)+1)/2)*D2_POW_2((n)/2)
#define D_POW_2(n)  D4_POW_2(n)

#define Q1_POW_2(n) ((Q_TYPE)((U_WORD)1 << (n)))
#define Q2_POW_2(n) Q1_POW_2(((n)+1)/2)*Q1_POW_2((n)/2)
#define Q4_POW_2(n) Q2_POW_2(((n)+1)/2)*Q2_POW_2((n)/2)
#define Q_POW_2(n)  Q4_POW_2(n)

#define F1_POW_2(n) ((F_TYPE)((U_WORD)1 << (n)))
#define F2_POW_2(n) F1_POW_2(((n)+1)/2)*F1_POW_2((n)/2)
#define F4_POW_2(n) F2_POW_2(((n)+1)/2)*F2_POW_2((n)/2)
#define F_POW_2(n)  F4_POW_2(n)

#define B1_POW_2(n) ((B_TYPE)((U_WORD)1 << (n)))
#define B2_POW_2(n) B1_POW_2(((n)+1)/2)*B1_POW_2((n)/2)
#define B4_POW_2(n) B2_POW_2(((n)+1)/2)*B2_POW_2((n)/2)
#define B_POW_2(n)  B4_POW_2(n)



#if (SINGLE_PRECISION)

#   define F_CHAR S_CHAR
#   define F_TYPE S_TYPE
#   define BITS_PER_F_TYPE BITS_PER_S_TYPE
#   define F_NORM S_NORM
#   define F_PRECISION S_PRECISION
#   define F_LSB_POS S_LSB_POS
#   define F_MSB_POS S_MSB_POS
#   define F_EXP_POS S_EXP_POS
#   define F_EXP_BIAS S_EXP_BIAS
#   define F_EXP_WIDTH S_EXP_WIDTH
#   define F_MIN_BIN_EXP S_MIN_BIN_EXP
#   define F_MAX_BIN_EXP S_MAX_BIN_EXP
#   define F_MIN_DEC_EXP S_MIN_DEC_EXP
#   define F_MAX_DEC_EXP S_MAX_DEC_EXP
#   define F_SIGN_BIT_POS S_SIGN_BIT_POS
#   undef  F_25TH_BIT_POS
#   define F_HI_WORD  S_HI_WORD
#   define F_LO1_WORD S_LO1_WORD
#   define F_LO2_WORD S_LO2_WORD
#   define F_LO3_WORD S_LO3_WORD
#   define F_LO_WORD  S_LO_WORD
#   define F_SIGNED_HI_WORD  S_SIGNED_HI_WORD
#   define F_SIGNED_HI_16    S_SIGNED_HI_16
#   define F_SIGNED_HI_32    S_SIGNED_HI_32
#   define F_SIGNED_HI_64    S_SIGNED_HI_64
#   define F_SIGNED_LO1_16   S_SIGNED_LO1_16
#   define F_SIGNED_LO1_32   S_SIGNED_LO1_32
#   define F_SIGNED_LO1_64   S_SIGNED_LO1_64
#   define F_SIGNED_LO2_16   S_SIGNED_LO2_16
#   define F_SIGNED_LO2_32   S_SIGNED_LO2_32
#   define F_SIGNED_LO2_64   S_SIGNED_LO2_64
#   define F_SIGNED_LO3_16   S_SIGNED_LO3_16
#   define F_SIGNED_LO3_32   S_SIGNED_LO3_32
#   define F_SIGNED_LO3_64   S_SIGNED_LO3_64
#   define F_SIGNED_LO_16    S_SIGNED_LO_16
#   define F_SIGNED_LO_32    S_SIGNED_LO_32
#   define F_SIGNED_LO_64    S_SIGNED_LO_64
#   define F_UNSIGNED_HI_16  S_UNSIGNED_HI_16
#   define F_UNSIGNED_HI_32  S_UNSIGNED_HI_32
#   define F_UNSIGNED_HI_64  S_UNSIGNED_HI_64
#   define F_UNSIGNED_LO1_16 S_UNSIGNED_LO1_16
#   define F_UNSIGNED_LO1_32 S_UNSIGNED_LO1_32
#   define F_UNSIGNED_LO1_64 S_UNSIGNED_LO1_64
#   define F_UNSIGNED_LO2_16 S_UNSIGNED_LO2_16
#   define F_UNSIGNED_LO2_32 S_UNSIGNED_LO2_32
#   define F_UNSIGNED_LO2_64 S_UNSIGNED_LO2_64
#   define F_UNSIGNED_LO3_16 S_UNSIGNED_LO3_16
#   define F_UNSIGNED_LO3_32 S_UNSIGNED_LO3_32
#   define F_UNSIGNED_LO3_64 S_UNSIGNED_LO3_64
#   define F_UNSIGNED_LO_16  S_UNSIGNED_LO_16
#   define F_UNSIGNED_LO_32  S_UNSIGNED_LO_32
#   define F_UNSIGNED_LO_64  S_UNSIGNED_LO_64
#   define F_UNION S_UNION
#   define F_COMPLEX S_COMPLEX
#   define F_HIDDEN_BIT_MASK S_HIDDEN_BIT_MASK
#   define F_SIGN_BIT_MASK S_SIGN_BIT_MASK
#   define F_EXP_MASK S_EXP_MASK
#   define F_SIGN_EXP_MASK S_SIGN_EXP_MASK
#   define F_MANTISSA_MASK S_MANTISSA_MASK
#   define F_MAX_BIASED_EXP S_MAX_BIASED_EXP

#   define B_FORMAT D_FORMAT
#   define B_CHAR D_CHAR
#   define B_TYPE D_TYPE
#   define BITS_PER_B_TYPE BITS_PER_D_TYPE
#   define B_NORM D_NORM
#   define B_PRECISION D_PRECISION
#   define B_LSB_POS D_LSB_POS
#   define B_MSB_POS D_MSB_POS
#   define B_EXP_POS D_EXP_POS
#   define B_EXP_BIAS D_EXP_BIAS
#   define B_EXP_WIDTH D_EXP_WIDTH
#   define B_MIN_BIN_EXP D_MIN_BIN_EXP
#   define B_MAX_BIN_EXP D_MAX_BIN_EXP
#   define B_MIN_DEC_EXP D_MIN_DEC_EXP
#   define B_MAX_DEC_EXP D_MAX_DEC_EXP
#   define B_SIGN_BIT_POS D_SIGN_BIT_POS
#   define B_25TH_BIT_POS D_25TH_BIT_POS
#   define B_HI_WORD  D_HI_WORD
#   define B_LO1_WORD D_LO1_WORD
#   define B_LO2_WORD D_LO2_WORD
#   define B_LO3_WORD D_LO3_WORD
#   define B_LO_WORD  D_LO_WORD
#   define B_SIGNED_HI_WORD  D_SIGNED_HI_WORD
#   define B_SIGNED_HI_16    D_SIGNED_HI_16
#   define B_SIGNED_HI_32    D_SIGNED_HI_32
#   define B_SIGNED_HI_64    D_SIGNED_HI_64
#   define B_SIGNED_LO1_16   D_SIGNED_LO1_16
#   define B_SIGNED_LO1_32   D_SIGNED_LO1_32
#   define B_SIGNED_LO1_64   D_SIGNED_LO1_64
#   define B_SIGNED_LO2_16   D_SIGNED_LO2_16
#   define B_SIGNED_LO2_32   D_SIGNED_LO2_32
#   define B_SIGNED_LO2_64   D_SIGNED_LO2_64
#   define B_SIGNED_LO3_16   D_SIGNED_LO3_16
#   define B_SIGNED_LO3_32   D_SIGNED_LO3_32
#   define B_SIGNED_LO3_64   D_SIGNED_LO3_64
#   define B_SIGNED_LO_16    D_SIGNED_LO_16
#   define B_SIGNED_LO_32    D_SIGNED_LO_32
#   define B_SIGNED_LO_64    D_SIGNED_LO_64
#   define B_UNSIGNED_HI_16  D_UNSIGNED_HI_16
#   define B_UNSIGNED_HI_32  D_UNSIGNED_HI_32
#   define B_UNSIGNED_HI_64  D_UNSIGNED_HI_64
#   define B_UNSIGNED_LO1_16 D_UNSIGNED_LO1_16
#   define B_UNSIGNED_LO1_32 D_UNSIGNED_LO1_32
#   define B_UNSIGNED_LO1_64 D_UNSIGNED_LO1_64
#   define B_UNSIGNED_LO2_16 D_UNSIGNED_LO2_16
#   define B_UNSIGNED_LO2_32 D_UNSIGNED_LO2_32
#   define B_UNSIGNED_LO2_64 D_UNSIGNED_LO2_64
#   define B_UNSIGNED_LO3_16 D_UNSIGNED_LO3_16
#   define B_UNSIGNED_LO3_32 D_UNSIGNED_LO3_32
#   define B_UNSIGNED_LO3_64 D_UNSIGNED_LO3_64
#   define B_UNSIGNED_LO_16  D_UNSIGNED_LO_16
#   define B_UNSIGNED_LO_32  D_UNSIGNED_LO_32
#   define B_UNSIGNED_LO_64  D_UNSIGNED_LO_64
#   define B_UNION D_UNION
#   define B_COMPLEX D_COMPLEX
#   define B_HIDDEN_BIT_MASK D_HIDDEN_BIT_MASK
#   define B_SIGN_BIT_MASK D_SIGN_BIT_MASK
#   define B_EXP_MASK D_EXP_MASK
#   define B_SIGN_EXP_MASK D_SIGN_EXP_MASK
#   define B_MANTISSA_MASK D_MANTISSA_MASK
#   define B_MAX_BIASED_EXP D_MAX_BIASED_EXP

#   define B_SET_25TH_BIT(x) { \
        B_UNION u; \
        u.f = (x); \
        u.B_LO_WORD |= ((U_WORD)1 << B_25TH_BIT_POS); \
        (x) = u.f; \
    }

#   define B_CLEAR_25TH_BIT(x) { \
        B_UNION u; \
        u.f = (x); \
        u.B_LO_WORD &= ~((U_WORD)1 << B_25TH_BIT_POS); \
        (x) = u.f; \
    }

#   define R_FORMAT S_FORMAT
#   define R_CHAR S_CHAR
#   define R_TYPE S_TYPE
#   define BITS_PER_R_TYPE BITS_PER_S_TYPE
#   define R_NORM S_NORM
#   define R_PRECISION S_PRECISION
#   define R_LSB_POS S_LSB_POS
#   define R_MSB_POS S_MSB_POS
#   define R_EXP_POS S_EXP_POS
#   define R_EXP_BIAS S_EXP_BIAS
#   define R_EXP_WIDTH S_EXP_WIDTH
#   define R_MIN_BIN_EXP S_MIN_BIN_EXP
#   define R_MAX_BIN_EXP S_MAX_BIN_EXP
#   define R_MIN_DEC_EXP S_MIN_DEC_EXP
#   define R_MAX_DEC_EXP S_MAX_DEC_EXP
#   define R_SIGN_BIT_POS S_SIGN_BIT_POS
#   define R_25TH_BIT_POS S_25TH_BIT_POS
#   define R_HI_WORD  S_HI_WORD
#   define R_LO1_WORD S_LO1_WORD
#   define R_LO2_WORD S_LO2_WORD
#   define R_LO3_WORD S_LO3_WORD
#   define R_LO_WORD  S_LO_WORD
#   define R_SIGNED_HI_WORD  S_SIGNED_HI_WORD
#   define R_SIGNED_HI_16    S_SIGNED_HI_16
#   define R_SIGNED_HI_32    S_SIGNED_HI_32
#   define R_SIGNED_HI_64    S_SIGNED_HI_64
#   define R_SIGNED_LO1_16   S_SIGNED_LO1_16
#   define R_SIGNED_LO1_32   S_SIGNED_LO1_32
#   define R_SIGNED_LO1_64   S_SIGNED_LO1_64
#   define R_SIGNED_LO2_16   S_SIGNED_LO2_16
#   define R_SIGNED_LO2_32   S_SIGNED_LO2_32
#   define R_SIGNED_LO2_64   S_SIGNED_LO2_64
#   define R_SIGNED_LO3_16   S_SIGNED_LO3_16
#   define R_SIGNED_LO3_32   S_SIGNED_LO3_32
#   define R_SIGNED_LO3_64   S_SIGNED_LO3_64
#   define R_SIGNED_LO_16    S_SIGNED_LO_16
#   define R_SIGNED_LO_32    S_SIGNED_LO_32
#   define R_SIGNED_LO_64    S_SIGNED_LO_64
#   define R_UNSIGNED_HI_16  S_UNSIGNED_HI_16
#   define R_UNSIGNED_HI_32  S_UNSIGNED_HI_32
#   define R_UNSIGNED_HI_64  S_UNSIGNED_HI_64
#   define R_UNSIGNED_LO1_16 S_UNSIGNED_LO1_16
#   define R_UNSIGNED_LO1_32 S_UNSIGNED_LO1_32
#   define R_UNSIGNED_LO1_64 S_UNSIGNED_LO1_64
#   define R_UNSIGNED_LO2_16 S_UNSIGNED_LO2_16
#   define R_UNSIGNED_LO2_32 S_UNSIGNED_LO2_32
#   define R_UNSIGNED_LO2_64 S_UNSIGNED_LO2_64
#   define R_UNSIGNED_LO3_16 S_UNSIGNED_LO3_16
#   define R_UNSIGNED_LO3_32 S_UNSIGNED_LO3_32
#   define R_UNSIGNED_LO3_64 S_UNSIGNED_LO3_64
#   define R_UNSIGNED_LO_16  S_UNSIGNED_LO_16
#   define R_UNSIGNED_LO_32  S_UNSIGNED_LO_32
#   define R_UNSIGNED_LO_64  S_UNSIGNED_LO_64
#   define R_UNION S_UNION
#   define R_COMPLEX S_COMPLEX
#   define R_HIDDEN_BIT_MASK S_HIDDEN_BIT_MASK
#   define R_SIGN_BIT_MASK S_SIGN_BIT_MASK
#   define R_EXP_MASK S_EXP_MASK
#   define R_SIGN_EXP_MASK S_SIGN_EXP_MASK
#   define R_MANTISSA_MASK S_MANTISSA_MASK
#   define R_MAX_BIASED_EXP S_MAX_BIASED_EXP

#elif (DOUBLE_PRECISION)


#   define F_CHAR D_CHAR
#   define F_TYPE D_TYPE
#   define BITS_PER_F_TYPE BITS_PER_D_TYPE
#   define F_NORM D_NORM
#   define F_PRECISION D_PRECISION
#   define F_LSB_POS D_LSB_POS
#   define F_MSB_POS D_MSB_POS
#   define F_EXP_POS D_EXP_POS
#   define F_EXP_BIAS D_EXP_BIAS
#   define F_EXP_WIDTH D_EXP_WIDTH
#   define F_MIN_BIN_EXP D_MIN_BIN_EXP
#   define F_MAX_BIN_EXP D_MAX_BIN_EXP
#   define F_MIN_DEC_EXP D_MIN_DEC_EXP
#   define F_MAX_DEC_EXP D_MAX_DEC_EXP
#   define F_SIGN_BIT_POS D_SIGN_BIT_POS
#   define F_25TH_BIT_POS D_25TH_BIT_POS
#   define F_HI_WORD  D_HI_WORD
#   define F_LO1_WORD D_LO1_WORD
#   define F_LO2_WORD D_LO2_WORD
#   define F_LO3_WORD D_LO3_WORD
#   define F_LO_WORD  D_LO_WORD
#   define F_SIGNED_HI_WORD  D_SIGNED_HI_WORD
#   define F_SIGNED_HI_16    D_SIGNED_HI_16
#   define F_SIGNED_HI_32    D_SIGNED_HI_32
#   define F_SIGNED_HI_64    D_SIGNED_HI_64
#   define F_SIGNED_LO1_16   D_SIGNED_LO1_16
#   define F_SIGNED_LO1_32   D_SIGNED_LO1_32
#   define F_SIGNED_LO1_64   D_SIGNED_LO1_64
#   define F_SIGNED_LO2_16   D_SIGNED_LO2_16
#   define F_SIGNED_LO2_32   D_SIGNED_LO2_32
#   define F_SIGNED_LO2_64   D_SIGNED_LO2_64
#   define F_SIGNED_LO3_16   D_SIGNED_LO3_16
#   define F_SIGNED_LO3_32   D_SIGNED_LO3_32
#   define F_SIGNED_LO3_64   D_SIGNED_LO3_64
#   define F_SIGNED_LO_16    D_SIGNED_LO_16
#   define F_SIGNED_LO_32    D_SIGNED_LO_32
#   define F_SIGNED_LO_64    D_SIGNED_LO_64
#   define F_UNSIGNED_HI_16  D_UNSIGNED_HI_16
#   define F_UNSIGNED_HI_32  D_UNSIGNED_HI_32
#   define F_UNSIGNED_HI_64  D_UNSIGNED_HI_64
#   define F_UNSIGNED_LO1_16 D_UNSIGNED_LO1_16
#   define F_UNSIGNED_LO1_32 D_UNSIGNED_LO1_32
#   define F_UNSIGNED_LO1_64 D_UNSIGNED_LO1_64
#   define F_UNSIGNED_LO2_16 D_UNSIGNED_LO2_16
#   define F_UNSIGNED_LO2_32 D_UNSIGNED_LO2_32
#   define F_UNSIGNED_LO2_64 D_UNSIGNED_LO2_64
#   define F_UNSIGNED_LO3_16 D_UNSIGNED_LO3_16
#   define F_UNSIGNED_LO3_32 D_UNSIGNED_LO3_32
#   define F_UNSIGNED_LO3_64 D_UNSIGNED_LO3_64
#   define F_UNSIGNED_LO_16  D_UNSIGNED_LO_16
#   define F_UNSIGNED_LO_32  D_UNSIGNED_LO_32
#   define F_UNSIGNED_LO_64  D_UNSIGNED_LO_64
#   define F_UNION D_UNION
#   define F_COMPLEX D_COMPLEX
#   define F_HIDDEN_BIT_MASK D_HIDDEN_BIT_MASK
#   define F_SIGN_BIT_MASK D_SIGN_BIT_MASK
#   define F_EXP_MASK D_EXP_MASK
#   define F_SIGN_EXP_MASK D_SIGN_EXP_MASK
#   define F_MANTISSA_MASK D_MANTISSA_MASK
#   define F_MAX_BIASED_EXP D_MAX_BIASED_EXP

#   define B_CHAR D_CHAR
#   define B_TYPE D_TYPE
#   define BITS_PER_B_TYPE BITS_PER_D_TYPE
#   define B_NORM D_NORM
#   define B_PRECISION D_PRECISION
#   define B_LSB_POS D_LSB_POS
#   define B_MSB_POS D_MSB_POS
#   define B_EXP_POS D_EXP_POS
#   define B_EXP_BIAS D_EXP_BIAS
#   define B_EXP_WIDTH D_EXP_WIDTH
#   define B_MIN_BIN_EXP D_MIN_BIN_EXP
#   define B_MAX_BIN_EXP D_MAX_BIN_EXP
#   define B_MIN_DEC_EXP D_MIN_DEC_EXP
#   define B_MAX_DEC_EXP D_MAX_DEC_EXP
#   define B_SIGN_BIT_POS D_SIGN_BIT_POS
#   define B_25TH_BIT_POS D_25TH_BIT_POS
#   define B_HI_WORD  D_HI_WORD
#   define B_LO1_WORD D_LO1_WORD
#   define B_LO2_WORD D_LO2_WORD
#   define B_LO3_WORD D_LO3_WORD
#   define B_LO_WORD  D_LO_WORD
#   define B_SIGNED_HI_WORD  D_SIGNED_HI_WORD
#   define B_SIGNED_HI_16    D_SIGNED_HI_16
#   define B_SIGNED_HI_32    D_SIGNED_HI_32
#   define B_SIGNED_HI_64    D_SIGNED_HI_64
#   define B_SIGNED_LO1_16   D_SIGNED_LO1_16
#   define B_SIGNED_LO1_32   D_SIGNED_LO1_32
#   define B_SIGNED_LO1_64   D_SIGNED_LO1_64
#   define B_SIGNED_LO2_16   D_SIGNED_LO2_16
#   define B_SIGNED_LO2_32   D_SIGNED_LO2_32
#   define B_SIGNED_LO2_64   D_SIGNED_LO2_64
#   define B_SIGNED_LO3_16   D_SIGNED_LO3_16
#   define B_SIGNED_LO3_32   D_SIGNED_LO3_32
#   define B_SIGNED_LO3_64   D_SIGNED_LO3_64
#   define B_SIGNED_LO_16    D_SIGNED_LO_16
#   define B_SIGNED_LO_32    D_SIGNED_LO_32
#   define B_SIGNED_LO_64    D_SIGNED_LO_64
#   define B_UNSIGNED_HI_16  D_UNSIGNED_HI_16
#   define B_UNSIGNED_HI_32  D_UNSIGNED_HI_32
#   define B_UNSIGNED_HI_64  D_UNSIGNED_HI_64
#   define B_UNSIGNED_LO1_16 D_UNSIGNED_LO1_16
#   define B_UNSIGNED_LO1_32 D_UNSIGNED_LO1_32
#   define B_UNSIGNED_LO1_64 D_UNSIGNED_LO1_64
#   define B_UNSIGNED_LO2_16 D_UNSIGNED_LO2_16
#   define B_UNSIGNED_LO2_32 D_UNSIGNED_LO2_32
#   define B_UNSIGNED_LO2_64 D_UNSIGNED_LO2_64
#   define B_UNSIGNED_LO3_16 D_UNSIGNED_LO3_16
#   define B_UNSIGNED_LO3_32 D_UNSIGNED_LO3_32
#   define B_UNSIGNED_LO3_64 D_UNSIGNED_LO3_64
#   define B_UNSIGNED_LO_16  D_UNSIGNED_LO_16
#   define B_UNSIGNED_LO_32  D_UNSIGNED_LO_32
#   define B_UNSIGNED_LO_64  D_UNSIGNED_LO_64
#   define B_UNION D_UNION
#   define B_COMPLEX D_COMPLEX
#   define B_HIDDEN_BIT_MASK D_HIDDEN_BIT_MASK
#   define B_SIGN_BIT_MASK D_SIGN_BIT_MASK
#   define B_EXP_MASK D_EXP_MASK
#   define B_SIGN_EXP_MASK D_SIGN_EXP_MASK
#   define B_MANTISSA_MASK D_MANTISSA_MASK
#   define B_MAX_BIASED_EXP D_MAX_BIASED_EXP


#   define R_FORMAT S_FORMAT
#   define R_CHAR S_CHAR
#   define R_TYPE S_TYPE
#   define BITS_PER_R_TYPE BITS_PER_S_TYPE
#   define R_NORM S_NORM
#   define R_PRECISION S_PRECISION
#   define R_LSB_POS S_LSB_POS
#   define R_MSB_POS S_MSB_POS
#   define R_EXP_POS S_EXP_POS
#   define R_EXP_BIAS S_EXP_BIAS
#   define R_EXP_WIDTH S_EXP_WIDTH
#   define R_MIN_BIN_EXP S_MIN_BIN_EXP
#   define R_MAX_BIN_EXP S_MAX_BIN_EXP
#   define R_MIN_DEC_EXP S_MIN_DEC_EXP
#   define R_MAX_DEC_EXP S_MAX_DEC_EXP
#   define R_SIGN_BIT_POS S_SIGN_BIT_POS
#   define R_25TH_BIT_POS S_25TH_BIT_POS
#   define R_HI_WORD  S_HI_WORD
#   define R_LO1_WORD S_LO1_WORD
#   define R_LO2_WORD S_LO2_WORD
#   define R_LO3_WORD S_LO3_WORD
#   define R_LO_WORD  S_LO_WORD
#   define R_SIGNED_HI_WORD  S_SIGNED_HI_WORD
#   define R_SIGNED_HI_16    S_SIGNED_HI_16
#   define R_SIGNED_HI_32    S_SIGNED_HI_32
#   define R_SIGNED_HI_64    S_SIGNED_HI_64
#   define R_SIGNED_LO1_16   S_SIGNED_LO1_16
#   define R_SIGNED_LO1_32   S_SIGNED_LO1_32
#   define R_SIGNED_LO1_64   S_SIGNED_LO1_64
#   define R_SIGNED_LO2_16   S_SIGNED_LO2_16
#   define R_SIGNED_LO2_32   S_SIGNED_LO2_32
#   define R_SIGNED_LO2_64   S_SIGNED_LO2_64
#   define R_SIGNED_LO3_16   S_SIGNED_LO3_16
#   define R_SIGNED_LO3_32   S_SIGNED_LO3_32
#   define R_SIGNED_LO3_64   S_SIGNED_LO3_64
#   define R_SIGNED_LO_16    S_SIGNED_LO_16
#   define R_SIGNED_LO_32    S_SIGNED_LO_32
#   define R_SIGNED_LO_64    S_SIGNED_LO_64
#   define R_UNSIGNED_HI_16  S_UNSIGNED_HI_16
#   define R_UNSIGNED_HI_32  S_UNSIGNED_HI_32
#   define R_UNSIGNED_HI_64  S_UNSIGNED_HI_64
#   define R_UNSIGNED_LO1_16 S_UNSIGNED_LO1_16
#   define R_UNSIGNED_LO1_32 S_UNSIGNED_LO1_32
#   define R_UNSIGNED_LO1_64 S_UNSIGNED_LO1_64
#   define R_UNSIGNED_LO2_16 S_UNSIGNED_LO2_16
#   define R_UNSIGNED_LO2_32 S_UNSIGNED_LO2_32
#   define R_UNSIGNED_LO2_64 S_UNSIGNED_LO2_64
#   define R_UNSIGNED_LO3_16 S_UNSIGNED_LO3_16
#   define R_UNSIGNED_LO3_32 S_UNSIGNED_LO3_32
#   define R_UNSIGNED_LO3_64 S_UNSIGNED_LO3_64
#   define R_UNSIGNED_LO_16  S_UNSIGNED_LO_16
#   define R_UNSIGNED_LO_32  S_UNSIGNED_LO_32
#   define R_UNSIGNED_LO_64  S_UNSIGNED_LO_64
#   define R_UNION S_UNION
#   define R_COMPLEX S_COMPLEX
#   define R_HIDDEN_BIT_MASK S_HIDDEN_BIT_MASK
#   define R_SIGN_BIT_MASK S_SIGN_BIT_MASK
#   define R_EXP_MASK S_EXP_MASK
#   define R_SIGN_EXP_MASK S_SIGN_EXP_MASK
#   define R_MANTISSA_MASK S_MANTISSA_MASK
#   define R_MAX_BIASED_EXP S_MAX_BIASED_EXP


#elif (QUAD_PRECISION)


#   define F_CHAR Q_CHAR
#   define F_TYPE Q_TYPE
#   define BITS_PER_F_TYPE BITS_PER_Q_TYPE
#   define F_NORM Q_NORM
#   define F_PRECISION Q_PRECISION
#   define F_LSB_POS Q_LSB_POS
#   define F_MSB_POS Q_MSB_POS
#   define F_EXP_POS Q_EXP_POS
#   define F_EXP_BIAS Q_EXP_BIAS
#   define F_EXP_WIDTH Q_EXP_WIDTH
#   define F_MIN_BIN_EXP Q_MIN_BIN_EXP
#   define F_MAX_BIN_EXP Q_MAX_BIN_EXP
#   define F_MIN_DEC_EXP Q_MIN_DEC_EXP
#   define F_MAX_DEC_EXP Q_MAX_DEC_EXP
#   define F_SIGN_BIT_POS Q_SIGN_BIT_POS
#   define F_25TH_BIT_POS Q_25TH_BIT_POS
#   define F_HI_WORD  Q_HI_WORD
#   define F_LO1_WORD Q_LO1_WORD
#   define F_LO2_WORD Q_LO2_WORD
#   define F_LO3_WORD Q_LO3_WORD
#   define F_LO_WORD  Q_LO_WORD
#   define F_SIGNED_HI_WORD  Q_SIGNED_HI_WORD
#   define F_SIGNED_HI_16    Q_SIGNED_HI_16
#   define F_SIGNED_HI_32    Q_SIGNED_HI_32
#   define F_SIGNED_HI_64    Q_SIGNED_HI_64
#   define F_SIGNED_LO1_16   Q_SIGNED_LO1_16
#   define F_SIGNED_LO1_32   Q_SIGNED_LO1_32
#   define F_SIGNED_LO1_64   Q_SIGNED_LO1_64
#   define F_SIGNED_LO2_16   Q_SIGNED_LO2_16
#   define F_SIGNED_LO2_32   Q_SIGNED_LO2_32
#   define F_SIGNED_LO2_64   Q_SIGNED_LO2_64
#   define F_SIGNED_LO3_16   Q_SIGNED_LO3_16
#   define F_SIGNED_LO3_32   Q_SIGNED_LO3_32
#   define F_SIGNED_LO3_64   Q_SIGNED_LO3_64
#   define F_SIGNED_LO_16    Q_SIGNED_LO_16
#   define F_SIGNED_LO_32    Q_SIGNED_LO_32
#   define F_SIGNED_LO_64    Q_SIGNED_LO_64
#   define F_UNSIGNED_HI_16  Q_UNSIGNED_HI_16
#   define F_UNSIGNED_HI_32  Q_UNSIGNED_HI_32
#   define F_UNSIGNED_HI_64  Q_UNSIGNED_HI_64
#   define F_UNSIGNED_LO1_16 Q_UNSIGNED_LO1_16
#   define F_UNSIGNED_LO1_32 Q_UNSIGNED_LO1_32
#   define F_UNSIGNED_LO1_64 Q_UNSIGNED_LO1_64
#   define F_UNSIGNED_LO2_16 Q_UNSIGNED_LO2_16
#   define F_UNSIGNED_LO2_32 Q_UNSIGNED_LO2_32
#   define F_UNSIGNED_LO2_64 Q_UNSIGNED_LO2_64
#   define F_UNSIGNED_LO3_16 Q_UNSIGNED_LO3_16
#   define F_UNSIGNED_LO3_32 Q_UNSIGNED_LO3_32
#   define F_UNSIGNED_LO3_64 Q_UNSIGNED_LO3_64
#   define F_UNSIGNED_LO_16  Q_UNSIGNED_LO_16
#   define F_UNSIGNED_LO_32  Q_UNSIGNED_LO_32
#   define F_UNSIGNED_LO_64  Q_UNSIGNED_LO_64
#   define F_UNION Q_UNION
#   define F_COMPLEX Q_COMPLEX
#   define F_HIDDEN_BIT_MASK Q_HIDDEN_BIT_MASK
#   define F_SIGN_BIT_MASK Q_SIGN_BIT_MASK
#   define F_EXP_MASK Q_EXP_MASK
#   define F_SIGN_EXP_MASK Q_SIGN_EXP_MASK
#   define F_MANTISSA_MASK Q_MANTISSA_MASK
#   define F_MAX_BIASED_EXP Q_MAX_BIASED_EXP

#   define B_CHAR Q_CHAR
#   define B_TYPE Q_TYPE
#   define BITS_PER_B_TYPE BITS_PER_Q_TYPE
#   define B_NORM Q_NORM
#   define B_PRECISION Q_PRECISION
#   define B_LSB_POS Q_LSB_POS
#   define B_MSB_POS Q_MSB_POS
#   define B_EXP_POS Q_EXP_POS
#   define B_EXP_BIAS Q_EXP_BIAS
#   define B_EXP_WIDTH Q_EXP_WIDTH
#   define B_MIN_BIN_EXP Q_MIN_BIN_EXP
#   define B_MAX_BIN_EXP Q_MAX_BIN_EXP
#   define B_MIN_DEC_EXP Q_MIN_DEC_EXP
#   define B_MAX_DEC_EXP Q_MAX_DEC_EXP
#   define B_SIGN_BIT_POS Q_SIGN_BIT_POS
#   define B_25TH_BIT_POS Q_25TH_BIT_POS
#   define B_HI_WORD  Q_HI_WORD
#   define B_LO1_WORD Q_LO1_WORD
#   define B_LO2_WORD Q_LO2_WORD
#   define B_LO3_WORD Q_LO3_WORD
#   define B_LO_WORD  Q_LO_WORD
#   define B_SIGNED_HI_WORD  Q_SIGNED_HI_WORD
#   define B_SIGNED_HI_16    Q_SIGNED_HI_16
#   define B_SIGNED_HI_32    Q_SIGNED_HI_32
#   define B_SIGNED_HI_64    Q_SIGNED_HI_64
#   define B_SIGNED_LO1_16   Q_SIGNED_LO1_16
#   define B_SIGNED_LO1_32   Q_SIGNED_LO1_32
#   define B_SIGNED_LO1_64   Q_SIGNED_LO1_64
#   define B_SIGNED_LO2_16   Q_SIGNED_LO2_16
#   define B_SIGNED_LO2_32   Q_SIGNED_LO2_32
#   define B_SIGNED_LO2_64   Q_SIGNED_LO2_64
#   define B_SIGNED_LO3_16   Q_SIGNED_LO3_16
#   define B_SIGNED_LO3_32   Q_SIGNED_LO3_32
#   define B_SIGNED_LO3_64   Q_SIGNED_LO3_64
#   define B_SIGNED_LO_16    Q_SIGNED_LO_16
#   define B_SIGNED_LO_32    Q_SIGNED_LO_32
#   define B_SIGNED_LO_64    Q_SIGNED_LO_64
#   define B_UNSIGNED_HI_16  Q_UNSIGNED_HI_16
#   define B_UNSIGNED_HI_32  Q_UNSIGNED_HI_32
#   define B_UNSIGNED_HI_64  Q_UNSIGNED_HI_64
#   define B_UNSIGNED_LO1_16 Q_UNSIGNED_LO1_16
#   define B_UNSIGNED_LO1_32 Q_UNSIGNED_LO1_32
#   define B_UNSIGNED_LO1_64 Q_UNSIGNED_LO1_64
#   define B_UNSIGNED_LO2_16 Q_UNSIGNED_LO2_16
#   define B_UNSIGNED_LO2_32 Q_UNSIGNED_LO2_32
#   define B_UNSIGNED_LO2_64 Q_UNSIGNED_LO2_64
#   define B_UNSIGNED_LO3_16 Q_UNSIGNED_LO3_16
#   define B_UNSIGNED_LO3_32 Q_UNSIGNED_LO3_32
#   define B_UNSIGNED_LO3_64 Q_UNSIGNED_LO3_64
#   define B_UNSIGNED_LO_16  Q_UNSIGNED_LO_16
#   define B_UNSIGNED_LO_32  Q_UNSIGNED_LO_32
#   define B_UNSIGNED_LO_64  Q_UNSIGNED_LO_64
#   define B_UNION Q_UNION
#   define B_COMPLEX Q_COMPLEX
#   define B_HIDDEN_BIT_MASK Q_HIDDEN_BIT_MASK
#   define B_SIGN_BIT_MASK Q_SIGN_BIT_MASK
#   define B_EXP_MASK Q_EXP_MASK
#   define B_SIGN_EXP_MASK Q_SIGN_EXP_MASK
#   define B_MANTISSA_MASK Q_MANTISSA_MASK
#   define B_MAX_BIASED_EXP Q_MAX_BIASED_EXP


#   define R_FORMAT D_FORMAT
#   define R_CHAR D_CHAR
#   define R_TYPE D_TYPE
#   define BITS_PER_R_TYPE BITS_PER_D_TYPE
#   define R_NORM D_NORM
#   define R_PRECISION D_PRECISION
#   define R_LSB_POS D_LSB_POS
#   define R_MSB_POS D_MSB_POS
#   define R_EXP_POS D_EXP_POS
#   define R_EXP_BIAS D_EXP_BIAS
#   define R_EXP_WIDTH D_EXP_WIDTH
#   define R_MIN_BIN_EXP D_MIN_BIN_EXP
#   define R_MAX_BIN_EXP D_MAX_BIN_EXP
#   define R_MIN_DEC_EXP D_MIN_DEC_EXP
#   define R_MAX_DEC_EXP D_MAX_DEC_EXP
#   define R_SIGN_BIT_POS D_SIGN_BIT_POS
#   define R_25TH_BIT_POS D_25TH_BIT_POS
#   define R_HI_WORD  D_HI_WORD
#   define R_LO1_WORD D_LO1_WORD
#   define R_LO2_WORD D_LO2_WORD
#   define R_LO3_WORD D_LO3_WORD
#   define R_LO_WORD  D_LO_WORD
#   define R_SIGNED_HI_WORD  D_SIGNED_HI_WORD
#   define R_SIGNED_HI_16    D_SIGNED_HI_16
#   define R_SIGNED_HI_32    D_SIGNED_HI_32
#   define R_SIGNED_HI_64    D_SIGNED_HI_64
#   define R_SIGNED_LO1_16   D_SIGNED_LO1_16
#   define R_SIGNED_LO1_32   D_SIGNED_LO1_32
#   define R_SIGNED_LO1_64   D_SIGNED_LO1_64
#   define R_SIGNED_LO2_16   D_SIGNED_LO2_16
#   define R_SIGNED_LO2_32   D_SIGNED_LO2_32
#   define R_SIGNED_LO2_64   D_SIGNED_LO2_64
#   define R_SIGNED_LO3_16   D_SIGNED_LO3_16
#   define R_SIGNED_LO3_32   D_SIGNED_LO3_32
#   define R_SIGNED_LO3_64   D_SIGNED_LO3_64
#   define R_SIGNED_LO_16    D_SIGNED_LO_16
#   define R_SIGNED_LO_32    D_SIGNED_LO_32
#   define R_SIGNED_LO_64    D_SIGNED_LO_64
#   define R_UNSIGNED_HI_16  D_UNSIGNED_HI_16
#   define R_UNSIGNED_HI_32  D_UNSIGNED_HI_32
#   define R_UNSIGNED_HI_64  D_UNSIGNED_HI_64
#   define R_UNSIGNED_LO1_16 D_UNSIGNED_LO1_16
#   define R_UNSIGNED_LO1_32 D_UNSIGNED_LO1_32
#   define R_UNSIGNED_LO1_64 D_UNSIGNED_LO1_64
#   define R_UNSIGNED_LO2_16 D_UNSIGNED_LO2_16
#   define R_UNSIGNED_LO2_32 D_UNSIGNED_LO2_32
#   define R_UNSIGNED_LO2_64 D_UNSIGNED_LO2_64
#   define R_UNSIGNED_LO3_16 D_UNSIGNED_LO3_16
#   define R_UNSIGNED_LO3_32 D_UNSIGNED_LO3_32
#   define R_UNSIGNED_LO3_64 D_UNSIGNED_LO3_64
#   define R_UNSIGNED_LO_16  D_UNSIGNED_LO_16
#   define R_UNSIGNED_LO_32  D_UNSIGNED_LO_32
#   define R_UNSIGNED_LO_64  D_UNSIGNED_LO_64
#   define R_UNION D_UNION
#   define R_COMPLEX D_COMPLEX
#   define R_HIDDEN_BIT_MASK D_HIDDEN_BIT_MASK
#   define R_SIGN_BIT_MASK D_SIGN_BIT_MASK
#   define R_EXP_MASK D_EXP_MASK
#   define R_SIGN_EXP_MASK D_SIGN_EXP_MASK
#   define R_MANTISSA_MASK D_MANTISSA_MASK
#   define R_MAX_BIASED_EXP D_MAX_BIASED_EXP


#else

#   error Unrecognized floating precision.

#endif

/* Fix up for x_MANTISSA_MASK */
#if BITS_PER_WORD > BITS_PER_F_TYPE
#   define __F_TYPE_BIT_MASK	MAKE_MASK(BITS_PER_F_TYPE, 0)
#else
#   define __F_TYPE_BIT_MASK	((U_WORD) (-1))
#endif


#undef  RANGE_BACKUP_AVAILABLE
#if (defined(B_FORMAT) \
    && defined(B_TYPE) \
    && (B_PRECISION >= F_PRECISION) \
    && (B_EXP_WIDTH >= F_EXP_WIDTH + 1))
#   define RANGE_BACKUP_AVAILABLE 1
#endif

#undef  PRECISION_BACKUP_AVAILABLE
#if (defined(B_FORMAT) \
    && defined(B_TYPE) \
    && (B_EXP_WIDTH >= F_EXP_WIDTH) \
    && (B_PRECISION >= F_PRECISION * 2))
#   define PRECISION_BACKUP_AVAILABLE 1
#endif


#define HI(a) PASTE(a,_hi)
#define LO(a) PASTE(a,_lo)


#if (PRECISION_BACKUP_AVAILABLE)
#   define DECLARE_PREC_BACKUP(x) B_TYPE x
#else
#   define DECLARE_PREC_BACKUP(x) F_TYPE HI(x), LO(x) 
#endif

#if USE_BACKUP
#   define USE_BACKUP	1
#elif !defined(USE_BACKUP)&& RANGE_BACKUP_AVAILABLE &&PRECISION_BACKUP_AVAILABLE
#   define USE_BACKUP	1
#else
#   define USE_BACKUP	0
#endif

#if USE_BACKUP
#   define IF_BACKUP(x)	x
#   define IF_NO_BACKUP(x)
#   define BACKUP_SELECT(x,y)	x
#else
#   define IF_BACKUP(x)	 
#   define IF_NO_BACKUP(x)	x
#   define BACKUP_SELECT(x,y)	y
#endif


#undef  PDP_SHUFFLE
#if (VAX_FLOATING)

#   if (BITS_PER_WORD == 32)

#       define PDP_SHUFFLE(i) \
            (((U_WORD)(i) << 16) | ((U_WORD)(i) >> 16))

#   elif (BITS_PER_F_TYPE < BITS_PER_WORD)

#       define PDP_SHUFFLE(i) \
            (((U_WORD)((i) & 0xffff) << 16) | ((U_WORD)(i) >> 16))

#       define SIGN_EXTENDED_PDP_SHUFFLE(i) \
            ( (((WORD)((U_WORD)(i) << (BITS_PER_WORD - 16))) \
                                                     >> (BITS_PER_WORD - 32)) \
            | ((U_WORD)(i) >> 16) )

#   elif (BITS_PER_WORD == 64)

#       define PDP_SHUFFLE(i) \
            ( ((U_WORD)(i) << 48) \
            | ((U_WORD)(i) >> 48) \
            | (((U_WORD)(i) >> 16) & ((U_WORD)0xffff << 16)) \
            | (((U_WORD)(i) << 16) & ((U_WORD)0xffff << 32)) ) \

#   else

#       error PDP_SHUFFLE macro not defined for this word size.

#   endif

#else
#   define PDP_SHUFFLE(i) (i)
#endif

    /*  In most cases a SIGN_EXTENDED_PDP_SHUFFLE and a PDP_SHUFFLE are the 
     *  same.  So if SIGN_EXTENDED_PDP_SHUFFLE is not defined above define
     *  it to be PDP_SHUFFLE.
     */

#if !defined(SIGN_EXTENDED_PDP_SHUFFLE)
#    define SIGN_EXTENDED_PDP_SHUFFLE(i) PDP_SHUFFLE(i)
#endif

/*
 * Currently _WORDS_PER_F_TYPE and _F_WORD are only used in the
 * xxx_LOW_BIT_yyy macros
 */

#define _WORDS_PER_F_TYPE   (BITS_PER_F_TYPE/BITS_PER_WORD)
#define _F_WORD(u,n)        (u).uw[ UNION_IX(_WORDS_PER_F_TYPE, n) ]

#if (_WORDS_PER_F_TYPE <= 1)
#    define OR_LOW_BITS_SET(u)
#    define AND_LOW_BITS_CLEAR(u)
#    define CLEAR_LOW_BITS(u)
#elif (_WORDS_PER_F_TYPE == 2)
#    define OR_LOW_BITS_SET(u)      | _F_WORD(u,1)
#    define AND_LOW_BITS_CLEAR(u)   && (_F_WORD(u,1) == 0)
#    define CLEAR_LOW_BITS(u)       (_F_WORD(u,1) = 0)
#elif (_WORDS_PER_F_TYPE == 3)
#    define OR_LOW_BITS_SET(u)      | _F_WORD(u,1) | _F_WORD(u,2) 
#    define AND_LOW_BITS_CLEAR(u)   && ((_F_WORD(u,1) | _F_WORD(u,2)  == 0)
#    define CLEAR_LOW_BITS(u)       (_F_WORD(u,1) = 0, _F_WORD(u,2) = 0)
#elif (_WORDS_PER_F_TYPE == 4)
#    define OR_LOW_BITS_SET(u)      | _F_WORD(u,1) | _F_WORD(u,2) | _F_WORD(u,3)
#    define AND_LOW_BITS_CLEAR(u)   && ((_F_WORD(u,1) | _F_WORD(u,2) |_F_WORD(u,3)) == 0)
#    define CLEAR_LOW_BITS(u)       (_F_WORD(u,1) = 0, _F_WORD(u,2) = 0, _F_WORD(u,3) = 0)
#else
#    error "Unsupport combinition of WORD and F_TYPE"
#endif

#if NEW_DPML_MACROS == 1

#   if !defined(VAX_FLOATING)
#      define VAX_FLOATING	0
#   else
#      define IEEE_FLOATING	0
#   endif

#   if IEEE_FLOATING
#      define IF_IEEE(x)	x
#      define IF_VAX(x)
#      define IEEE_SELECT(x,y)	x
#      define VMS_SELECT(x,y)	y
#   else
#      define IF_IEEE(x)	
#      define IF_VAX(x)	x
#      define IEEE_SELECT(x,y)	y
#      define VMS_SELECT(x,y)	x
#   endif

#   if SINGLE_PRECISION
#      define IF_SNGL(x)	x
#      define IF_DBLE(x)
#      define IF_QUAD(x)
#      define PREC_SELECT(x,y)	x
#   elif DOUBLE_PRECISION
#      define IF_SNGL(x)
#      define IF_DBLE(x)	x
#      define IF_QUAD(x)
#      define PREC_SELECT(x,y)	y
#   elif QUAD_PRECISION
#      define IF_SNGL(x)
#      define IF_DBLE(x)
#      define IF_QUAD(x)	x
#      define PREC_SELECT(x,y)	y
#   endif

#   define F_HI_HALF_PRECISION	(F_PRECISION - BITS_PER_F_TYPE/2)

#   if SINGLE_PRECISION
#      define F_CLEAR_LO_HALF_WORD(u)		(u).u16[ UNION_IX(1) ] = 0;
#   elif DOUBLE_PRECISION
#      define F_CLEAR_LO_HALF_WORD(u)		(u).u32[ UNION_IX(1) ] = 0;
#   else /* QUAD_PRECISION */
#      if BITS_PER_WORD >= 64
#          define F_CLEAR_LO_HALF_WORD(u)	(u).u64[ UNION_IX(1) ] = 0;
#      else
#          define F_CLEAR_LO_HALF_WORD(u)	(u).u32[ UNION_IX(2) ] = 0; \
						(u).u32[ UNION_IX(3) ] = 0
#      endif
#   endif

#endif

#endif  /* (!NO_FLOATING) */


#endif  /* F_FORMAT_H */
























#if 0



typedef union {
    struct  {
#ifdef __MIPSEL
        unsigned fraction_low: 32;
        unsigned hi_bits: 20;
        unsigned exponent : 11;
        unsigned sign    : 1;
#else
        unsigned sign    : 1;
        unsigned exponent : 11;
        unsigned hi_bits: 20;
        unsigned fraction_low: 32;
#endif
    } bits;
    double d;
} dnan; 



typedef union {
    float f;
    unsigned long  l[1];
    unsigned int   i[1];
    unsigned short s[2];
    unsigned char  c[4];
    struct {
        unsigned hi_bits  : 7;
        unsigned exponent : 8;
        unsigned sign_bit : 1;
        unsigned char    c[2];
    } 
    vax_f_float;
    struct {
        unsigned char    c[2];
        unsigned hi_bits  : 7;
        unsigned exponent : 8;
        unsigned sign_bit : 1;
    } 
    ieee_single;
    struct {
        unsigned char    c[3];
        unsigned exponent : 7;
        unsigned sign_bit : 1;
    } 
    ibm_short;
} 
REAL4;

typedef REAL4 *REAL4_PTR;



typedef union {
    double d;
    unsigned long   l[2];
    unsigned int    i[2];
    unsigned short  s[4];
    unsigned char   c[8];
    struct {
        unsigned hi_bits  : 7;
        unsigned exponent : 8;
        unsigned sign_bit : 1;
        unsigned char    c[6];
    } 
    vax_d_float;
    struct {
        unsigned hi_bits  : 4;
        unsigned exponent : 11;
        unsigned sign_bit : 1;
        unsigned char    c[6];
    } 
    vax_g_float;
    struct {
        unsigned char    c[6];
        unsigned hi_bits  : 4;
        unsigned exponent : 11;
        unsigned sign_bit : 1;
    } 
    ieee_double;
    struct {
        unsigned char    c[7];
        unsigned exponent : 7;
        unsigned sign_bit : 1;
    } 
    ibm_long;
    struct {
        unsigned char    c[6];
        unsigned exponent : 15;
        unsigned sign_bit : 1;
    } 
    cray;
} 
REAL8;

typedef REAL8 *REAL8_PTR;



typedef union {
    unsigned long   l[4];
    unsigned int    i[4];
    unsigned short  s[8];
    unsigned char   c[16];
    struct {
        /*
        unsigned hi_bits  : 0;
        */
        unsigned exponent : 15;
        unsigned sign_bit : 1;
        unsigned char   c[14];
    } vax_h_float;
} REAL16;

typedef REAL16 *REAL16_PTR;

#endif


