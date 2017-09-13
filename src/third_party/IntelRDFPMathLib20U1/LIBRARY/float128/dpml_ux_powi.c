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

#define	BASE_NAME	powi
#include "dpml_ux.h"

#if !defined(MAKE_INCLUDE)
#   include STR(BUILD_FILE_NAME)
#endif

/*
** The DPML can potentially support 6 different types of power functions with
** a floating point base and a integer power.  Six types are determined by
** whether the integer power is a signed or unsigned integer and whether 0^0
** retun 0, 1 or an error.  The following note discusses a common subroutine,
** __powil, that supports all 6 types of powi functions.
** 
** 
** 1.0 BASIC DESIGN AND INTERFACE
** ------------------------------
** 
** The basic approach to __powil to to encode the behavior of the 0^0 case in
** the class-to-action mapping array.   Specifically, if we denote the exponent
** as n, we create a class-to-action mapping array that has mappings for n < 0,
** n > 0 (both even and odd cases) and three entries for n = 0.  The three
** entries for n = 0 correspond to the three choices for 0^0.
**
** For each of the six possible powi routines, we define an integer, call it
** index_map, consisting of 3, k-bit fields.  The first field contains the
** index into the class-to-action mapping table for n < 0; the second for n = 0;
** and the third for n > 0.  Note that the unsigned integer case is handled by
** making the first and third field of index_map identical.
** 
** The actual algorithm for __powil is fairly simple - it uses the standard
** iterative "square and multiply" approach.  The only difference from the basic
** DPML implementation is that for negative exponents, the reciprocal of the
** argument is used for the iterations rather than performing the reciprocal
** after the iterations.
** 
** It should be pointed out, that this will most likely mean the __powil routine
** will be slightly *SLOWER* than the existing DPML routines for the
** non-exceptional cases.  We might want to consider expanding the MULTIPLY and
** SQUARE operations in-line to improve performance.  The resulting code
** expansion should not be too great (i.e. less that 10%).
*/ 

#if !defined(C_UX_POW_I)
#   define C_UX_POW_I	__INTERNAL_NAME(C_ux_pow_i)
#endif

#define INDEX_INC		(64/BITS_PER_WORD)
#define POWI_INDEX_MASK		MAKE_MASK(EXPONENT_INDEX_FIELD_WIDTH,0)
#define INDEX_MAP(n,z,p)						\
		(((z)             << 0*EXPONENT_INDEX_FIELD_WIDTH) |	\
		 ((p)             << 1*EXPONENT_INDEX_FIELD_WIDTH) |	\
		 (((p)+INDEX_INC) << 2*EXPONENT_INDEX_FIELD_WIDTH) |	\
		 ((n)             << 3*EXPONENT_INDEX_FIELD_WIDTH) |	\
		 (((n)+INDEX_INC) << 4*EXPONENT_INDEX_FIELD_WIDTH) )

static void
C_UX_POW_I(_X_FLOAT * packed_argument,  WORD n, WORD index_map,
   _X_FLOAT * packed_result OPT_EXCEPTION_INFO_DECLARATION )
    {
    WORD fp_class, exponent, index;
    UX_FLOAT unpacked_argument, unpacked_result;

    /*
    ** Get correct index for class-to-action array.  The next line computes
    ** index according to the following table:
    **
    **		     n		index
    **		---------	-----
    **		zero		  0
    **		pos, even	  1
    **		pos, odd	  2
    **		neg, even	  3
    **		neg, odd	  4
    **
    ** the macro INDEX_MAP, needs to adhere to the above ordering and the
    ** class to action mappings for the odd cases must immediately follow
    ** the even cases.
    */

    index = (((n >> (BITS_PER_WORD - 1)) & 2) | (n & 1)) + (n != 0);
    index = (index_map >> (EXPONENT_INDEX_FIELD_WIDTH*index)) & POWI_INDEX_MASK;

    fp_class = UNPACK(
        packed_argument,
        & unpacked_argument,
	POWI_CLASS_TO_ACTION_MAP + index,
        packed_result
        OPT_EXCEPTION_INFO_ARGUMENT );

    if (0 > fp_class)
        return;

    /* Initialize result to 1 */

    UX_SET_SIGN_EXP_MSD(&unpacked_result, 0, 1, UX_MSB);

    if (index <= (NEG_EXPONENT_INDEX + INDEX_INC))
        { /* For negative exponents use reciprocal of the argument */
        n = -n;
        DIVIDE(0, &unpacked_argument, FULL_PRECISION, &unpacked_argument);
        }

    while (1)
        {
        if (n & 1)
            {
            MULTIPLY(&unpacked_result, &unpacked_argument, &unpacked_result);
            NORMALIZE(&unpacked_result);
            }
        exponent = G_UX_EXPONENT(&unpacked_result) - UX_UNDERFLOW_EXPONENT;
        n = (U_WORD)(n >> 1);
        if (( 0 == n ) || (((unsigned) exponent) >
          (UX_OVERFLOW_EXPONENT - UX_UNDERFLOW_EXPONENT )))
            break;
        SQUARE(&unpacked_argument, &unpacked_argument);
        NORMALIZE(&unpacked_argument);
	}

    PACK(
         &unpacked_result,
         packed_result,
         G_UX_SIGN(&unpacked_result) ?
             INTPOWER_NEG_UNDERFLOW : INTPOWER_POS_UNDERFLOW,
         G_UX_SIGN(&unpacked_result) ?
             INTPOWER_NEG_OVERFLOW : INTPOWER_POS_OVERFLOW
        OPT_EXCEPTION_INFO_ARGUMENT );
    }


#undef  F_ENTRY_NAME
#define F_ENTRY_NAME	F_POW_I_NAME

X_XI_PROTO(F_ENTRY_NAME, packed_result, packed_base, n)
    {
    EXCEPTION_INFO_DECL
    DECLARE_X_FLOAT(packed_result)

    INIT_EXCEPTION_INFO;
    C_UX_POW_I(
        PASS_ARG_X_FLOAT(packed_base),
        n,
        INDEX_MAP(NEG_EXPONENT_INDEX,
                  ZERO_EXPONENT_RETURN_1_INDEX,
                  POS_EXPONENT_INDEX),
        PASS_RET_X_FLOAT(packed_result)
        OPT_EXCEPTION_INFO);

    RETURN_X_FLOAT(packed_result);

    }
    
#undef  F_ENTRY_NAME
#define F_ENTRY_NAME	F_POW_I_E_NAME

X_XI_PROTO(F_ENTRY_NAME, packed_result, packed_base, n)
    {
    EXCEPTION_INFO_DECL
    DECLARE_X_FLOAT(packed_result)

    INIT_EXCEPTION_INFO;
    C_UX_POW_I(
        PASS_ARG_X_FLOAT(packed_base),
        n,
        INDEX_MAP(NEG_EXPONENT_INDEX,
                  ZERO_EXPONENT_RETURN_ERROR_INDEX,
                  POS_EXPONENT_INDEX),
        PASS_RET_X_FLOAT(packed_result)
        OPT_EXCEPTION_INFO);

    RETURN_X_FLOAT(packed_result);

    }
    
    
#if defined(POW_Z)

#   undef  F_ENTRY_NAME
#   define F_ENTRY_NAME	F_POW_I_Z_NAME

    X_XI_PROTO(F_ENTRY_NAME, packed_result, packed_base, n)
        {
        EXCEPTION_INFO_DECL
        DECLARE_X_FLOAT(packed_result)

        INIT_EXCEPTION_INFO;
        C_UX_POW_I(
            PASS_ARG_X_FLOAT(packed_base),
            n,
            INDEX_MAP(NEG_EXPONENT_INDEX,
                      ZERO_EXPONENT_RETURN_0_INDEX,
                      POS_EXPONENT_INDEX),
            PASS_RET_X_FLOAT(packed_result)
            OPT_EXCEPTION_INFO);

        RETURN_X_FLOAT(packed_result);

        }
#endif
    


#if defined(MAKE_INCLUDE)

    @divert -append divertText

    precision = ceil(UX_PRECISION/8) + 4;

    START_TABLE;

    TABLE_COMMENT("powi class-to-action-mapping");
    PRINT_CLASS_TO_ACTION_TBL_DEF( "POWI_CLASS_TO_ACTION_MAP");

#   define PRINT_INDEX_DEF(name) 					\
			printf("#define " name "\t%i\n",		\
	 		(MP_BIT_OFFSET - base_offset)/BITS_PER_WORD )

    base_offset = MP_BIT_OFFSET;

    TABLE_COMMENT("... for n < 0, even and odd");
    PRINT_INDEX_DEF( "NEG_EXPONENT_INDEX\t\t" );
    PRINT_64_TBL_ITEM( CLASS_TO_ACTION_DISP(7) +
	      CLASS_TO_ACTION( F_C_SIG_NAN,    RETURN_QUIET_NAN, 0) +
              CLASS_TO_ACTION( F_C_QUIET_NAN,  RETURN_VALUE,     0) +
              CLASS_TO_ACTION( F_C_POS_INF,    RETURN_VALUE,     4) +
              CLASS_TO_ACTION( F_C_NEG_INF,    RETURN_VALUE,     4) +
              CLASS_TO_ACTION( F_C_POS_ZERO,   RETURN_ERROR,     2) +
              CLASS_TO_ACTION( F_C_NEG_ZERO ,  RETURN_ERROR,     2) );

    PRINT_64_TBL_ITEM( CLASS_TO_ACTION_DISP(6) +
	      CLASS_TO_ACTION( F_C_SIG_NAN,    RETURN_QUIET_NAN, 0) +
              CLASS_TO_ACTION( F_C_QUIET_NAN,  RETURN_VALUE,     0) +
              CLASS_TO_ACTION( F_C_POS_INF,    RETURN_VALUE,     4) +
              CLASS_TO_ACTION( F_C_NEG_INF,    RETURN_NEGATIVE,  4) +
              CLASS_TO_ACTION( F_C_POS_ZERO,   RETURN_ERROR,     2) +
              CLASS_TO_ACTION( F_C_NEG_ZERO ,  RETURN_ERROR,     3) );

    TABLE_COMMENT("... for n = 0, 0^0 = 0");
    PRINT_INDEX_DEF( "ZERO_EXPONENT_RETURN_0_INDEX\t" );
    PRINT_64_TBL_ITEM( CLASS_TO_ACTION_DISP(5) +
              CLASS_TO_ACTION( F_C_SIG_NAN,    RETURN_QUIET_NAN, 0) +
              CLASS_TO_ACTION( F_C_QUIET_NAN,  RETURN_VALUE,     0) +
              CLASS_TO_ACTION( F_C_POS_INF,    RETURN_VALUE,     5) +
              CLASS_TO_ACTION( F_C_NEG_INF,    RETURN_VALUE,     5) +
              CLASS_TO_ACTION( F_C_POS_DENORM, RETURN_VALUE,     5) +
              CLASS_TO_ACTION( F_C_NEG_DENORM, RETURN_VALUE,     5) +
              CLASS_TO_ACTION( F_C_POS_NORM,   RETURN_VALUE, 	 5) +
              CLASS_TO_ACTION( F_C_NEG_NORM,   RETURN_VALUE,     5) +
              CLASS_TO_ACTION( F_C_POS_ZERO,   RETURN_VALUE,     4) +
              CLASS_TO_ACTION( F_C_NEG_ZERO ,  RETURN_VALUE,     4) );

    TABLE_COMMENT("... for n = 0, 0^0 = 1");
    PRINT_INDEX_DEF( "ZERO_EXPONENT_RETURN_1_INDEX\t" );
    PRINT_64_TBL_ITEM( CLASS_TO_ACTION_DISP(4) +
              CLASS_TO_ACTION( F_C_SIG_NAN,    RETURN_QUIET_NAN, 0) +
              CLASS_TO_ACTION( F_C_QUIET_NAN,  RETURN_VALUE,     0) +
              CLASS_TO_ACTION( F_C_POS_INF,    RETURN_VALUE,     5) +
              CLASS_TO_ACTION( F_C_NEG_INF,    RETURN_VALUE,     5) +
              CLASS_TO_ACTION( F_C_POS_DENORM, RETURN_VALUE,     5) +
              CLASS_TO_ACTION( F_C_NEG_DENORM, RETURN_VALUE,     5) +
              CLASS_TO_ACTION( F_C_POS_NORM,   RETURN_VALUE, 	 5) +
              CLASS_TO_ACTION( F_C_NEG_NORM,   RETURN_VALUE,     5) +
              CLASS_TO_ACTION( F_C_POS_ZERO,   RETURN_VALUE,     5) +
              CLASS_TO_ACTION( F_C_NEG_ZERO ,  RETURN_VALUE,     5) );

    TABLE_COMMENT("... for n = 0, 0^0 = error");
    PRINT_INDEX_DEF( "ZERO_EXPONENT_RETURN_ERROR_INDEX" );
    PRINT_64_TBL_ITEM( CLASS_TO_ACTION_DISP(3) +
              CLASS_TO_ACTION( F_C_SIG_NAN,    RETURN_QUIET_NAN, 0) +
              CLASS_TO_ACTION( F_C_QUIET_NAN,  RETURN_VALUE,     0) +
              CLASS_TO_ACTION( F_C_POS_INF,    RETURN_VALUE,     5) +
              CLASS_TO_ACTION( F_C_NEG_INF,    RETURN_VALUE,     5) +
              CLASS_TO_ACTION( F_C_POS_DENORM, RETURN_VALUE,     5) +
              CLASS_TO_ACTION( F_C_NEG_DENORM, RETURN_VALUE,     5) +
              CLASS_TO_ACTION( F_C_POS_NORM,   RETURN_VALUE, 	 5) +
              CLASS_TO_ACTION( F_C_NEG_NORM,   RETURN_VALUE,     5) +
              CLASS_TO_ACTION( F_C_POS_ZERO,   RETURN_ERROR,     7) +
              CLASS_TO_ACTION( F_C_NEG_ZERO ,  RETURN_ERROR,     7) );

    TABLE_COMMENT("... for n > 0, even and odd");
    PRINT_INDEX_DEF( "POS_EXPONENT_INDEX\t\t" );
    PRINT_64_TBL_ITEM( CLASS_TO_ACTION_DISP(2) +
              CLASS_TO_ACTION( F_C_SIG_NAN,    RETURN_QUIET_NAN, 0) +
              CLASS_TO_ACTION( F_C_QUIET_NAN,  RETURN_VALUE,     0) +
              CLASS_TO_ACTION( F_C_POS_INF,    RETURN_VALUE,     0) +
              CLASS_TO_ACTION( F_C_NEG_INF,    RETURN_NEGATIVE,  0) +
              CLASS_TO_ACTION( F_C_POS_ZERO,   RETURN_VALUE,     0) +
              CLASS_TO_ACTION( F_C_NEG_ZERO ,  RETURN_NEGATIVE,  0) );

    PRINT_64_TBL_ITEM( CLASS_TO_ACTION_DISP(1) +
              CLASS_TO_ACTION( F_C_SIG_NAN,    RETURN_QUIET_NAN, 0) +
              CLASS_TO_ACTION( F_C_QUIET_NAN,  RETURN_VALUE,     0) +
              CLASS_TO_ACTION( F_C_POS_INF,    RETURN_VALUE,     0) +
              CLASS_TO_ACTION( F_C_NEG_INF,    RETURN_VALUE,     0) +
              CLASS_TO_ACTION( F_C_POS_ZERO,   RETURN_VALUE,     0) +
              CLASS_TO_ACTION( F_C_NEG_ZERO ,  RETURN_VALUE,     0) );

    printf("#define EXPONENT_INDEX_FIELD_WIDTH\t\t%i\n",
              bexp((MP_BIT_OFFSET - base_offset)/BITS_PER_WORD));

    TABLE_COMMENT("Data for the above mappings");
        PRINT_U_TBL_ITEM( /* data  1 */                     NULL );
        PRINT_U_TBL_ITEM( /* data  2 */ INTPOWER_POS_DIV_BY_ZERO );
        PRINT_U_TBL_ITEM( /* data  3 */ INTPOWER_NEG_DIV_BY_ZERO );
        PRINT_U_TBL_ITEM( /* data  4 */                     ZERO );
        PRINT_U_TBL_ITEM( /* data  5 */                      ONE );
        PRINT_U_TBL_ITEM( /* data  6 */                      INF );
        PRINT_U_TBL_ITEM( /* data  7 */    INTPOWER_ZERO_TO_ZERO );

    END_TABLE;

    @end_divert
    @eval my $tableText;						\
          my $outText    = MphocEval( GetStream( "divertText" ) );	\
          my $defineText = Egrep( "#define", $outText, \$tableText );	\
             $outText    = "$tableText\n\n$defineText";			\
          my $headerText = GetHeaderText( STR(BUILD_FILE_NAME),         \
                           "Definitions and constants floating base " .	\
                              "integer power routines", __FILE__ );	\
             print "$headerText\n\n$outText\n";

#endif
