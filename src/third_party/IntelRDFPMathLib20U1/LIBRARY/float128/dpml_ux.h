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

#if !defined(DPML_UX_H)
#define DPML_UX_H

#if !defined(X_FLOAT)
#   define X_FLOAT
#endif
#define NEW_DPML_MACROS			1
#define DPML_PROTOTYPES_H

#include "dpml_private.h"

/* Alignment macros for 16-byte floating point types _Quad and _Decimal128. */
#if defined(_WIN32)||defined(_WIN64)
    #define F128_ALIGN_16 __declspec(align(16))
#else
#if !defined(HPUX_OS)
    #define F128_ALIGN_16 __attribute__((aligned(16)))
#else
    #define F128_ALIGN_16 
#endif
#endif

/*
** Types:
**
** Define the basic data types that are used by the unpacked x_float routines
** as well as macros to access their fields and define specific values
*/

typedef INT_32   UX_SIGN_TYPE;
typedef INT_32   UX_EXPONENT_TYPE;
typedef U_INT_32 UX_UNSIGNED_EXPONENT_TYPE;
typedef U_WORD   UX_FRACTION_DIGIT_TYPE;
typedef WORD     UX_SIGNED_FRACTION_DIGIT_TYPE;

#define BITS_PER_UX_SIGN_TYPE		32
#define BITS_PER_UX_EXPONENT_TYPE	32
#define BITS_PER_UX_FRACTION_DIGIT_TYPE	BITS_PER_WORD

#define	NUM_UX_FRACTION_DIGITS	(128/BITS_PER_UX_FRACTION_DIGIT_TYPE)
#define	NUM_X_FRACTION_DIGITS	(128/BITS_PER_UX_FRACTION_DIGIT_TYPE)

#if (VAX_FLOATING) || (ENDIANESS == big_endian)
#    define DIGIT(n)	digit[n]
#else
#    define DIGIT(n)	digit[NUM_UX_FRACTION_DIGITS - 1 - (n)]
#endif

typedef struct F128_ALIGN_16 {
	UX_FRACTION_DIGIT_TYPE digit[ NUM_X_FRACTION_DIGITS ];
	} _X_FLOAT;

#define G_X_DIGIT(p,n)		(((_X_FLOAT *)(p))->DIGIT(n))
#define P_X_DIGIT(p,n,v)	(((_X_FLOAT *)(p))->DIGIT(n) = (v))
#define X_TOGGLE_SIGN(p,v)	(((_X_FLOAT *)(p))->DIGIT(0) ^= (v))

#define SHIFT			F_EXP_WIDTH
#define CSHIFT			(BITS_PER_UX_FRACTION_DIGIT_TYPE - F_EXP_WIDTH)


typedef struct {
	UX_SIGN_TYPE        sign;
	UX_EXPONENT_TYPE    exponent;
	UX_FRACTION_DIGIT_TYPE fraction[ NUM_UX_FRACTION_DIGITS ];
	} UX_FLOAT;

typedef struct {
	UX_FRACTION_DIGIT_TYPE digits[ NUM_UX_FRACTION_DIGITS ];
	} FIXED_128;

#define UX_SIGN_SHIFT  (BITS_PER_UX_FRACTION_DIGIT_TYPE - BITS_PER_UX_SIGN_TYPE)
#define UX_PRECISION	128 

#define	LSD_NUM			(NUM_UX_FRACTION_DIGITS - 1)
#define	MSD_NUM			0

#define	G_UX_SIGN(x)		   (((UX_FLOAT*)(x))->sign)
#define	G_UX_EXPONENT(x)	   (((UX_FLOAT*)(x))->exponent)
#define	G_UX_MSD(x)		   (((UX_FLOAT*)(x))->fraction[0])
#define	G_UX_2nd_MSD(x)		   (((UX_FLOAT*)(x))->fraction[1])
#define	G_UX_LSD(x)		   (((UX_FLOAT*)(x))->fraction[LSD_NUM])
#define	G_UX_2nd_LSD(x)		   (((UX_FLOAT*)(x))->fraction[LSD_NUM-1])
#define G_UX_FRACTION_DIGIT(x,n)   (((UX_FLOAT*)(x))->fraction[n])

#define	P_UX_SIGN(x,v)		   ((((UX_FLOAT*)(x))->sign)=(v))
#define	P_UX_EXPONENT(x,v)	   ((((UX_FLOAT*)(x))->exponent)=(v))
#define	P_UX_MSD(x,v)		   ((((UX_FLOAT*)(x))->fraction[0])=(v))
#define	P_UX_2nd_MSD(x,v)	   ((((UX_FLOAT*)(x))->fraction[1])=(v))
#define	P_UX_LSD(x,v)		   ((((UX_FLOAT*)(x))->fraction[LSD_NUM])=(v))
#define	P_UX_2nd_LSD(x,v)	   ((((UX_FLOAT*)(x))->fraction[LSD_NUM-1])=(v))
#define P_UX_FRACTION_DIGIT(x,n,v) (((UX_FLOAT*)(x))->fraction[n] = (v))

#define UX_INCR_EXPONENT(x,v)	   ((((UX_FLOAT *)(x))->exponent) += (v))
#define UX_DECR_EXPONENT(x,v)	   ((((UX_FLOAT *)(x))->exponent) -= (v))
#define UX_TOGGLE_SIGN(x,v)	   ((((UX_FLOAT *)(x))->sign) ^= (v))

#define UX_SIGN_BIT	      ((WORD) 1 << 31)
#define UX_MSB		      ((U_WORD)1 <<(BITS_PER_UX_FRACTION_DIGIT_TYPE-1))
#define UX_OVERFLOW_EXPONENT  (1 << F_EXP_WIDTH)
#define UX_UNDERFLOW_EXPONENT (- UX_OVERFLOW_EXPONENT)
#define UX_ZERO_EXPONENT      (- (UX_EXPONENT_TYPE) 1 << (F_EXP_WIDTH + 2))
#define UX_INFINITY_EXPONENT  (-(UX_ZERO_EXPONENT + 1)) 


#define AS_DIGIT(p,n)	(((UX_FRACTION_DIGIT_TYPE *)(p))[n])

#include "dpml_ux_32_64.h"

#define UX_LOW_FRACTION_IS_ZERO(p)	(UX_OR_LOW_FRACTION_DIGITS(p) == 0)
#define UX_FRACTION_IS_ONE_HALF(p)	((G_UX_MSD(p) == UX_MSB) & \
					(UX_OR_LOW_FRACTION_DIGITS(p) == 0))

#define UX_SET_SIGN_EXP_MSD(p,s,e,m)	( P_UX_SIGN(p,s),	\
					  P_UX_EXPONENT(p,e),	\
					  P_UX_MSD(p,m),	\
					  CLR_UX_LOW_FRACTION(p))

#define UX_COPY(p,q)	( P_UX_SIGN(q, G_UX_SIGN(p)),	      \
			  P_UX_EXPONENT(q, G_UX_EXPONENT(p)), \
			  COPY_TO_UX_FRACTION(&G_UX_MSD(p),q))

typedef U_WORD	ERROR_CODE;

/******************************************************************************/
/******************************************************************************/
/**                                                                          **/
/**                 Name Macros                                              **/
/**                                                                          **/
/******************************************************************************/
/******************************************************************************/

/* 
** Following macros are defined to modify the interface of X_FLOAT routines
** for different architectures variants at compile time.  The macros are
** defined as returnType_Arg1Arg2_PROTO. For example X_X_PROTO defines a 
** function which takes X_FLOAT argument and result is X_FLOAT argument. 
**
** X_FLOAT_RES_OR_VOID defines what functions is returning. It can be void,
** X_FLOAT or X_FLOAT *. 
** 
** X_FLOAT_RET_TYPE(x) defines the return type when result is part of the 
** argument list i.e a pointer is provided in the argument list to 
** put the result. It can be Nothing or X_FLOAT *x.    
** 
** X_FLOAT_ARG_TYPE(x) defines  the argument type. It can be X_FLOAT *x, or
** X_FLOAT x. 
** 
** X_FLOAT_INT_TYPE defines the integer type in the argument list. This should
** be int in case of intel compilers. 
**  
** RETURN_X_FLOAT(x) defines the return statement of the function. It can be
** be Nothing, return *x or return x
*/

#if defined(EMT64_LINUX_QUAD_INTERFACE)
#   define X_FLOAT_RET_TYPE(x)
#   define X_FLOAT_ARG_TYPE(x)         _Quad x
#   define X_FLOAT_INT_TYPE            int
#   define X_FLOAT_RES_OR_VOID         _Quad
#   define DECLARE_X_FLOAT(res)        _X_FLOAT res;
#   define PASS_RET_X_FLOAT(x)         &x
//#   define PASS_ARG_X_FLOAT(x)         &x
#   define PASS_ARG_X_FLOAT(x)         (_X_FLOAT *) &x
#   define RETURN_X_FLOAT(x)           return *(_Quad *) &(x)
#   define PACKED_ARG_IS_NEG(p)        ((WORD)((_X_FLOAT *)(&p))->DIGIT(0) < 0)

#elif defined(X_NONVOID_RES_VAL_ARG_VAL)
#   define X_FLOAT_RET_TYPE(x)          
#   define X_FLOAT_ARG_TYPE(x)         _X_FLOAT x 
#   define X_FLOAT_INT_TYPE            int
#   define X_FLOAT_RES_OR_VOID         _X_FLOAT
#   define DECLARE_X_FLOAT(res)        _X_FLOAT res; 
#   define PASS_RET_X_FLOAT(x)             &x
#   define PASS_ARG_X_FLOAT(x)             &x
#   define RETURN_X_FLOAT(x)           return x; 
#   define PACKED_ARG_IS_NEG(p)	       ((WORD)((_X_FLOAT *)(&p))->DIGIT(0) < 0)

#elif defined(X_VOID_RES_REF_ARG_VAL)
#   define X_FLOAT_RET_TYPE(x)         _X_FLOAT *x,
#   define X_FLOAT_ARG_TYPE(x)         _X_FLOAT x 
#   define X_FLOAT_INT_TYPE            int
#   define X_FLOAT_RES_OR_VOID         void
#   define DECLARE_X_FLOAT(res)   
#   define PASS_RET_X_FLOAT(x)          x
#   define PASS_ARG_X_FLOAT(x)          &x
#   define RETURN_X_FLOAT(x)            return; 
#   define PACKED_ARG_IS_NEG(p)	       ((WORD)((_X_FLOAT *)(&p))->DIGIT(0) < 0)

#else
#   define X_FLOAT_RET_TYPE(x)         _X_FLOAT *x,
#   define X_FLOAT_ARG_TYPE(x)         _X_FLOAT *x 
#   define X_FLOAT_INT_TYPE            WORD
#   define X_FLOAT_RES_OR_VOID         void
#   define DECLARE_X_FLOAT(res)   
#   define PASS_RET_X_FLOAT(x)         x
#   define PASS_ARG_X_FLOAT(x)         x
#   define RETURN_X_FLOAT(x)           return; 
#   define PACKED_ARG_IS_NEG(p)	       ((WORD)((_X_FLOAT *)(p))->DIGIT(0) < 0)
#endif

#if !defined(X_FLOAT_INT_TYPE)
#   define X_FLOAT_INT_TYPE            WORD
#endif


#   define X_I_PROTO(name,res,arg)                                          \
            X_FLOAT_RES_OR_VOID name(X_FLOAT_RET_TYPE(res) int arg)

#   define X_X_PROTO(name,res,arg)                                          \
            X_FLOAT_RES_OR_VOID name(X_FLOAT_RET_TYPE(res)                 \
               X_FLOAT_ARG_TYPE(arg)) \

#   define X_XX_PROTO(name, res, arg1, arg2)                                \
            X_FLOAT_RES_OR_VOID name(X_FLOAT_RET_TYPE(res)                 \
               X_FLOAT_ARG_TYPE(arg1), X_FLOAT_ARG_TYPE(arg2))              \

#   define X_XI_PROTO(name, res, arg,i)                                     \
           X_FLOAT_RES_OR_VOID name(X_FLOAT_RET_TYPE(res)                  \
               X_FLOAT_ARG_TYPE(arg), X_FLOAT_INT_TYPE i)                   \

#   define X_IX_PROTO(name, res, i, arg)                                    \
           X_FLOAT_RES_OR_VOID name(X_FLOAT_RET_TYPE(res)                  \
           X_FLOAT_INT_TYPE i, X_FLOAT_ARG_TYPE(arg))                      \

#   define X_XIptr_PROTO(name, res, arg,i)                                  \
           X_FLOAT_RES_OR_VOID name(X_FLOAT_RET_TYPE(res)                  \
               X_FLOAT_ARG_TYPE(arg), X_FLOAT_INT_TYPE *i)                  \

#   define X_XXptr_PROTO(name, res, arg, p)                                 \
           X_FLOAT_RES_OR_VOID name(X_FLOAT_RET_TYPE(res)                   \
               X_FLOAT_ARG_TYPE(arg), _X_FLOAT * p)                         \

#   define X_XXIptr_PROTO(name, res, arg1, arg2, i)                         \
           X_FLOAT_RES_OR_VOID name(X_FLOAT_RET_TYPE(res)                  \
               X_FLOAT_ARG_TYPE(arg1), X_FLOAT_ARG_TYPE(arg2),              \
               X_FLOAT_INT_TYPE *i)                                         \

#   define I_XXI_PROTO(name, arg1, arg2, i)                                  \
           int name( X_FLOAT_ARG_TYPE(arg1), X_FLOAT_ARG_TYPE(arg2), int i)

#   define RR_X_PROTO(name, res1, res2, arg)                                \
           void name(X_FLOAT_ARG_TYPE(arg), _X_FLOAT *res1, _X_FLOAT *res2)


/******************************************************************************/
/******************************************************************************/
/**                                                                          **/
/**                 Packed and Unpacked Constant Tables                      **/
/**                                                                          **/
/******************************************************************************/
/******************************************************************************/

#if !defined(PACKED_CONSTANT_TABLE)
#    define PACKED_CONSTANT_TABLE	__TABLE_NAME(x_constants__ )
#endif

#undef NAN

#if !defined(DPML_UX_CONS_FILE_NAME)
#   define DPML_UX_CONS_FILE_NAME	dpml_cons_x.h
#endif

#if !defined(BUILD_UX_CONS_TABLE)
#   define INSTANTIATE_TABLE	0
#   define INSTANTIATE_DEFINES	1
#   include STR(DPML_UX_CONS_FILE_NAME)
#endif

/******************************************************************************/
/******************************************************************************/
/**                                                                          **/
/**                      Pack and Unpacked Routines                          **/
/**                                                                          **/
/******************************************************************************/
/******************************************************************************/

/*
** There's a slight complication here because the interface to these routines
** depend on the interface to the exception handler.  Specifically, if we
** need to pass the original arguments and/or the name of the function to the
** exception handler, that information must be passed to pack and unpack.
*/

typedef struct {
	U_WORD     arg_classes;
	char     * name;
	_X_FLOAT * args[2];
	} UX_EXCEPTION_INFO_STRUCT;

#define EXCPTN_INFO			__INTERNAL_NAME(ux_excptn_info__)

#define EXCEPTION_INFO_DECL		UX_EXCEPTION_INFO_STRUCT EXCEPTION_INFO;
#define OPT_EXCEPTION_INFO		, &EXCEPTION_INFO
#define OPT_EXCEPTION_INFO_DECLARATION  , UX_EXCEPTION_INFO_STRUCT * EXCPTN_INFO
#define OPT_EXCEPTION_INFO_ARGUMENT	, EXCPTN_INFO
#define IF_OPTNL_ERROR_INFO(x)		x



#if (EXCEPTION_INTERFACE_SEND & send_function_name )
#   define INIT_EXCEPTION_INFO		EXCPTN_INFO.name = STR(F_ENTRY_NAME)
#else
#   define INIT_EXCEPTION_INFO
#endif

#define UNPACK(a,b,c,d)		UNPACK_X_OR_Y(a,0,b,c,d)

#if !defined( UNPACK_X_OR_Y )
#   define UNPACK_X_OR_Y	__INTERNAL_NAME( unpack_x_or_y__ )
#endif

#if !defined( UNPACK2 )
#   define UNPACK2		__INTERNAL_NAME( unpack2__ )
#endif

#if !defined( PACK )
#   define PACK		__INTERNAL_NAME( pack__ )
#endif

extern WORD UNPACK_X_OR_Y (
	_X_FLOAT     *,		/* packed   argument 1 */
	_X_FLOAT     *,		/* packed   argument 2 */
	UX_FLOAT     *,		/* unpacked argument   */
	U_WORD const *,		/* class-to-action map */
	_X_FLOAT     *		/* packed   result */
        OPT_EXCEPTION_INFO_DECLARATION
	);

extern WORD UNPACK2 (
	_X_FLOAT     *,		/* packed   argument 1 */
	_X_FLOAT     *,		/* packed   argument 2 */
	UX_FLOAT     *,		/* unpacked argument 1 */
	UX_FLOAT     *,		/* unpacked argument 2 */
	U_WORD const *,		/* class-to-action map */
	_X_FLOAT     *		/* packed   result */
        OPT_EXCEPTION_INFO_DECLARATION
	);


extern void PACK (
	UX_FLOAT  *,		/* unpacked result */
	_X_FLOAT  *,		/* packed   result */
	ERROR_CODE,		/* underflow code  */
	ERROR_CODE		/* overflow  code  */
        OPT_EXCEPTION_INFO_DECLARATION
	);

/*
** Include the class-to-action-mapping definitions here, since they are used
** primarily by the unpack routines.
*/

#define INDEX_POS	0
#define INDEX_WIDTH	3
#define INDEX_MASK	0x7

#define ACTION_POS	3
#define ACTION_WIDTH	3
#define ACTION_MASK	0x7

#define CLASS_TO_ACTION(class, action, index) \
		(((action << INDEX_WIDTH) | (index)) << \
		((INDEX_WIDTH + ACTION_WIDTH)*(class)))
#define CLASS_TO_ACTION_DISP(n) \
		((n) << ((INDEX_WIDTH + ACTION_WIDTH)*F_C_NUM_CLASSES))

#define RETURN_UNPACKED		0
#define RETURN_QUIET_NAN	1
#define RETURN_VALUE		2
#define RETURN_NEGATIVE		3
#define RETURN_ABSOLUTE		4
#define RETURN_CPYSN_ARG_0	5
#define RETURN_ERROR		7

#define CLASS_TO_INDEX_WIDTH		4
#define CLASS_TO_INDEX(n,m)		((m) << ((n)*CLASS_TO_INDEX_WIDTH))
#define CLASS_TO_INDEX_MASK		MAKE_MASK(CLASS_TO_INDEX_WIDTH, 0)
#define WORDS_PER_CLASS_TO_ACTION_MAP	(64/BITS_PER_WORD)

/******************************************************************************/
/******************************************************************************/
/**                                                                          **/
/**                        Rational Evaluation Routine                       **/
/**                                                                          **/
/******************************************************************************/
/******************************************************************************/

#if !defined( EVALUATE_RATIONAL )
#   define EVALUATE_RATIONAL	__INTERNAL_NAME( evaluate_rational__ )
#endif

extern void EVALUATE_RATIONAL(
	UX_FLOAT  *,		/* Argument				*/
	FIXED_128 *,		/* Coefficient array			*/
	U_WORD,			/* Number of coefficients		*/
	U_WORD,			/* Evaluation flags			*/
	UX_FLOAT  *		/* Result				*/
	);

#define STANDARD		0x001
#define POST_MULTIPLY		0x002
#define SQUARE_TERM		0x004
#define ALTERNATE_SIGN		0x008

#define NUM_DEN_FIELD_WIDTH	4

#define NO_DIVIDE		((WORD)  1 << (2*NUM_DEN_FIELD_WIDTH))
#define SWAP			((WORD)  2 << (2*NUM_DEN_FIELD_WIDTH))
#define SKIP			((WORD)  4 << (2*NUM_DEN_FIELD_WIDTH))

#define	SCALE_WIDTH		6
#define SCALE_POS		(BITS_PER_WORD - SCALE_WIDTH)
#define P_SCALE(n)		(((WORD) (n)) << SCALE_POS)
#define G_SCALE(n)		(((WORD) (n)) >> SCALE_POS)

#define POLY_SHIFT(u,n)		((((UX_FLOAT *)(u))->exponent)*(n))
#define NUMERATOR_FLAGS(n)	(n)
#define DENOMINATOR_FLAGS(n)	((n) << NUM_DEN_FIELD_WIDTH)

#if !defined(EVALUATE_PACKED_POLY)
#   define EVALUATE_PACKED_POLY __INTERNAL_NAME(evaluate_packed_poly__)
#endif

void
EVALUATE_PACKED_POLY( UX_FLOAT * argument, WORD degree, FIXED_128 * coefs,
  U_WORD mask, WORD bias, UX_FLOAT * result);

/******************************************************************************/
/******************************************************************************/
/**                                                                          **/
/**                        Rational Evaluation Routine                       **/
/**                                                                          **/
/******************************************************************************/
/******************************************************************************/

#if !defined( ADDSUB )
#   define ADDSUB	__INTERNAL_NAME( addsub__ )
#endif

extern void ADDSUB(
	UX_FLOAT *,	/* arg1			*/
	UX_FLOAT *,	/* arg2			*/
	U_WORD,		/* operation flags	*/
	UX_FLOAT *	/* result		*/
	);

/*
** The logic of the add/sub routine depends on theses symbols have
** these *SPECIFIC* values.  !!! DO NOT CHANGE THEM !!!
*/

#define	ADD			0
#define	SUB			1
#define	ADD_SUB			2
#define	SUB_ADD			3
#define	MAGNITUDE_ONLY		4
#define	NO_NORMALIZATION	8


/******************************************************************************/
/******************************************************************************/
/**                                                                          **/
/**                        Round to Integer Routine                          **/
/**                                                                          **/
/******************************************************************************/
/******************************************************************************/

#if !defined(UX_RND_TO_INT)
#   define UX_RND_TO_INT	__INTERNAL_NAME(ux_rnd_to_int__)
#endif

extern WORD UX_RND_TO_INT(	/* return val is integer part as int	*/
	UX_FLOAT *,		/* argument				*/
	WORD,			/* rounding mode bit vector		*/
        UX_FLOAT *,		/* Integer part as float, ignored if 0	*/
	UX_FLOAT *); 		/* fraction part, ignored if 0		*/

#define RZ_BIT_VECTOR   0x0000  /* 0000 0000 0000 0000 */
#define RP_BIT_VECTOR   0x00fa  /* 0000 0000 1111 1010 */
#define RM_BIT_VECTOR   0xfa00  /* 1111 1010 0000 0000 */
#define RN_BIT_VECTOR   0xa8a8  /* 1010 1000 1010 1000 */
#define RV_BIT_VECTOR   0xaaaa  /* 1010 1010 1010 1010 */

#define INTEGER_RESULT	0x10000
#define FRACTION_RESULT	0x20000

/******************************************************************************/
/******************************************************************************/
/**                                                                          **/
/**                        Normalization Routines                            **/
/**                                                                          **/
/******************************************************************************/
/******************************************************************************/

#if !defined(FFS_AND_SHIFT)
#   define FFS_AND_SHIFT		__INTERNAL_NAME(ffs_and_shift__)
#endif

extern WORD FFS_AND_SHIFT(	/* returns shift count		*/
	UX_FLOAT *,		/* source and destination	*/
	U_WORD);		/* 'opcode'			*/

#define	FFS_NORMALIZE	0
#define	FFS_CVT_WORD	1
#define	FFS_CVT_U_WORD	2

#define	NORMALIZE(x)	   FFS_AND_SHIFT(x, FFS_NORMALIZE)
#define	WORD_TO_UX(n,x)	   (P_UX_MSD(x, n), FFS_AND_SHIFT(x, FFS_CVT_WORD))
#define	U_WORD_TO_UX(n,x)  (P_UX_MSD(x, n), FFS_AND_SHIFT(x, FFS_CVT_U_WORD))

/******************************************************************************/
/******************************************************************************/
/**                                                                          **/
/**                         Radian Trig Prototypes                           **/
/**                                                                          **/
/******************************************************************************/
/******************************************************************************/

#if !defined(UX_SINCOS)
#    define UX_SINCOS		__INTERNAL_NAME(ux_sincos)
#endif

extern WORD UX_SINCOS(
	UX_FLOAT *,	/* unpacked_argument	*/
	WORD,		/* octant		*/
	WORD,		/* function_code,	*/
	UX_FLOAT *);	/* unpacked_result	*/

#define	DEGREE		16

#define	SIN_FUNC	1
#define	COS_FUNC	2
#define	SINCOS_FUNC	(SIN_FUNC | COS_FUNC)

#define	SIND_FUNC	(SIN_FUNC | DEGREE)
#define	COSD_FUNC	(COS_FUNC | DEGREE)
#define	SINCOSD_FUNC	(SINCOS_FUNC | DEGREE)

#define	TAN_FUNC	4
#define	COT_FUNC	8

#define	TAND_FUNC	(TAN_FUNC | DEGREE)
#define	COTD_FUNC	(COT_FUNC | DEGREE)

#define	SIN(a,b)	EVAL_SINCOS(a, 0, SIN_FUNC, b)
#define	COS(a,b)	EVAL_SINCOS(a, 0, COS_FUNC, b)
#define	SINCOS(a,b)	EVAL_SINCOS(a, 0, SINCOS_FUNC, b)

#define SINCOS_COEF_ARRAY_LENGTH        12
extern FIXED_128 sincos_coef_array[2*SINCOS_COEF_ARRAY_LENGTH];

/******************************************************************************/
/******************************************************************************/
/**                                                                          **/
/**                              Log Prototypes                              **/
/**                                                                          **/
/******************************************************************************/
/******************************************************************************/

#if !defined(UX_LOG)
#   define UX_LOG	__INTERNAL_NAME( ux_log__ )
#endif

extern void UX_LOG(
	UX_FLOAT *,	/* Argument				*/ 
	UX_FLOAT *,	/* scale - LOG(x) = scale*log2(x)	*/
	UX_FLOAT *);	/* Result				*/

#define	LOG(a,b)		UX_LOG( a, & UX_CON( LN_2 ), b)


#if !defined(UX_LOG_POLY)
#   define UX_LOG_POLY	__INTERNAL_NAME( ux_log_poly__ )
#endif

extern void UX_LOG_POLY(
	UX_FLOAT *,	/* Argument				*/ 
	UX_FLOAT *);	/* Result				*/

/******************************************************************************/
/******************************************************************************/
/**                                                                          **/
/**                        Miscellaneous Prototypes                         **/
/**                                                                          **/
/******************************************************************************/
/******************************************************************************/

#if !defined( EXP )
#   define UX_EXP	__INTERNAL_NAME( ux_exp__ )
#endif

extern void UX_EXP(
	UX_FLOAT *,			/* argument	*/
	UX_FLOAT *			/* result	*/
	);

#if !defined( DIVIDE )
#   define DIVIDE	__INTERNAL_NAME( divide__ )
#endif

extern void DIVIDE(
	UX_FLOAT *,	/* numerator - assume 1	if ptr is 0	*/
	UX_FLOAT *,	/* denominator				*/
	U_WORD,		/* result precision			*/
	UX_FLOAT *	/* result				*/
	);

#define	HALF_PRECISION		1
#define	FULL_PRECISION		2

#if !defined( MULTIPLY )
#   define MULTIPLY	__INTERNAL_NAME( multiply__ )
#endif

extern void MULTIPLY(
	UX_FLOAT *,	/* arg1		*/
	UX_FLOAT *,	/* arg1		*/
	UX_FLOAT *	/* result	*/
	);

#define SQUARE(a,b)	MULTIPLY(a, a, b)

#if !defined( EXTENDED_MULTIPLY )
#   define EXTENDED_MULTIPLY	__INTERNAL_NAME( extended_multiply__ )
#endif

extern void EXTENDED_MULTIPLY(
	UX_FLOAT *,	/* arg1		*/
	UX_FLOAT *,	/* arg1		*/
	UX_FLOAT *,	/* hi result	*/
	UX_FLOAT *	/* lo result	*/
	);

#if !defined(UX_SQRT_EVALUATION)
#   define UX_SQRT_EVALUATION	__INTERNAL_NAME( ux_sqrt_evaluation__ )
#endif

#define EVALUATE_SQRT		0
#define EVALUATE_RSQRT		1


extern void UX_SQRT_EVALUATION(
	UX_FLOAT *,	/* Argument				*/ 
        WORD,		/* evaluation type - sqrt or rsqrt	*/
	UX_FLOAT *);	/* Result				*/


#define UX_SQRT(a,b)	UX_SQRT_EVALUATION(a, EVALUATE_SQRT, b)

#if !defined(HYPOT)
#   define HYPOT	__INTERNAL_NAME( hypot__ )
#endif

extern void HYPOT(
	UX_FLOAT *,	/* Argument 1				*/ 
	UX_FLOAT *,	/* Argument 2				*/ 
	UX_FLOAT *);	/* Result				*/


/******************************************************************************/
/******************************************************************************/
/**                                                                          **/
/**                        Miscellaneous Definitions                         **/
/**                                                                          **/
/******************************************************************************/
/******************************************************************************/

#define	NONE			0
#define	NOT_USED		0

#if defined(NULL)
#   undef  NULL
#endif
#define	NULL		0

#if defined GROUP
#   define D_GROUP(x)	GROUP(x)
#else
#   define D_GROUP_NAME		PASTE_2(__INTERNAL_NAME(group),_d)
    extern double D_GROUP_NAME( double );
#   define D_GROUP(x)	D_GROUP_NAME(x)
#endif


/******************************************************************************/
/******************************************************************************/
/**                                                                          **/
/**             MPHOC Macros for Class-to-Action Table Definitions           **/
/**                                                                          **/
/******************************************************************************/
/******************************************************************************/

#define	POS	0
#define NEG	UX_SIGN_BIT

#if defined(MAKE_INCLUDE)

#    define PRINT_64_TBL_ITEM(i)	\
		printf( "\t/* %3i */ %#16.4.16i,\n", BYTES(MP_BIT_OFFSET), i);\
                MP_BIT_OFFSET += 64

#    define PRINT_UX_FRACTION_DIGIT_TBL_ITEM(val)	PRINT_64_TBL_ITEM(val)

#    define PRINT_CLASS_TO_ACTION_TBL_DEF(name)		\
		printf("#define\t" name "\t((U_WORD const *) ((char *) "\
		        STR(MP_TABLE_NAME) " + %i))\n",	BYTES(MP_BIT_OFFSET))

#    define PRINT_UX_FRACTION_DIGIT_TBL_VDEF(name)	\
                 printf("#define\t" name \
		     "\t*((UX_FRACTION_DIGIT_TYPE *) ((char *) " \
		     STR(MP_TABLE_NAME) " + %i))\n", BYTES(MP_BIT_OFFSET))

#    define PRINT_UX_FRACTION_DIGIT_TBL_VDEF_ITEM(name, val)	\
		printf("#define\t" name \
		     "\t*((UX_FRACTION_DIGIT_TYPE *) ((char *) " \
		     STR(MP_TABLE_NAME) " + %i))\n", BYTES(MP_BIT_OFFSET)); \
		PRINT_64_TBL_ITEM(val)

#    define PRINT_FIXED_128_TBL_ADEF(name)	\
		printf("#define\t" name "\t((FIXED_128 *) ((char *) "	\
		     STR(MP_TABLE_NAME) " + %i))\n", BYTES(MP_BIT_OFFSET))

#    define PRINT_UX_FRACTION_DIGIT_TBL_ADEF(name)					\
		printf("#define\t" name "\t((UX_FRACTION_DIGIT_TYPE *) ((char *) "	 \
		   STR(MP_TABLE_NAME) " + %i))\n", BYTES(MP_BIT_OFFSET))

#    define PRINT_UX_TBL_ADEF(name)					\
		printf("#define\t" name "\t((UX_FLOAT *) ((char *) "	 \
		   STR(MP_TABLE_NAME) " + %i))\n", BYTES(MP_BIT_OFFSET))

#    define PRINT_UX_TBL_ITEM(val)				\
		MP_BIT_OFFSET = print_ux_table_value(val, MP_BIT_OFFSET)

#    define PRINT_UX_TBL_ADEF_ITEM(name, val)				\
                PRINT_UX_TBL_ADEF(name);  PRINT_UX_TBL_ITEM(val)

    @divert divertText

    function print_ux_fraction_digits(value)
        {
        auto hi, i;
        
        if (value >= 1)
            {
            printf("ERROR: value out of range in print_ux_fraction_digits\n");
            exit;
            }

        for (i = NUM_UX_FRACTION_DIGITS; i > 0; i--)
            {
            value = bldexp(value, BITS_PER_UX_FRACTION_DIGIT_TYPE);
            hi = trunc(value);
            if (hi)
                printf( DIGIT_FORMAT, hi);
            else
                printf( ZERO_FORMAT );
            value -= hi;
            }
        printf("\n");

        return value;
        }


    function print_ux_table_value(value, offset)
        {
        auto exponent, hi, sign_bit, i;
        
        sign_bit = 0;
        if (value == 0)
            exponent = bldexp(-1, F_EXP_WIDTH + 2);
        else
            {
            exponent = bexp(value);
            if (value < 0)
                {
                sign_bit = 1;
                value = -value;
                }
            value = bldexp(value, -exponent);
            }

        if (sign_bit)
            printf("\t/* %3i */ NEG, %4i,", BYTES(offset), exponent);
        else
            printf("\t/* %3i */ POS, %4i,", BYTES(offset), exponent);

        print_ux_fraction_digits(value);
        return offset + BITS_PER_UX_SIGN_TYPE + BITS_PER_UX_EXPONENT_TYPE +
           UX_PRECISION;
        }


    function find_max_exponent(degree, index)
        {
        auto i, max_exponent;

        max_exponent = -128;
        for (i = 0; i <= degree; i++)
             {
             exponent = bexp(bround(ux_rational_coefs[index + i], 128));
             if (exponent > max_exponent)
                 max_exponent = exponent;
             }
        return max_exponent;
        }


    /*
    ** The following routine prints out coefficients in the array
    **  'ux_rational_coefs' in fixed point format.
    */

    procedure print_ux_poly_coefs(pad_len, degree, final_scale, index)
        {
        auto scale, exponent, i;

        for (i = 0; i < pad_len; i++)
            {
	    printf( "\t/* %3i */ %#32.4.16i,\n", BYTES(MP_BIT_OFFSET), 0);
            MP_BIT_OFFSET += 128;
            }

        exponent = find_max_exponent(degree, index);
        scale = 128 - exponent;
        index += degree;
        for (i = degree; i >= 0; i--)
            {
	    printf( "\t/* %3i */ %#32.4.16i,\n", BYTES(MP_BIT_OFFSET), 
                abs(nint(bldexp(ux_rational_coefs[index], scale ))));
            MP_BIT_OFFSET += 128;
            index--;
            }
        PRINT_U_TBL_ITEM(exponent + final_scale);
        }

    function print_ux_rational_coefs( num_degree, den_degree, scale)
        {
        auto max_degree;

        max_degree = max(num_degree, den_degree);
        print_ux_poly_coefs(max_degree - num_degree , num_degree, scale, 0);

        if (den_degree)
            print_ux_poly_coefs(max_degree - den_degree, den_degree, 0,
                num_degree + 1);

        return max_degree;
        }

    /*
    ** This routine finds the "width" and "bias" for converting MP numbers 
    ** to a special 128 bit packed format used for special polynomial
    ** evaluations. The coefficients are contained in the global array
    ** ux_rational_coef and the both the width and the bias are returned
    ** via global values.  See the description in dpml_ux_ops.c
    */

    procedure find_exponent_width_and_bias(degree, base_index)
        {
        auto i, top, _diff, min_diff, max_diff, old_exp, new_exp, width;
    
        top = base_index + degree;
        min_diff = max_diff = 0;
        old_exp = 0;
        for (i = base_index; i <= top; i++)
            {
            new_exp = bexp(ux_rational_coefs[i]);
            _diff = new_exp - old_exp;
            if (_diff < min_diff)
                min_diff = _diff;
            else if (_diff > max_diff)
                max_diff = _diff;
            old_exp = new_exp;
            }
    
        _diff = max_diff - min_diff + 1;
        width = bexp(_diff);
        if (bldexp(.5, width) == _diff)
            width--;
        packed_exponent_width = width;
        packed_exponent_bias = -min_diff;
        }

    /*
    ** After we know the bias and the width, we need to pack the coefficient
    ** values
    */

    procedure cvt_to_packed(degree, base_index, width, bias)
        {
        auto i, top, num_bits, tmp, old_exp, new_exp, sign_bit;

        find_exponent_width_and_bias(degree, base_index);
        top = base_index + degree;
        old_exp = 0;
        num_bits = UX_PRECISION - width - 1;
        for (i = base_index; i <= top; i++)
            {
            sign_bit = 0;
            tmp = bround(ux_rational_coefs[i], num_bits);
            if (tmp < 0)
                {
                tmp = -tmp;
                sign_bit = 1;
                }
            new_exp = bexp(tmp);
            tmp = nint(bldexp(tmp, num_bits - new_exp));
            tmp = bldexp(tmp, width + 1) + 2*(new_exp - old_exp + bias) +
                 sign_bit;
            old_exp = new_exp;
            ux_rational_coefs[i] = tmp;
            }
        }

    /*
    ** After converting to pack format, we need to print them out
    */

    procedure print_packed(degree, base_index)
        {
        auto i, top;

        top = base_index + degree;
        for (i = degree; i >= 0; i--)
            {
            printf( "\t/* %3i */ %#32.4.16i,\n", BYTES(MP_BIT_OFFSET),
                ux_rational_coefs[top--]);
            MP_BIT_OFFSET += 128;
            }
        }

    @end_divert

#endif


#if !defined(EXTENDED_DIGIT_MULTIPLY)
#   define EXTENDED_DIGIT_MULTIPLY(a,b,h,l) (l) = (a)*(b); UMULH(a,b,h)
#endif

#endif
