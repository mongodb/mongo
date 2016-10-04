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

#if ANSI_C_DEF

#   define IF_ANSI_C(x)	x
#   define ANSI_C_SELECT(x,y)	x
#   define ERROR_FILE_NAME	POW_ANSI_C_ERROR_BUILD_FILE_NAME
#   define ANSI_C_DEF		1

#else

#   define IF_ANSI_C(x)
#   define ANSI_C_SELECT(x,y)	y
#   define ERROR_FILE_NAME	POW_FORTRAN_ERROR_BUILD_FILE_NAME
#   define ANSI_C_DEF		0

#endif

/* Define the evalution precision characteristics and parameters */

#define ALIGN_WITH_B_TYPE_EXP(w)	((U_WORD)(w) << B_EXP_POS)
#define S32_PER_B_TYPE			(BITS_PER_B_TYPE >> 5)
#define __LO_32(n)			UNION_IX(n, (n-1))
#define SIGNED_LO_32			i32[__LO_32(S32_PER_B_TYPE)]
#define UNSIGNED_LO_32			u32[__LO_32(S32_PER_B_TYPE)]


#if BITS_PER_WORD < BITS_PER_B_TYPE
#   define IF_SMALL_WORD(x)	x
#else
#   define IF_SMALL_WORD(x)
#endif

/*
** When BITS_PER_F_TYPE < BITS_PER_WORD, the sign bit of IEEE floating point
** values is not at the high end of a WORD, so that integer tests on the 
** sign of the value don't yield the correct result.
*/

#if BITS_PER_F_TYPE < BITS_PER_WORD
#   define R_WORD		PASTE(INT_, BITS_PER_F_TYPE)
#   define SIGN_EXTEND(x)	((WORD)((R_WORD) (x)))
#else
#   define SIGN_EXTEND(x)	((WORD) (x))
#endif

#define NORMALIZE(x,f)	BACKUP_SELECT(					\
				B_COPY_SIGN_AND_EXP(x, ONE, f),		\
                                F_COPY_SIGN_AND_EXP((B_TYPE) x, ONE, f)	\
				)
/*
** For IEEE types, we get the index bits for the log table by shifting
** right.  But for VAX types we need to do a PDP_SHUFFLE and shift left
**
**	NOTES:
**	   o LOG_INDEX_BASE_POS is computed at table generation time and is
**	     defined in the power table include file.
**	   o For VAX data types, the macro, POSITION_BITS, combines the index
**	     alignment with the PDP_SHUFFLE.
*/

#define LOG_INDEX_SHIFT		    (LOG2_K + LOG_INDEX_BASE_POS)
#define LOG_INDEX_AND_RND_BIT_MASK  MAKE_MASK(LOG2_K+1, LOG_INDEX_BASE_POS-1)
#define LOG_INDEX_MASK		    MAKE_MASK(LOG2_K + 1, LOG_INDEX_BASE_POS)
#define LOG_INDEX_ROUND_BIT	    SET_BIT(LOG_INDEX_BASE_POS - 1)

#if IEEE_FLOATING
#   define POSITION_BITS(ix, exp_pos)	(ix >> (exp_pos - LOG_INDEX_SHIFT))
#else
#   define POSITION_BITS(ix, exp_pos)					\
		((ix << (LOG_INDEX_SHIFT - (exp_pos))) |		\
		 ((U_INT_32)ix >> ((exp_pos) + (32 - LOG_INDEX_SHIFT)))) 
#endif

/*
** When aligning the scale factor with the exponent field of the POW2 table
** entries, we may need to shift right or left depending on the relative
** sizes of POW2_K and E_EXP_POS.  Also, when adding to the exponent field
** of a VAX data type, we must make sure that the addition does not propagate
** beyond the exponent field.  To insure this, we mask off the sign, exponent
** and high bits using LO_MASK (since these bits are in the low part of the
** integer).  Last, but not least, when determining if x^y has an integer
** exponent, we need to do some shifting *AFTER* a PDP_SHUFFLE for VAX
** data types.
*/

#if (B_EXP_POS - POW2_K) >= 0
#   define ALIGN_SCALE_WITH_EXP(m)	((m) << (B_EXP_POS - POW2_K))
#   define PDP_F_EXP_POS                F_EXP_POS
#   define PDP_B_EXP_POS                B_EXP_POS
#else
#   define ALIGN_SCALE_WITH_EXP(m)	((m) >> (POW2_K - B_EXP_POS))
#   define PDP_F_EXP_POS                (F_EXP_POS + BITS_PER_F_TYPE - 16)
#   define PDP_B_EXP_POS                (B_EXP_POS + BITS_PER_B_TYPE - 16)
#endif

#if IEEE_FLOATING
#   define W_ADD_TO_EXP_FIELD(i, j)	((i) + (j))
#else
#   define LO_MASK			MAKE_MASK(F_SIGN_BIT_POS + 1, 0)
#   define W_ADD_TO_EXP_FIELD(i, j)	((i & ~LO_MASK) | ((i + j) & LO_MASK))
#endif

#define POW2_INDEX_MASK		    MAKE_MASK(POW2_K, 0)

#if USE_DIVIDE
#   define DIV_SELECT(x,y)	x
#else
#   define DIV_SELECT(x,y)	y
#endif

/*
** We obtain the nearest integer part of x*(1/ln2) as a floating point value
** by an "add big/sub big" operation.  Getting the nearest integer as an
** integer value is done by extracting the low 32 bits of the "add big"
** operation.  For VAX data types, the low 32 bits must be PDP_SHUFFLED
** before they can be used.
*/

#if IEEE_FLOATING
#   define GET_LOW_32_BITS(i,u)	i = (WORD) ( (INT_32) u.B_LO_WORD )
#else
#   define GET_LOW_32_BITS(i,u)	{					\
				U_INT_32 u32;				\
				u32 = ( U_INT_32) (u.uw[0] >>		\
				    (BITS_PER_WORD - 32));		\
				u32 = ((u32 >> 16) | (u32 << 16));	\
				i = (WORD) ((INT_32) u32);		\
				} 
#endif
