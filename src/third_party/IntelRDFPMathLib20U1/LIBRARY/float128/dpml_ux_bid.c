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

#define BASE_NAME	bid

#include "dpml_ux.h"

#if !defined(MAKE_INCLUDE)
#   include STR(BUILD_FILE_NAME)
#endif

/*
** For the arithmetic operation (add, sub, mul and divide) it is sometines
** necessary to return an invalid operation result as well and overflow and
** underflow. For the time being, this will be done mapping the error ocdes
** for these operations onto existing error codes.
*/

#define MUL_ZERO_BY_INF	SQRT_OF_NEGATIVE

#define DIV_ZERO_BY_ZERO	SQRT_OF_NEGATIVE
#define DIV_BY_ZERO_POS	COT_OF_ZERO
#define DIV_BY_ZERO_NEG	LOG_OF_ZERO
#define DIV_INF_BY_INF	SQRT_OF_NEGATIVE

#define ADD_PINF_TO_NINF	SQRT_OF_NEGATIVE

#define SUB_INF_FROM_INF	SQRT_OF_NEGATIVE

/******************************************************************************/
/*
/* Basic arithmetic operations
/*
/******************************************************************************/

#undef  F_ENTRY_NAME
#define F_ENTRY_NAME	F_MUL_NAME

X_XX_PROTO(F_ENTRY_NAME, packed_result, packed_x, packed_y)
    {
    WORD    fp_class;
    UX_FLOAT unpacked_x, unpacked_y, unpacked_result;
    EXCEPTION_INFO_DECL
    DECLARE_X_FLOAT(packed_result)

    fp_class  = UNPACK2(
        PASS_ARG_X_FLOAT(packed_x),
        PASS_ARG_X_FLOAT(packed_y),
        & unpacked_x,
        & unpacked_y,
        MUL_CLASS_TO_ACTION_MAP,
        PASS_RET_X_FLOAT(packed_result)
        OPT_EXCEPTION_INFO );

    if (0 > fp_class)
       RETURN_X_FLOAT(packed_result);

    MULTIPLY( &unpacked_x, &unpacked_y, &unpacked_result);

    PACK(
        &unpacked_result,
        PASS_RET_X_FLOAT(packed_result),
        G_UX_SIGN(&unpacked_result) ? FMA_NEG_UNDERFLOW : FMA_POS_UNDERFLOW,
        G_UX_SIGN(&unpacked_result) ? FMA_NEG_OVERFLOW  : FMA_POS_OVERFLOW
        OPT_EXCEPTION_INFO );

    RETURN_X_FLOAT(packed_result);
    }


#undef  F_ENTRY_NAME
#define F_ENTRY_NAME	F_DIV_NAME

X_XX_PROTO(F_ENTRY_NAME, packed_result, packed_x, packed_y)
    {
    WORD    fp_class;
    UX_FLOAT unpacked_x, unpacked_y, unpacked_result;
    EXCEPTION_INFO_DECL
    DECLARE_X_FLOAT(packed_result)

    fp_class  = UNPACK2(
        PASS_ARG_X_FLOAT(packed_x),
        PASS_ARG_X_FLOAT(packed_y),
        & unpacked_x,
        & unpacked_y,
        DIV_CLASS_TO_ACTION_MAP,
        PASS_RET_X_FLOAT(packed_result)
        OPT_EXCEPTION_INFO );

    if (0 > fp_class)
       RETURN_X_FLOAT(packed_result);

    DIVIDE( &unpacked_x, &unpacked_y, 0, &unpacked_result);

    PACK(
        &unpacked_result,
        PASS_RET_X_FLOAT(packed_result),
        G_UX_SIGN(&unpacked_result) ? FMA_NEG_UNDERFLOW : FMA_POS_UNDERFLOW,
        G_UX_SIGN(&unpacked_result) ? FMA_NEG_OVERFLOW  : FMA_POS_OVERFLOW
        OPT_EXCEPTION_INFO );

    RETURN_X_FLOAT(packed_result);
    }


#undef  F_ENTRY_NAME
#define F_ENTRY_NAME	F_ADD_NAME

X_XX_PROTO(F_ENTRY_NAME, packed_result, packed_x, packed_y)
    {
    WORD    fp_class;
    UX_FLOAT unpacked_x, unpacked_y, unpacked_result;
    EXCEPTION_INFO_DECL
    DECLARE_X_FLOAT(packed_result)

    fp_class  = UNPACK2(
        PASS_ARG_X_FLOAT(packed_x),
        PASS_ARG_X_FLOAT(packed_y),
        & unpacked_x,
        & unpacked_y,
        ADDITION_CLASS_TO_ACTION_MAP,
        PASS_RET_X_FLOAT(packed_result)
        OPT_EXCEPTION_INFO );

    if (0 > fp_class)
       RETURN_X_FLOAT(packed_result);

    ADDSUB( &unpacked_x, &unpacked_y, ADD, &unpacked_result);

    PACK(
        &unpacked_result,
        PASS_RET_X_FLOAT(packed_result),
        G_UX_SIGN(&unpacked_result) ? FMA_NEG_UNDERFLOW : FMA_POS_UNDERFLOW,
        G_UX_SIGN(&unpacked_result) ? FMA_NEG_OVERFLOW  : FMA_POS_OVERFLOW
        OPT_EXCEPTION_INFO );

    RETURN_X_FLOAT(packed_result);
    }


#undef  F_ENTRY_NAME
#define F_ENTRY_NAME	F_SUB_NAME

X_XX_PROTO(F_ENTRY_NAME, packed_result, packed_x, packed_y)
    {
    WORD    fp_class;
    UX_FLOAT unpacked_x, unpacked_y, unpacked_result;
    EXCEPTION_INFO_DECL
    DECLARE_X_FLOAT(packed_result)

    fp_class  = UNPACK2(
        PASS_ARG_X_FLOAT(packed_x),
        PASS_ARG_X_FLOAT(packed_y),
        & unpacked_x,
        & unpacked_y,
        SUBTRACTION_CLASS_TO_ACTION_MAP,
        PASS_RET_X_FLOAT(packed_result)
        OPT_EXCEPTION_INFO );

    if (0 > fp_class)
       RETURN_X_FLOAT(packed_result);

    ADDSUB( &unpacked_x, &unpacked_y, SUB, &unpacked_result);

    PACK(
        &unpacked_result,
        PASS_RET_X_FLOAT(packed_result),
        G_UX_SIGN(&unpacked_result) ? FMA_NEG_UNDERFLOW : FMA_POS_UNDERFLOW,
        G_UX_SIGN(&unpacked_result) ? FMA_NEG_OVERFLOW  : FMA_POS_OVERFLOW
        OPT_EXCEPTION_INFO );

    RETURN_X_FLOAT(packed_result);
    }

#undef  F_ENTRY_NAME
#define F_ENTRY_NAME	F_NEG_NAME

X_X_PROTO(F_ENTRY_NAME, packed_result, packed_x)
    {
    WORD    fp_class;
    UX_FLOAT unpacked_x, unpacked_y, unpacked_result;
    EXCEPTION_INFO_DECL
    DECLARE_X_FLOAT(packed_result)

    fp_class  = UNPACK(
        PASS_ARG_X_FLOAT(packed_x),
        & unpacked_x,
        NEGATE_CLASS_TO_ACTION_MAP,
        PASS_RET_X_FLOAT(packed_result)
        OPT_EXCEPTION_INFO );

    RETURN_X_FLOAT(packed_result);
    }

#undef  F_ENTRY_NAME
#define F_ENTRY_NAME	F_FABS_NAME

X_X_PROTO(F_ENTRY_NAME, packed_result, packed_x)
    {
    WORD    fp_class;
    UX_FLOAT unpacked_x, unpacked_y, unpacked_result;
    EXCEPTION_INFO_DECL
    DECLARE_X_FLOAT(packed_result)

    fp_class  = UNPACK(
        PASS_ARG_X_FLOAT(packed_x),
        & unpacked_x,
        FABS_CLASS_TO_ACTION_MAP,
        PASS_RET_X_FLOAT(packed_result)
        OPT_EXCEPTION_INFO );

    RETURN_X_FLOAT(packed_result);
    }


#undef  F_ENTRY_NAME
#define F_ENTRY_NAME	F_ITOF_NAME

X_I_PROTO(F_ENTRY_NAME, packed_result, i)
    {
    UX_SIGN_TYPE           sign;
    UX_FRACTION_DIGIT_TYPE msd, mask;
    UX_EXPONENT_TYPE       exponent, cnt;
    UX_FLOAT               unpacked_result;
    DECLARE_X_FLOAT(packed_result)
    EXCEPTION_INFO_DECL

    #define ITOF_SHIFT	(BITS_PER_UX_FRACTION_DIGIT_TYPE - 32)

    sign  = 0;
    msd   = i;
    if ( i == 0 ) {
        exponent = 0;
    } else {
        exponent = 32;
        cnt      = 16;
        if ( (UX_SIGNED_FRACTION_DIGIT_TYPE) msd < 0 ) {
            msd = -msd;
            sign = 1;
        }

        mask = ((UX_FRACTION_DIGIT_TYPE) 0xffff0000) << ITOF_SHIFT;
        msd  = ((UX_FRACTION_DIGIT_TYPE) msd) << ITOF_SHIFT;

        while ( cnt ) {
            if ( (mask & msd) == 0 ) {
                msd <<= cnt;
                exponent -= cnt;
            }
            cnt >>= 1;
            mask <<= cnt;
        }
    }

    UX_SET_SIGN_EXP_MSD(&unpacked_result, sign, exponent, msd);

    PACK(
        &unpacked_result,
        PASS_RET_X_FLOAT(packed_result),
        /* Not Used */ 0,
        /* Not Used */ 0
        OPT_EXCEPTION_INFO );

    RETURN_X_FLOAT(packed_result);
    }


#define	LT	0
#define EQ	1
#define GT	2
#define UN	3
#define	DO	4

#define NUM_CMP_BITS	3

#define CPACK(a,b,c,d,e,f,g,h,i,j)		\
		( ((a) << (F_C_NEG_INF    * NUM_CMP_BITS)) | \
		  ((b) << (F_C_NEG_NORM   * NUM_CMP_BITS)) | \
		  ((c) << (F_C_NEG_DENORM * NUM_CMP_BITS)) | \
		  ((d) << (F_C_NEG_ZERO   * NUM_CMP_BITS)) | \
		  ((e) << (F_C_POS_ZERO   * NUM_CMP_BITS)) | \
		  ((f) << (F_C_POS_DENORM * NUM_CMP_BITS)) | \
		  ((g) << (F_C_POS_NORM   * NUM_CMP_BITS)) | \
		  ((h) << (F_C_POS_INF    * NUM_CMP_BITS)) | \
		  ((i) << (F_C_SIG_NAN    * NUM_CMP_BITS)) | \
		  ((j) << (F_C_QUIET_NAN  * NUM_CMP_BITS)) )

static U_INT_32 cmpTable[] = {
    /*                 -Inf -Nrm -Dnrm -Zero +Zero +Dnrm +Nrm +Inf SNaN QNaN) */
    /* -----------------------------------------------------------------------*/
    /* SNaN   */ CPACK( UN,  UN,   UN,   UN,   UN,   UN,  UN,  UN,  UN,  UN ),
    /* QNaN   */ CPACK( UN,  UN,   UN,   UN,   UN,   UN,  UN,  UN,  UN,  UN ),
    /* +Inf   */ CPACK( GT,  GT,   GT,   GT,   GT,   GT,  GT,  EQ,  UN,  UN ),
    /* -Inf   */ CPACK( EQ,  LT,   LT,   LT,   LT,   LT,  LT,  LT,  UN,  UN ),
    /* +Nrm   */ CPACK( GT,  GT,   GT,   GT,   GT,   GT,  DO,  LT,  UN,  UN ),
    /* -Nrm   */ CPACK( GT,  DO,   LT,   LT,   LT,   LT,  LT,  LT,  UN,  UN ),
    /* +Dnrm  */ CPACK( GT,  GT,   GT,   GT,   GT,   DO,  LT,  LT,  UN,  UN ),
    /* -Dnrm  */ CPACK( GT,  GT,   DO,   LT,   LT,   LT,  LT,  LT,  UN,  UN ),
    /* +Zero  */ CPACK( GT,  GT,   GT,   EQ,   EQ,   LT,  LT,  LT,  UN,  UN ),
    /* -Zero  */ CPACK( GT,  GT,   GT,   EQ,   EQ,   LT,  LT,  LT,  UN,  UN ),
    };


#if !defined(UX_CMP)
#   define UX_CMP       __INTERNAL_NAME(ux_cmp__)
#endif

static int
UX_CMP( WORD x_class, UX_FLOAT * unpacked_x,
        WORD y_class, UX_FLOAT * unpacked_y )
    {
    UX_SIGN_TYPE sign;
    int          i, order;
    WORD         diff; 

    order = (cmpTable[ x_class ] >> (NUM_CMP_BITS * y_class)) &
                  MAKE_MASK(NUM_CMP_BITS,0);

    if ( order == DO ) {

        // Both arguments have the same sign
        
        diff = ((WORD) G_UX_EXPONENT(unpacked_x)) -
                         ((WORD) G_UX_EXPONENT(unpacked_y));
        if (diff == 0) {
            for (i = 0; i < NUM_UX_FRACTION_DIGITS; i++) {
               diff = G_UX_FRACTION_DIGIT(unpacked_x, i) - 
                        G_UX_FRACTION_DIGIT(unpacked_y, i);
               if ( diff != 0 ) 
                   break;
             }
         }
         sign = G_UX_SIGN( unpacked_x );
         if ( diff > 0 ) {
             order = sign ? LT : GT;
         } else if ( diff < 0 )  {
             order = sign ? GT : LT;
         } else {
             order = EQ;
         }
    }
    return order;
}

#undef  F_ENTRY_NAME
#define F_ENTRY_NAME	F_CMP_NAME

I_XXI_PROTO(F_ENTRY_NAME, packed_x, packed_y, predicate)
    {
    WORD    fp_class, x_class, y_class;
    UX_FLOAT unpacked_x, unpacked_y, unpacked_result;
    int order;
    EXCEPTION_INFO_DECL
    _X_FLOAT dummy;

    fp_class  = UNPACK2(
        PASS_ARG_X_FLOAT(packed_x),
        PASS_ARG_X_FLOAT(packed_y),
        & unpacked_x,
        & unpacked_y,
        MUL_CLASS_TO_ACTION_MAP,
        &dummy
        OPT_EXCEPTION_INFO );

    #define CLASS_MASK		MAKE_MASK(F_C_CLASS_BIT_WIDTH,0);

    x_class = (fp_class >> F_C_CLASS_BIT_WIDTH) & CLASS_MASK;
    y_class = fp_class & CLASS_MASK;
    
    order =  UX_CMP(x_class, &unpacked_x, y_class, &unpacked_y);
    return (predicate >> order) & 1;
}


#undef  F_ENTRY_NAME
#define F_ENTRY_NAME	F_NEXTAFTER_NAME

X_XX_PROTO(F_ENTRY_NAME, packed_result, packed_x, packed_y)
    {
    WORD    fp_class, x_class, y_class, order;
    UX_FLOAT unpacked_x, unpacked_y;
    UX_EXPONENT_TYPE exponent;
    EXCEPTION_INFO_DECL
    DECLARE_X_FLOAT(packed_result)

    fp_class  = UNPACK2(
        PASS_ARG_X_FLOAT(packed_x),
        PASS_ARG_X_FLOAT(packed_y),
        & unpacked_x,
        & unpacked_y,
        NEXTAFTER_CLASS_TO_ACTION_MAP,
        PASS_RET_X_FLOAT(packed_result)
        OPT_EXCEPTION_INFO );

    if (0 > fp_class)
       RETURN_X_FLOAT(packed_result);

    x_class = (fp_class >> F_C_CLASS_BIT_WIDTH);
    y_class = fp_class & MAKE_MASK(F_C_CLASS_BIT_WIDTH,0);
    
    order =  UX_CMP(x_class, &unpacked_x, y_class, &unpacked_y);

    // Create (denormalized) increment value

    CLR_UX_LOW_FRACTION( &unpacked_y );
    P_UX_EXPONENT( &unpacked_y, G_UX_EXPONENT( &unpacked_x) );

    if (order != EQ) {
        
        exponent = G_UX_EXPONENT( &unpacked_x);
        
        UX_SET_SIGN_EXP_MSD( &unpacked_y, order == LT ? 0 : UX_SIGN_BIT,
                            exponent, 0);
        CLR_UX_LOW_FRACTION( &unpacked_y );
        P_UX_LSD( &unpacked_y, 1 << 15 );
        ADDSUB( &unpacked_x, &unpacked_y, ADD, &unpacked_x);
    }

    PACK(
        &unpacked_x,
        PASS_RET_X_FLOAT(packed_result),
        G_UX_SIGN(&unpacked_x) ? FMA_NEG_UNDERFLOW : FMA_POS_UNDERFLOW,
        G_UX_SIGN(&unpacked_x) ? FMA_NEG_OVERFLOW  : FMA_POS_OVERFLOW
        OPT_EXCEPTION_INFO );

    RETURN_X_FLOAT(packed_result);
    }

#if defined(MAKE_INCLUDE)

    @divert -append divertText

#   undef TABLE_NAME

    START_TABLE;

    TABLE_COMMENT("Negate class-to-action-mapping");
    PRINT_CLASS_TO_ACTION_TBL_DEF( "NEGATE_CLASS_TO_ACTION_MAP");

    PRINT_64_TBL_ITEM( CLASS_TO_ACTION_DISP(0) + 
	      CLASS_TO_ACTION( F_C_NEG_INF,    RETURN_NEGATIVE,  0) +
	      CLASS_TO_ACTION( F_C_NEG_NORM,   RETURN_NEGATIVE,  0) +
	      CLASS_TO_ACTION( F_C_NEG_DENORM, RETURN_NEGATIVE,  0) +
	      CLASS_TO_ACTION( F_C_NEG_ZERO,   RETURN_NEGATIVE,  0) +
	      CLASS_TO_ACTION( F_C_POS_ZERO,   RETURN_NEGATIVE,  0) +
	      CLASS_TO_ACTION( F_C_POS_DENORM, RETURN_NEGATIVE,  0) +
	      CLASS_TO_ACTION( F_C_POS_NORM,   RETURN_NEGATIVE,  0) +
	      CLASS_TO_ACTION( F_C_POS_INF,    RETURN_NEGATIVE,  0) +
	      CLASS_TO_ACTION( F_C_SIG_NAN,    RETURN_NEGATIVE,  0) +
	      CLASS_TO_ACTION( F_C_QUIET_NAN,  RETURN_NEGATIVE,  0));

    TABLE_COMMENT("Fabs class-to-action-mapping");
    PRINT_CLASS_TO_ACTION_TBL_DEF( "FABS_CLASS_TO_ACTION_MAP");

    PRINT_64_TBL_ITEM( CLASS_TO_ACTION_DISP(0) + 
	      CLASS_TO_ACTION( F_C_NEG_INF,    RETURN_NEGATIVE,  0) +
	      CLASS_TO_ACTION( F_C_NEG_NORM,   RETURN_NEGATIVE,  0) +
	      CLASS_TO_ACTION( F_C_NEG_DENORM, RETURN_NEGATIVE,  0) +
	      CLASS_TO_ACTION( F_C_NEG_ZERO,   RETURN_NEGATIVE,  0) +
	      CLASS_TO_ACTION( F_C_POS_ZERO,   RETURN_VALUE,     0) +
	      CLASS_TO_ACTION( F_C_POS_DENORM, RETURN_VALUE,     0) +
	      CLASS_TO_ACTION( F_C_POS_NORM,   RETURN_VALUE,     0) +
	      CLASS_TO_ACTION( F_C_POS_INF,    RETURN_VALUE,     0) +
	      CLASS_TO_ACTION( F_C_SIG_NAN,    RETURN_VALUE,     0) +
	      CLASS_TO_ACTION( F_C_QUIET_NAN,  RETURN_VALUE,     0));


    TABLE_COMMENT("Nextafter class-to-action-mapping");
    PRINT_CLASS_TO_ACTION_TBL_DEF( "NEXTAFTER_CLASS_TO_ACTION_MAP");

	  /* Index 0: mapping for x */

    PRINT_64_TBL_ITEM( CLASS_TO_ACTION_DISP(0) + 
	      CLASS_TO_ACTION( F_C_SIG_NAN,    RETURN_QUIET_NAN, 0) +
	      CLASS_TO_ACTION( F_C_QUIET_NAN,  RETURN_VALUE,     0) );

	  /* Index 1: class-to-index mapping */

    PRINT_64_TBL_ITEM( 
	      CLASS_TO_INDEX( F_C_POS_ZERO,   2) +
	      CLASS_TO_INDEX( F_C_NEG_ZERO,   2) +
	      CLASS_TO_INDEX( F_C_POS_DENORM, 2) +
	      CLASS_TO_INDEX( F_C_NEG_DENORM, 2) +
	      CLASS_TO_INDEX( F_C_POS_NORM,   2) +
	      CLASS_TO_INDEX( F_C_NEG_NORM,   2) +
	      CLASS_TO_INDEX( F_C_POS_INF,    2) +
	      CLASS_TO_INDEX( F_C_NEG_INF,    2) );

	  /* Index 2: y class-to-index mapping for x != SNaN or QNaN */

    PRINT_64_TBL_ITEM( CLASS_TO_ACTION_DISP(0) + 
	      CLASS_TO_ACTION( F_C_SIG_NAN,    RETURN_QUIET_NAN, 1) +
	      CLASS_TO_ACTION( F_C_QUIET_NAN,  RETURN_VALUE,     1) );

    TABLE_COMMENT("Multiply class-to-action-mapping");
    PRINT_CLASS_TO_ACTION_TBL_DEF( "MUL_CLASS_TO_ACTION_MAP");

	  /* Index 0: mapping for x */

    PRINT_64_TBL_ITEM( CLASS_TO_ACTION_DISP(5) + 
	      CLASS_TO_ACTION( F_C_SIG_NAN,    RETURN_QUIET_NAN, 0) +
	      CLASS_TO_ACTION( F_C_QUIET_NAN,  RETURN_VALUE,     0) );

	  /* Index 1: class-to-index mapping */

    PRINT_64_TBL_ITEM( 
	      CLASS_TO_INDEX( F_C_POS_ZERO,   2) +
	      CLASS_TO_INDEX( F_C_NEG_ZERO,   2) +
	      CLASS_TO_INDEX( F_C_NEG_DENORM, 3) +
	      CLASS_TO_INDEX( F_C_NEG_NORM,   3) +
	      CLASS_TO_INDEX( F_C_POS_DENORM, 4) +
	      CLASS_TO_INDEX( F_C_POS_NORM,   4) +
	      CLASS_TO_INDEX( F_C_POS_INF,    5) +
	      CLASS_TO_INDEX( F_C_NEG_INF,    5) );

	  /* Index 2: y class-to-index mapping for x = +/- zero */

    PRINT_64_TBL_ITEM( CLASS_TO_ACTION_DISP(3) + 
	      CLASS_TO_ACTION( F_C_NEG_INF,    RETURN_ERROR,     2) +
	      CLASS_TO_ACTION( F_C_NEG_NORM,   RETURN_NEGATIVE,  0) +
	      CLASS_TO_ACTION( F_C_NEG_DENORM, RETURN_NEGATIVE,  0) +
	      CLASS_TO_ACTION( F_C_NEG_ZERO,   RETURN_NEGATIVE,  0) +
	      CLASS_TO_ACTION( F_C_POS_ZERO,   RETURN_VALUE,     0) +
	      CLASS_TO_ACTION( F_C_POS_DENORM, RETURN_VALUE,     0) +
	      CLASS_TO_ACTION( F_C_POS_NORM,   RETURN_VALUE,     0) +
	      CLASS_TO_ACTION( F_C_POS_INF,    RETURN_ERROR,     2) +
	      CLASS_TO_ACTION( F_C_SIG_NAN,    RETURN_QUIET_NAN, 1) +
	      CLASS_TO_ACTION( F_C_QUIET_NAN,  RETURN_VALUE,     1));

	  /* Index 3: y class-to-index mapping for x = -norm or -denorm */

    PRINT_64_TBL_ITEM( CLASS_TO_ACTION_DISP(2) + 
	      CLASS_TO_ACTION( F_C_NEG_INF,    RETURN_NEGATIVE,  1) +
	      CLASS_TO_ACTION( F_C_NEG_NORM,   RETURN_UNPACKED,  1) +
	      CLASS_TO_ACTION( F_C_NEG_DENORM, RETURN_UNPACKED,  1) +
	      CLASS_TO_ACTION( F_C_NEG_ZERO,   RETURN_NEGATIVE,  1) +
	      CLASS_TO_ACTION( F_C_POS_ZERO,   RETURN_NEGATIVE,  1) +
	      CLASS_TO_ACTION( F_C_POS_DENORM, RETURN_UNPACKED,  1) +
	      CLASS_TO_ACTION( F_C_POS_NORM,   RETURN_UNPACKED,  1) +
	      CLASS_TO_ACTION( F_C_POS_INF,    RETURN_NEGATIVE,  1) +
	      CLASS_TO_ACTION( F_C_SIG_NAN,    RETURN_QUIET_NAN, 1) +
	      CLASS_TO_ACTION( F_C_QUIET_NAN,  RETURN_VALUE,     1));

	  /* Index 4: y class-to-index mapping for x = +norm or +denorm */

    PRINT_64_TBL_ITEM( CLASS_TO_ACTION_DISP(1) + 
	      CLASS_TO_ACTION( F_C_NEG_INF,    RETURN_VALUE,     1) +
	      CLASS_TO_ACTION( F_C_NEG_NORM,   RETURN_UNPACKED,  1) +
	      CLASS_TO_ACTION( F_C_NEG_DENORM, RETURN_UNPACKED,  1) +
	      CLASS_TO_ACTION( F_C_NEG_ZERO,   RETURN_VALUE,     1) +
	      CLASS_TO_ACTION( F_C_POS_ZERO,   RETURN_VALUE,     1) +
	      CLASS_TO_ACTION( F_C_POS_DENORM, RETURN_UNPACKED,  1) +
	      CLASS_TO_ACTION( F_C_POS_NORM,   RETURN_UNPACKED,  1) +
	      CLASS_TO_ACTION( F_C_POS_INF,    RETURN_VALUE,     1) +
	      CLASS_TO_ACTION( F_C_SIG_NAN,    RETURN_QUIET_NAN, 1) +
	      CLASS_TO_ACTION( F_C_QUIET_NAN,  RETURN_VALUE,     1));

	  /* Index 5: y class-to-index mapping for x = +/-Inf */

    PRINT_64_TBL_ITEM( CLASS_TO_ACTION_DISP(0) + 
	      CLASS_TO_ACTION( F_C_NEG_INF,    RETURN_NEGATIVE,  0) +
	      CLASS_TO_ACTION( F_C_NEG_NORM,   RETURN_NEGATIVE,  0) +
	      CLASS_TO_ACTION( F_C_NEG_DENORM, RETURN_NEGATIVE,  0) +
	      CLASS_TO_ACTION( F_C_NEG_ZERO,   RETURN_ERROR,     2) +
	      CLASS_TO_ACTION( F_C_POS_ZERO,   RETURN_ERROR,     2) +
	      CLASS_TO_ACTION( F_C_POS_DENORM, RETURN_VALUE,     0) +
	      CLASS_TO_ACTION( F_C_POS_NORM,   RETURN_VALUE,     0) +
	      CLASS_TO_ACTION( F_C_POS_INF,    RETURN_VALUE,     0) +
	      CLASS_TO_ACTION( F_C_SIG_NAN,    RETURN_QUIET_NAN, 1) +
	      CLASS_TO_ACTION( F_C_QUIET_NAN,  RETURN_VALUE,     1));

              TABLE_COMMENT("Data for the above mapping");
                  PRINT_U_TBL_ITEM( /* data 2 */ MUL_ZERO_BY_INF  );

    TABLE_COMMENT("Divide class-to-action-mapping");
    PRINT_CLASS_TO_ACTION_TBL_DEF( "DIV_CLASS_TO_ACTION_MAP");

	  /* Index 0: mapping for x */

    PRINT_64_TBL_ITEM( CLASS_TO_ACTION_DISP(5) + 
	      CLASS_TO_ACTION( F_C_SIG_NAN,    RETURN_QUIET_NAN, 0) +
	      CLASS_TO_ACTION( F_C_QUIET_NAN,  RETURN_VALUE,     0) );

	  /* Index 1: class-to-index mapping */

    PRINT_64_TBL_ITEM( 
	      CLASS_TO_INDEX( F_C_POS_ZERO,   2) +
	      CLASS_TO_INDEX( F_C_NEG_ZERO,   2) +
	      CLASS_TO_INDEX( F_C_NEG_DENORM, 3) +
	      CLASS_TO_INDEX( F_C_NEG_NORM,   3) +
	      CLASS_TO_INDEX( F_C_POS_DENORM, 4) +
	      CLASS_TO_INDEX( F_C_POS_NORM,   4) +
	      CLASS_TO_INDEX( F_C_POS_INF,    5) +
	      CLASS_TO_INDEX( F_C_NEG_INF,    5) );

	  /* Index 2: y class-to-index mapping for x = +/- zero */

    PRINT_64_TBL_ITEM( CLASS_TO_ACTION_DISP(3) + 
	      CLASS_TO_ACTION( F_C_NEG_INF,    RETURN_NEGATIVE,  0) +
	      CLASS_TO_ACTION( F_C_NEG_NORM,   RETURN_NEGATIVE,  0) +
	      CLASS_TO_ACTION( F_C_NEG_DENORM, RETURN_NEGATIVE,  0) +
	      CLASS_TO_ACTION( F_C_NEG_ZERO,   RETURN_ERROR,     3) +
	      CLASS_TO_ACTION( F_C_POS_ZERO,   RETURN_ERROR,     3) +
	      CLASS_TO_ACTION( F_C_POS_DENORM, RETURN_VALUE,     0) +
	      CLASS_TO_ACTION( F_C_POS_NORM,   RETURN_VALUE,     0) +
	      CLASS_TO_ACTION( F_C_POS_INF,    RETURN_VALUE,     0) +
	      CLASS_TO_ACTION( F_C_SIG_NAN,    RETURN_QUIET_NAN, 1) +
	      CLASS_TO_ACTION( F_C_QUIET_NAN,  RETURN_VALUE,     1));

	  /* Index 3: y class-to-index mapping for x = -norm or -denorm */

    PRINT_64_TBL_ITEM( CLASS_TO_ACTION_DISP(2) + 
	      CLASS_TO_ACTION( F_C_NEG_INF,    RETURN_VALUE,     2) +
	      CLASS_TO_ACTION( F_C_NEG_NORM,   RETURN_UNPACKED,  1) +
	      CLASS_TO_ACTION( F_C_NEG_DENORM, RETURN_UNPACKED,  1) +
	      CLASS_TO_ACTION( F_C_NEG_ZERO,   RETURN_ERROR,     4) +
	      CLASS_TO_ACTION( F_C_POS_ZERO,   RETURN_ERROR,     5) +
	      CLASS_TO_ACTION( F_C_POS_DENORM, RETURN_UNPACKED,  1) +
	      CLASS_TO_ACTION( F_C_POS_NORM,   RETURN_UNPACKED,  1) +
	      CLASS_TO_ACTION( F_C_POS_INF,    RETURN_NEGATIVE,  2) +
	      CLASS_TO_ACTION( F_C_SIG_NAN,    RETURN_QUIET_NAN, 1) +
	      CLASS_TO_ACTION( F_C_QUIET_NAN,  RETURN_VALUE,     1));

	  /* Index 4: y class-to-index mapping for x = +norm or +denorm */

    PRINT_64_TBL_ITEM( CLASS_TO_ACTION_DISP(1) + 
	      CLASS_TO_ACTION( F_C_NEG_INF,    RETURN_NEGATIVE,  2) +
	      CLASS_TO_ACTION( F_C_NEG_NORM,   RETURN_UNPACKED,  1) +
	      CLASS_TO_ACTION( F_C_NEG_DENORM, RETURN_UNPACKED,  1) +
	      CLASS_TO_ACTION( F_C_NEG_ZERO,   RETURN_ERROR,     5) +
	      CLASS_TO_ACTION( F_C_POS_ZERO,   RETURN_ERROR,     4) +
	      CLASS_TO_ACTION( F_C_POS_DENORM, RETURN_UNPACKED,  1) +
	      CLASS_TO_ACTION( F_C_POS_NORM,   RETURN_UNPACKED,  1) +
	      CLASS_TO_ACTION( F_C_POS_INF,    RETURN_VALUE,     2) +
	      CLASS_TO_ACTION( F_C_SIG_NAN,    RETURN_QUIET_NAN, 1) +
	      CLASS_TO_ACTION( F_C_QUIET_NAN,  RETURN_VALUE,     1));

	  /* Index 5: y class-to-index mapping for x = +/-Inf */

    PRINT_64_TBL_ITEM( CLASS_TO_ACTION_DISP(0) + 
	      CLASS_TO_ACTION( F_C_NEG_INF,    RETURN_ERROR,     6) +
	      CLASS_TO_ACTION( F_C_NEG_NORM,   RETURN_NEGATIVE,  0) +
	      CLASS_TO_ACTION( F_C_NEG_DENORM, RETURN_NEGATIVE,  0) +
	      CLASS_TO_ACTION( F_C_NEG_ZERO,   RETURN_NEGATIVE,  0) +
	      CLASS_TO_ACTION( F_C_POS_ZERO,   RETURN_VALUE,     0) +
	      CLASS_TO_ACTION( F_C_POS_DENORM, RETURN_VALUE,     0) +
	      CLASS_TO_ACTION( F_C_POS_NORM,   RETURN_VALUE,     0) +
	      CLASS_TO_ACTION( F_C_POS_INF,    RETURN_ERROR,     6) +
	      CLASS_TO_ACTION( F_C_SIG_NAN,    RETURN_QUIET_NAN, 1) +
	      CLASS_TO_ACTION( F_C_QUIET_NAN,  RETURN_VALUE,     1));

              TABLE_COMMENT("Data for the above mapping");
                  PRINT_U_TBL_ITEM( /* data 2 */             ZERO );
                  PRINT_U_TBL_ITEM( /* data 3 */ DIV_ZERO_BY_ZERO );
                  PRINT_U_TBL_ITEM( /* data 4 */  DIV_BY_ZERO_POS );
                  PRINT_U_TBL_ITEM( /* data 5 */  DIV_BY_ZERO_NEG );
                  PRINT_U_TBL_ITEM( /* data 6 */  DIV_INF_BY_INF  );

    TABLE_COMMENT("Addition class-to-action-mapping");
    PRINT_CLASS_TO_ACTION_TBL_DEF( "ADDITION_CLASS_TO_ACTION_MAP");

	  /* Index 0: mapping for x */

    PRINT_64_TBL_ITEM( CLASS_TO_ACTION_DISP(5) + 
	      CLASS_TO_ACTION( F_C_SIG_NAN,    RETURN_QUIET_NAN, 0) +
	      CLASS_TO_ACTION( F_C_QUIET_NAN,  RETURN_VALUE,     0) );

	  /* Index 1: class-to-index mapping */

    PRINT_64_TBL_ITEM( 
	      CLASS_TO_INDEX( F_C_POS_ZERO,   2) +
	      CLASS_TO_INDEX( F_C_NEG_ZERO,   2) +
	      CLASS_TO_INDEX( F_C_NEG_DENORM, 3) +
	      CLASS_TO_INDEX( F_C_NEG_NORM,   3) +
	      CLASS_TO_INDEX( F_C_POS_DENORM, 3) +
	      CLASS_TO_INDEX( F_C_POS_NORM,   3) +
	      CLASS_TO_INDEX( F_C_NEG_INF,    4) + 
	      CLASS_TO_INDEX( F_C_POS_INF,    5) );

	  /* Index 2: y class-to-index mapping for x = +/- zero */

    PRINT_64_TBL_ITEM( CLASS_TO_ACTION_DISP(3) + 
	      CLASS_TO_ACTION( F_C_NEG_INF,    RETURN_VALUE,     1) +
	      CLASS_TO_ACTION( F_C_NEG_NORM,   RETURN_VALUE,     1) +
	      CLASS_TO_ACTION( F_C_NEG_DENORM, RETURN_VALUE,     1) +
	      CLASS_TO_ACTION( F_C_NEG_ZERO,   RETURN_VALUE,     0) +
	      CLASS_TO_ACTION( F_C_POS_ZERO,   RETURN_VALUE,     1) +
	      CLASS_TO_ACTION( F_C_POS_DENORM, RETURN_VALUE,     1) +
	      CLASS_TO_ACTION( F_C_POS_NORM,   RETURN_VALUE,     1) +
	      CLASS_TO_ACTION( F_C_POS_INF,    RETURN_VALUE,     1) +
	      CLASS_TO_ACTION( F_C_SIG_NAN,    RETURN_QUIET_NAN, 1) +
	      CLASS_TO_ACTION( F_C_QUIET_NAN,  RETURN_VALUE,     1));

	  /* Index 3: y class-to-index mapping for x = +/-norm or +/-denorm */

    PRINT_64_TBL_ITEM( CLASS_TO_ACTION_DISP(2) + 
	      CLASS_TO_ACTION( F_C_NEG_INF,    RETURN_VALUE,     1) +
	      CLASS_TO_ACTION( F_C_NEG_NORM,   RETURN_UNPACKED,  1) +
	      CLASS_TO_ACTION( F_C_NEG_DENORM, RETURN_UNPACKED,  1) +
	      CLASS_TO_ACTION( F_C_NEG_ZERO,   RETURN_VALUE,     0) +
	      CLASS_TO_ACTION( F_C_POS_ZERO,   RETURN_VALUE,     0) +
	      CLASS_TO_ACTION( F_C_POS_DENORM, RETURN_UNPACKED,  1) +
	      CLASS_TO_ACTION( F_C_POS_NORM,   RETURN_UNPACKED,  1) +
	      CLASS_TO_ACTION( F_C_POS_INF,    RETURN_VALUE,     1) +
	      CLASS_TO_ACTION( F_C_SIG_NAN,    RETURN_QUIET_NAN, 1) +
	      CLASS_TO_ACTION( F_C_QUIET_NAN,  RETURN_VALUE,     1));

	  /* Index 4: y class-to-index mapping for x = -Inf */

    PRINT_64_TBL_ITEM( CLASS_TO_ACTION_DISP(1) + 
	      CLASS_TO_ACTION( F_C_NEG_INF,    RETURN_VALUE,     1) +
	      CLASS_TO_ACTION( F_C_NEG_NORM,   RETURN_VALUE,     0) +
	      CLASS_TO_ACTION( F_C_NEG_DENORM, RETURN_VALUE,     0) +
	      CLASS_TO_ACTION( F_C_NEG_ZERO,   RETURN_VALUE,     0) +
	      CLASS_TO_ACTION( F_C_POS_ZERO,   RETURN_VALUE,     0) +
	      CLASS_TO_ACTION( F_C_POS_DENORM, RETURN_VALUE,     0) +
	      CLASS_TO_ACTION( F_C_POS_NORM,   RETURN_VALUE,     0) +
	      CLASS_TO_ACTION( F_C_POS_INF,    RETURN_ERROR,     2) +
	      CLASS_TO_ACTION( F_C_SIG_NAN,    RETURN_QUIET_NAN, 1) +
	      CLASS_TO_ACTION( F_C_QUIET_NAN,  RETURN_VALUE,     1));

	  /* Index 5: y class-to-index mapping for x = +Inf */

    PRINT_64_TBL_ITEM( CLASS_TO_ACTION_DISP(0) + 
	      CLASS_TO_ACTION( F_C_NEG_INF,    RETURN_ERROR,     2) +
	      CLASS_TO_ACTION( F_C_NEG_NORM,   RETURN_VALUE,     0) +
	      CLASS_TO_ACTION( F_C_NEG_DENORM, RETURN_VALUE,     0) +
	      CLASS_TO_ACTION( F_C_NEG_ZERO,   RETURN_VALUE,     0) +
	      CLASS_TO_ACTION( F_C_POS_ZERO,   RETURN_VALUE,     0) +
	      CLASS_TO_ACTION( F_C_POS_DENORM, RETURN_VALUE,     0) +
	      CLASS_TO_ACTION( F_C_POS_NORM,   RETURN_VALUE,     0) +
	      CLASS_TO_ACTION( F_C_POS_INF,    RETURN_VALUE,     0) +
	      CLASS_TO_ACTION( F_C_SIG_NAN,    RETURN_QUIET_NAN, 1) +
	      CLASS_TO_ACTION( F_C_QUIET_NAN,  RETURN_VALUE,     1));

              TABLE_COMMENT("Data for the above mapping");
                  PRINT_U_TBL_ITEM( /* data 2 */  ADD_PINF_TO_NINF );

    TABLE_COMMENT("Subtraction class-to-action-mapping");
    PRINT_CLASS_TO_ACTION_TBL_DEF( "SUBTRACTION_CLASS_TO_ACTION_MAP");

	  /* Index 0: mapping for x */

    PRINT_64_TBL_ITEM( CLASS_TO_ACTION_DISP(5) + 
	      CLASS_TO_ACTION( F_C_SIG_NAN,    RETURN_QUIET_NAN, 0) +
	      CLASS_TO_ACTION( F_C_QUIET_NAN,  RETURN_VALUE,     0) );

	  /* Index 1: class-to-index mapping */

    PRINT_64_TBL_ITEM( 
	      CLASS_TO_INDEX( F_C_POS_ZERO,   2) +
	      CLASS_TO_INDEX( F_C_NEG_ZERO ,  2) +
	      CLASS_TO_INDEX( F_C_NEG_DENORM, 3) +
	      CLASS_TO_INDEX( F_C_NEG_NORM,   3) +
	      CLASS_TO_INDEX( F_C_POS_DENORM, 3) +
	      CLASS_TO_INDEX( F_C_POS_NORM,   3) +
	      CLASS_TO_INDEX( F_C_NEG_INF,    4) +
	      CLASS_TO_INDEX( F_C_POS_INF,    5) );

	  /* Index 2: y class-to-index mapping for x = +/- zero */

    PRINT_64_TBL_ITEM( CLASS_TO_ACTION_DISP(3) + 
	      CLASS_TO_ACTION( F_C_NEG_INF,    RETURN_NEGATIVE,  1) +
	      CLASS_TO_ACTION( F_C_NEG_NORM,   RETURN_NEGATIVE,  1) +
	      CLASS_TO_ACTION( F_C_NEG_DENORM, RETURN_NEGATIVE,  1) +
	      CLASS_TO_ACTION( F_C_NEG_ZERO ,  RETURN_NEGATIVE,  1) +
	      CLASS_TO_ACTION( F_C_POS_ZERO,   RETURN_VALUE,     0) +
	      CLASS_TO_ACTION( F_C_POS_DENORM, RETURN_NEGATIVE,  1) +
	      CLASS_TO_ACTION( F_C_POS_NORM,   RETURN_NEGATIVE,  1) +
	      CLASS_TO_ACTION( F_C_POS_INF,    RETURN_NEGATIVE,  1) +
	      CLASS_TO_ACTION( F_C_SIG_NAN,    RETURN_QUIET_NAN, 1) +
	      CLASS_TO_ACTION( F_C_QUIET_NAN,  RETURN_VALUE,     1));

	  /* Index 3: y class-to-index mapping for x = +/-norm or +/-denorm */

    PRINT_64_TBL_ITEM( CLASS_TO_ACTION_DISP(2) + 
	      CLASS_TO_ACTION( F_C_NEG_INF,    RETURN_NEGATIVE,  1) +
	      CLASS_TO_ACTION( F_C_NEG_NORM,   RETURN_UNPACKED,  1) +
	      CLASS_TO_ACTION( F_C_NEG_DENORM, RETURN_UNPACKED,  1) +
	      CLASS_TO_ACTION( F_C_NEG_ZERO ,  RETURN_VALUE,     0) +
	      CLASS_TO_ACTION( F_C_POS_ZERO,   RETURN_VALUE,     0) +
	      CLASS_TO_ACTION( F_C_POS_DENORM, RETURN_UNPACKED,  1) +
	      CLASS_TO_ACTION( F_C_POS_NORM,   RETURN_UNPACKED,  1) +
	      CLASS_TO_ACTION( F_C_POS_INF,    RETURN_NEGATIVE,  1) +
	      CLASS_TO_ACTION( F_C_SIG_NAN,    RETURN_QUIET_NAN, 1) +
	      CLASS_TO_ACTION( F_C_QUIET_NAN,  RETURN_VALUE,     1));

	  /* Index 4: y class-to-index mapping for x = -Inf */

    PRINT_64_TBL_ITEM( CLASS_TO_ACTION_DISP(1) + 
	      CLASS_TO_ACTION( F_C_NEG_INF,    RETURN_ERROR,     2) +
	      CLASS_TO_ACTION( F_C_NEG_NORM,   RETURN_VALUE,     0) +
	      CLASS_TO_ACTION( F_C_NEG_DENORM, RETURN_VALUE,     0) +
	      CLASS_TO_ACTION( F_C_NEG_ZERO ,  RETURN_VALUE,     0) +
	      CLASS_TO_ACTION( F_C_POS_ZERO,   RETURN_VALUE,     0) +
	      CLASS_TO_ACTION( F_C_POS_DENORM, RETURN_VALUE,     0) +
	      CLASS_TO_ACTION( F_C_POS_NORM,   RETURN_VALUE,     0) +
	      CLASS_TO_ACTION( F_C_POS_INF,    RETURN_VALUE,     0) +
	      CLASS_TO_ACTION( F_C_SIG_NAN,    RETURN_QUIET_NAN, 1) +
	      CLASS_TO_ACTION( F_C_QUIET_NAN,  RETURN_VALUE,     1));

	  /* Index 5: y class-to-index mapping for x = +Inf */

    PRINT_64_TBL_ITEM( CLASS_TO_ACTION_DISP(0) + 
	      CLASS_TO_ACTION( F_C_NEG_INF,    RETURN_VALUE,     0) +
	      CLASS_TO_ACTION( F_C_NEG_NORM,   RETURN_VALUE,     0) +
	      CLASS_TO_ACTION( F_C_NEG_DENORM, RETURN_VALUE,     0) +
	      CLASS_TO_ACTION( F_C_NEG_ZERO ,  RETURN_VALUE,     0) +
	      CLASS_TO_ACTION( F_C_POS_ZERO,   RETURN_VALUE,     0) +
	      CLASS_TO_ACTION( F_C_POS_DENORM, RETURN_VALUE,     0) +
	      CLASS_TO_ACTION( F_C_POS_NORM,   RETURN_VALUE,     0) +
	      CLASS_TO_ACTION( F_C_POS_INF,    RETURN_ERROR,     2) +
	      CLASS_TO_ACTION( F_C_SIG_NAN,    RETURN_QUIET_NAN, 1) +
	      CLASS_TO_ACTION( F_C_QUIET_NAN,  RETURN_VALUE,     1));

              TABLE_COMMENT("Data for the above mapping");
                  PRINT_U_TBL_ITEM( /* data 2 */  SUB_INF_FROM_INF );


    PAD_IF_NEEDED(MP_BIT_OFFSET, 64);

    /* Print various powers of 2 */

    TABLE_COMMENT("2^n, n = .5, 0, 24, 75, -24, -77 in double precision");

    PRINT_R_TBL_VDEF_ITEM( "D_SQRT_TWO\t",               sqrt(2));
    PRINT_R_TBL_VDEF_ITEM( "D_ONE\t\t",                        1);
    PRINT_R_TBL_VDEF_ITEM( "D_TWO_POW_24\t",     bldexp(1,   24));
    PRINT_R_TBL_VDEF_ITEM( "D_TWO_POW_75\t",     bldexp(1,   75));
    PRINT_R_TBL_VDEF_ITEM( "D_RECIP_TWO_POW_24", bldexp(1,  -24));
    PRINT_R_TBL_VDEF_ITEM( "D_RECIP_TWO_POW_77", bldexp(1,  -77));

    TABLE_COMMENT(
       "Rsqrt iteration (double precision) constants: 7/8 and 3/8");

    PRINT_R_TBL_VDEF_ITEM( "D_SEVEN_EIGHTS", 7/8);
    PRINT_R_TBL_VDEF_ITEM( "D_THREE_EIGHTS", 3/8);

    TABLE_COMMENT("3 in unpacked format");
    PRINT_UX_TBL_ADEF_ITEM( "UX_THREE\t\t",             3);

    END_TABLE;

    @end_divert
    @eval my $tableText;						\
          my $outText    = MphocEval( GetStream( "divertText" ) );	\
          my $defineText = Egrep( "#define", $outText, \$tableText );	\
             $outText    = "$tableText\n\n$defineText";			\
          my $headerText = GetHeaderText( STR(BUILD_FILE_NAME),         \
                           "Definitions and constants square root " .	\
                              "related routines", __FILE__ );		\
             print "$headerText\n\n$outText\n";
#endif

