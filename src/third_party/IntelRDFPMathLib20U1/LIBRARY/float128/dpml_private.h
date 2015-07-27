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

#ifndef DPML_PRIVATE_H
#define DPML_PRIVATE_H


#ifndef TRUE
#    define     TRUE    1
#endif

#ifndef FALSE
#    define     FALSE   0
#endif


#include "build.h"
#include "op_system.h"
#include "compiler.h"
#include "architecture.h"
#include "i_format.h"
#include "f_format.h"

#if NEW_DPML_MACROS == 1

#   if MULTIPLE_ISSUE
#       define PIPELINED	1
#   else
#       define PIPELINED	0
#   endif

#endif

#define	DPML_NULL_MACRO
#define DPML_NULL_MACRO_TOKEN   1

/*
 * For values that are small powers of two, the follow macros are useful for
 * generating the base two log of that values.  For example,
 * LOG2(BITS_PER_F_TYPE) will evaluate to 5, 6 or 7 for floating point
 * types s/f, t/g or x.
 */

#define	__LOG2(name)	PASTE_2(__LOG2_,name)
#define __LOG2_1	0
#define __LOG2_2	1
#define __LOG2_4	2
#define __LOG2_8	3
#define __LOG2_16	4
#define __LOG2_32	5
#define __LOG2_64	6
#define __LOG2_128	7
#define __LOG2_256	8
#define __LOG2_512	9
#define __LOG2_1024	10
#define __LOG2_2048	11
#define __LOG2_4096	12
#define __LOG2_8192	13
#define __LOG2_16384	14
#define __LOG2_32768	15
#define __LOG2_65536	16


#if defined(F_TYPE)
#    define     GENERIC_TYPE            F_TYPE
#else
#    define     GENERIC_TYPE            I_TYPE
#endif

#ifndef TYPE1
#   define      TYPE1   GENERIC_TYPE
#endif

#ifndef TYPE2
#    define     TYPE2   GENERIC_TYPE
#endif

#ifndef TYPE3
#    define     TYPE3   GENERIC_TYPE
#endif

#if ((defined(ALPHA) || defined(alpha)) && (defined(wnt) || defined(vms)))
#   define EXP_WORD_TYPE INT_64
#else
#   define EXP_WORD_TYPE WORD
#endif

#if defined(MAKE_INCLUDE) || defined(MAKE_MTC)
#    include "mtc_macros.h"
#    include "mphoc_macros.h"
#endif

#include "poly_macros.h"
#include "assert.h"
#include "dpml_names.h"
#include "dpml_exception.h"

# define C_F_PROTO( name )	extern F_COMPLEX name( F_TYPE )
# define C_FF_PROTO( name )	extern F_COMPLEX name( F_TYPE, F_TYPE )
# define C_FI_PROTO( name )	extern F_COMPLEX name( F_TYPE, WORD )
# define C_FFFF_PROTO( name )	extern F_COMPLEX name( F_TYPE, F_TYPE, F_TYPE, F_TYPE )
# define C_p_PROTO( name )	extern F_COMPLEX name( F_COMPLEX * )
# define C_s_PROTO( name )      extern F_COMPLEX name( F_COMPLEX  )

# define C_B_PROTO( name )	extern B_COMPLEX name( B_TYPE )
# define C_BB_PROTO( name )	extern B_COMPLEX name( B_TYPE, B_TYPE )
# define C_BBBB_PROTO( name )	extern B_COMPLEX name( B_TYPE, B_TYPE, B_TYPE, B_TYPE )

# define F_F_PROTO( name )	extern F_TYPE name( F_TYPE )
# define F_FF_PROTO( name )	extern F_TYPE name( F_TYPE, F_TYPE )
# define F_FI_PROTO( name )	extern F_TYPE name( F_TYPE, WORD )
# define F_FpI_PROTO( name )	extern F_TYPE name( F_TYPE, WORD* )
# define F_IF_PROTO( name )	extern F_TYPE name( WORD, F_TYPE )

# define B_B_PROTO( name )	extern B_TYPE name( B_TYPE )
# define B_BB_PROTO( name )	extern B_TYPE name( B_TYPE, B_TYPE )
# define B_BI_PROTO( name )	extern B_TYPE name( B_TYPE, WORD )
# define B_BpI_PROTO( name )	extern B_TYPE name( B_TYPE, WORD* )
# define B_IB_PROTO( name )	extern B_TYPE name( WORD, B_TYPE )

# define I_F_PROTO( name )	extern WORD name( F_TYPE )
# define I_FpF_PROTO( name )	extern WORD name( F_TYPE, F_TYPE* )
# define I_FIpF_PROTO( name )	extern WORD name( F_TYPE, WORD, F_TYPE* )
# define I_FIpFpF_PROTO( name )	extern WORD name( F_TYPE, WORD, F_TYPE*, F_TYPE* )

# define I_B_PROTO( name )	extern WORD name( B_TYPE )
# define I_BpB_PROTO( name )	extern WORD name( B_TYPE, B_TYPE* )
# define I_BIpB_PROTO( name )	extern WORD name( B_TYPE, WORD, B_TYPE* )
# define I_BIpBpB_PROTO( name )	extern WORD name( B_TYPE, WORD, B_TYPE*, B_TYPE* )

# define I_II_PROTO( name )	extern WORD name( WORD, WORD )

#define F_C_NAN		0
#define F_C_INF		1
#define F_C_NORM	2
#define F_C_DENORM	3
#define F_C_ZERO	4

#define F_C_POS_CLASS(n)	((n) << 1)
#define F_C_NEG_CLASS(n)	(((n) << 1) | 1)
#define F_C_BASE_CLASS(c)	((c) >> 1)
#define F_C_IS_NEG_CLASS(c)	((c) & 1)
#define F_C_IS_POS_CLASS(c)	(((c) & 1) == 0)

/* The F_C_* defs must be in the current order, enumerated from 0 to 9 */

#   define F_C_SIG_NAN		F_C_POS_CLASS(F_C_NAN)		/* 0 */
#   define F_C_QUIET_NAN	F_C_NEG_CLASS(F_C_NAN)		/* 1 */
#   define F_C_POS_INF		F_C_POS_CLASS(F_C_INF)		/* 2 */
#   define F_C_NEG_INF		F_C_NEG_CLASS(F_C_INF)		/* 3 */
#   define F_C_POS_NORM		F_C_POS_CLASS(F_C_NORM)		/* 4 */
#   define F_C_NEG_NORM		F_C_NEG_CLASS(F_C_NORM)		/* 5 */
#   define F_C_POS_DENORM	F_C_POS_CLASS(F_C_DENORM)	/* 6 */
#   define F_C_NEG_DENORM	F_C_NEG_CLASS(F_C_DENORM)	/* 7 */
#   define F_C_POS_ZERO		F_C_POS_CLASS(F_C_ZERO)		/* 8 */
#   define F_C_NEG_ZERO		F_C_NEG_CLASS(F_C_ZERO)		/* 9 */

#   define F_C_NUM_CLASSES	10
#   define F_C_CLASS_BIT_WIDTH	4


#define AS_WORD(p) (*(WORD *)&(p))
#define AS_CHAR(p) (*(char *)&(p))
#define AS_SHORT(p) (*(short *)&(p))
#define AS_INT(p) (*(int *)&(p))
#define AS_LONG(p) (*(long *)&(p))
#define AS_FLOAT(p) (*(float *)&(p))
#define AS_DOUBLE(p) (*(double *)&(p))
#define AS_F_TYPE(p) (*(F_TYPE *)&(p))
#define AS_B_TYPE(p) (*(B_TYPE *)&(p))



/* Environment specific macro definitions that pre-empt the generic
(and perhaps slow) definitions below are in include files per
ARCHITECTURE.  The macros defined in these files should be a subset of
the macros defined below (i.e. if there is a specific version, there
should also be a generic version that will work with any ANSI C
compiler).  [ In practice, we may not get around to writing the generic
versions until we need them. ] */


#if (ARCHITECTURE == vax)

#    include "vax_macros.h"

#elif (ARCHITECTURE == mips)

#    include "mips_macros.h"

#elif (ARCHITECTURE == hp_pa)

#    include "ix86_macros.h"

#elif (ARCHITECTURE == cray)

#    include "cray_macros.h"

#elif (ARCHITECTURE == alpha)

#    include "alpha_macros.h"

#elif (ARCHITECTURE == ix86)

#    include "ix86_macros.h"

#elif (ARCHITECTURE == merced)

#include "ix86_macros.h"

#elif (ARCHITECTURE == amd64 )

#    include "ix86_macros.h"

#elif (ARCHITECTURE == sparc )

#    include "ix86_macros.h"

#else

#    error Unknown ARCHITECTURE.

#endif


# if (defined( _WIN32 ) && defined( _M_IX86 )) || (defined(merced) && !defined(HPUX_OS))

/*  Disallow use of intrinsic math functions on Windows NT on Intel  */

    double acos( double ) ;
#	pragma function( acos )
    double asin( double ) ;
#	pragma function( asin )
    double atan( double ) ;
#	pragma function( atan )
    double atan2( double, double ) ;
#	pragma function( atan2 )
    double cos( double ) ;
#	pragma function( cos )
    double cosh( double ) ;
#	pragma function( cosh )
    double exp( double ) ;
#	pragma function( exp )
    double fabs( double ) ;
#	pragma function( fabs )
    double fmod( double, double ) ;
#	pragma function( fmod )
    double log( double ) ;
#	pragma function( log )
    double log10( double ) ;
#	pragma function( log10 )
    double pow( double, double ) ;
#	pragma function( pow )
    double sin( double ) ;
#	pragma function( sin )
    double sinh( double ) ;
#	pragma function( sinh )
    double sqrt( double ) ;
#	pragma function( sqrt )
    double tan( double ) ;
#	pragma function( tan )
    double tanh( double ) ;
#	pragma function( tanh )

# endif  /*  defined( _WIN32 ) && defined( _M_IX86 ) */


# if defined(merced)

    float acosf( float ) ;
#       pragma function( acosf )
    float asinf( float ) ;
#       pragma function( asinf )
    float atanf( float ) ;
#       pragma function( atanf )
    float atan2f( float, float ) ;
#       pragma function( atan2f )
    float cosf( float ) ;
#       pragma function( cosf )
    float coshf( float ) ;
#       pragma function( coshf )
    float expf( float ) ;
#       pragma function( expf )
    float fabsf( float ) ;
#       pragma function( fabsf )
    float fmodf( float, float ) ;
#       pragma function( fmodf )
    float logf( float ) ;
#       pragma function( logf )
    float log10f( float ) ;
#       pragma function( log10f )
    float powf( float, float ) ;
#       pragma function( powf )
    float sinf( float ) ;
#       pragma function( sinf )
    float sinhf( float ) ;
#       pragma function( sinhf )
    float sqrtf( float ) ;
#       pragma function( sqrtf )
    float tanf( float ) ;
#       pragma function( tanf )
    float tanhf( float ) ;
#       pragma function( tanhf )
    float ceilf( float ) ;
#       pragma function( ceilf )
    float floorf( float ) ;
#       pragma function( floorf )

#endif



/* General macros and generic (though perhaps slow) versions of the
specific macro definitions included above.  */



#ifndef F_IS_NAN
#define F_IS_NAN(x) (x != x)
#endif

#ifndef F_IS_ZERO
#define F_IS_ZERO(x) (x == 0.0)
#endif

#ifndef F_IS_NEG
#define F_IS_NEG(x) (x < 0.0)
#endif

#ifndef F_IS_POS
#define F_IS_POS(x) (x > 0.0)
#endif

#ifndef F_SET_FLAG_IF_ZERO
#define F_SET_FLAG_IF_ZERO(x,flag) { \
        (flag) = ((x) == 0.0); \
}
#endif

#if 0
#ifndef F_SET_FLAG_IF_NEG
#define F_SET_FLAG_IF_NEG(x,flag) { \
        (flag) = ((x) < 0.0); \
}
#endif
#endif

#ifndef F_SET_FLAG_IF_POS
#define F_SET_FLAG_IF_POS(x,flag) { \
        (flag) = ((x) > 0.0); \
}
#endif


#if (VAX_FLOATING) 

#ifndef F_EXP_WORD_IS_ABNORMAL
#define F_EXP_WORD_IS_ABNORMAL(exp_word) (!((exp_word) & F_EXP_MASK))
#endif

#ifndef F_EXP_WORD_IS_ABNORMAL_OR_NEG
#define F_EXP_WORD_IS_ABNORMAL_OR_NEG(exp_word) \
        ((INT_16)((exp_word) & ((1 << 16) - 1)) < (INT_16)(1 << F_EXP_POS))
#endif
#ifndef B_EXP_WORD_IS_ABNORMAL_OR_NEG
#define B_EXP_WORD_IS_ABNORMAL_OR_NEG(exp_word) \
        ((INT_16)((exp_word) & ((1 << 16) - 1)) < (INT_16)(1 << B_EXP_POS))
#endif

#ifndef F_EXP_WORD_IS_INFINITE_OR_NAN
/* It is assumed that ROP detection has already been done */
#define F_EXP_WORD_IS_INFINITE_OR_NAN(exp_word) (0)
#endif

#ifndef F_SET_FLAG_IF_ZERO_OR_DENORM
#define F_SET_FLAG_IF_ZERO_OR_DENORM(x,flag) { \
        F_UNION u; \
        u.f = (x); \
        (flag) = (!(u.F_HI_WORD & F_SIGN_EXP_MASK)); \
}
#endif

#ifndef F_SET_FLAG_IF_DENORM
#define F_SET_FLAG_IF_DENORM(x,flag) { \
        (flag) = 0; \
}
#endif

#ifndef F_SET_FLAG_IF_INF
#define F_SET_FLAG_IF_INF(x,flag) { \
        (flag) = 0; \
}
#endif

#ifndef F_SET_FLAG_IF_FINITE
#define F_SET_FLAG_IF_FINITE(x,flag) { \
        (flag) = 1; \
}
#endif

#ifndef F_SET_FLAG_IF_NAN
#define F_SET_FLAG_IF_NAN(x,flag) { \
        F_UNION u; \
        u.f = (x); \
        (flag) = ((u.F_HI_WORD & F_SIGN_EXP_MASK) == F_SIGN_BIT_MASK); \
}
#endif

#ifndef F_SET_FLAG_IF_NAN_OR_INF
#define F_SET_FLAG_IF_NAN_OR_INF(x,flag) { \
        F_UNION u; \
        u.f = (x); \
        (flag) = ((u.F_HI_WORD & F_SIGN_EXP_MASK) == F_SIGN_BIT_MASK); \
}
#endif

#ifndef F_SET_FLAG_IF_NORM
#define F_SET_FLAG_IF_NORM(x,flag) { \
        F_UNION u; \
        u.f = (x); \
        (flag) = (u.F_HI_WORD & F_EXP_MASK); \
}
#endif

#ifndef F_CLASSIFY
#define F_CLASSIFY(x,class) { \
        U_WORD exp; \
        F_UNION u; \
        u.f = (x); \
        (class) = (((U_WORD)u.F_HI_WORD >> F_SIGN_BIT_POS) & 0x1); \
        exp = (u.F_HI_WORD & F_EXP_MASK); \
        if (exp) \
                (class) += F_C_POS_NORM; \
        else \
                (class) = ((class) ? F_C_SIG_NAN : F_C_POS_ZERO); \
}
#endif

#ifndef F_CLASSIFY_AND_GET_EXP_WORD
#define F_CLASSIFY_AND_GET_EXP_WORD(x,class,exp_word) { \
        U_WORD exp; \
        F_UNION u; \
        u.f = (x); \
        exp_word = u.F_HI_WORD; \
        (class) = (((U_WORD)u.F_HI_WORD >> F_SIGN_BIT_POS) & 0x1); \
        exp = (u.F_HI_WORD & F_EXP_MASK); \
        if (exp) \
                (class) += F_C_POS_NORM; \
        else \
                (class) = ((class) ? F_C_SIG_NAN : F_C_POS_ZERO); \
}
#endif



#elif (IEEE_FLOATING)



#ifndef F_EXP_WORD_IS_ABNORMAL
#define F_EXP_WORD_IS_ABNORMAL(exp_word) \
        (((exp_word) & F_EXP_MASK) - ((U_WORD)1 << F_EXP_POS) \
        >= MAKE_MASK(F_EXP_WIDTH - 1, F_EXP_POS + 1))
#endif

#ifndef F_EXP_WORD_IS_ABNORMAL_OR_NEG
#define F_EXP_WORD_IS_ABNORMAL_OR_NEG(exp_word) \
        ((exp_word) - ((U_WORD)1 << F_EXP_POS) \
        >= MAKE_MASK(F_EXP_WIDTH - 1, F_EXP_POS + 1))
#endif
#ifndef B_EXP_WORD_IS_ABNORMAL_OR_NEG
#define B_EXP_WORD_IS_ABNORMAL_OR_NEG(exp_word) \
        ((exp_word) - ((U_WORD)1 << B_EXP_POS) \
        >= MAKE_MASK(B_EXP_WIDTH - 1, B_EXP_POS + 1))
#endif

#ifndef F_EXP_WORD_IS_INFINITE_OR_NAN
#define F_EXP_WORD_IS_INFINITE_OR_NAN(exp_word) \
        (((exp_word) & F_EXP_MASK) == F_EXP_MASK)
#endif

#ifndef F_SET_FLAG_IF_ZERO_OR_DENORM
#define F_SET_FLAG_IF_ZERO_OR_DENORM(x,flag) { \
        F_UNION u; \
        u.f = (x); \
        flag = (!(u.F_HI_WORD & F_EXP_MASK)); \
}
#endif

#ifndef F_SET_FLAG_IF_DENORM
#define F_SET_FLAG_IF_DENORM(x,flag) { \
		F_UNION u; \
		u.f = (x); \
		flag = (!(u.F_HI_WORD & F_EXP_MASK) \
			&&  ((u.F_HI_WORD & F_MANTISSA_MASK) OR_LOW_BITS_SET(u))); \
}
#endif

#ifndef F_SET_FLAG_IF_INF
#define F_SET_FLAG_IF_INF(x,flag) { \
		F_UNION u; \
		u.f = (x); \
		(flag) = (((u.F_HI_WORD & F_EXP_MASK) == F_EXP_MASK) \
			&&  (!((u.F_HI_WORD & F_MANTISSA_MASK) OR_LOW_BITS_SET(u)))); \
}
#endif

#ifndef F_SET_FLAG_IF_FINITE
#define F_SET_FLAG_IF_FINITE(x,flag) { \
        F_UNION u; \
        u.f = (x); \
        (flag) = ((u.F_HI_WORD & F_EXP_MASK) != F_EXP_MASK); \
}
#endif

#ifndef F_SET_FLAG_IF_NAN
#define F_SET_FLAG_IF_NAN(x,flag) { \
		F_UNION u; \
		u.f = (x); \
		(flag) = (((u.F_HI_WORD & F_EXP_MASK) == F_EXP_MASK) \
			&&    ((u.F_HI_WORD & F_MANTISSA_MASK) OR_LOW_BITS_SET(u))); \
}
#endif

#ifndef F_SET_FLAG_IF_NAN_OR_INF
#define F_SET_FLAG_IF_NAN_OR_INF(x,flag) { \
        F_UNION u; \
        u.f = (x); \
        (flag) = ((u.F_HI_WORD & F_EXP_MASK) == F_EXP_MASK); \
}
#endif

#ifndef F_SET_FLAG_IF_NORM
#define F_SET_FLAG_IF_NORM(x,flag) { \
        F_UNION u; \
        u.f = (x); \
        (flag) = (u.F_HI_WORD & F_EXP_MASK); \
        (flag) = ((flag) && (flag < F_EXP_MASK)); \
}
#endif

#ifndef F_CLASSIFY
#define F_CLASSIFY(x,class) { \
        U_WORD exp; \
        F_UNION u; \
        u.f = (x); \
        (class) = (((U_WORD)u.F_HI_WORD >> F_SIGN_BIT_POS) & 0x1); \
        exp = (u.F_HI_WORD & F_EXP_MASK); \
        if (exp) { \
                if (exp < F_EXP_MASK) \
                        (class) += F_C_POS_NORM; \
                else { \
                        u.F_HI_WORD &= F_MANTISSA_MASK; \
                        if (u.F_HI_WORD OR_LOW_BITS_SET(u)) { \
								(class) = (((U_WORD)u.F_HI_WORD >> F_MSB_POS) & 0x1); \
						} else \
                                (class) += F_C_POS_INF; \
                } \
        } else { \
                u.F_HI_WORD &= F_MANTISSA_MASK; \
                (class) += \
                        ((u.F_HI_WORD OR_LOW_BITS_SET(u)) ? F_C_POS_DENORM : F_C_POS_ZERO); \
        } \
}
#endif

#ifndef F_CLASSIFY_AND_GET_EXP_WORD
#define F_CLASSIFY_AND_GET_EXP_WORD(x,class,exp_word) { \
        U_WORD exp; \
        F_UNION u; \
        u.f = (x); \
        exp_word = u.F_HI_WORD; \
        (class) = (((U_WORD)u.F_HI_WORD >> F_SIGN_BIT_POS) & 0x1); \
        exp = (u.F_HI_WORD & F_EXP_MASK); \
        if (exp) { \
                if (exp < F_EXP_MASK) \
                        (class) += F_C_POS_NORM; \
                else { \
                        u.F_HI_WORD &= F_MANTISSA_MASK; \
                        if (u.F_HI_WORD OR_LOW_BITS_SET(u)) { \
								(class) = (((U_WORD)u.F_HI_WORD >> F_MSB_POS) & 0x1); \
                        } else \
                                (class) += F_C_POS_INF; \
                } \
        } else { \
                u.F_HI_WORD &= F_MANTISSA_MASK; \
                (class) += \
                        ((u.F_HI_WORD OR_LOW_BITS_SET(u)) ? F_C_POS_DENORM : F_C_POS_ZERO); \
        } \
}
#endif




#endif  /* floating type */


#ifndef F_SET_FLAG_IF_NEG
#define F_SET_FLAG_IF_NEG(x,flag) { \
        F_UNION u; \
        u.f = (x); \
        (flag) = ((u.F_HI_WORD) & F_SIGN_BIT_MASK); \
}
#endif


#ifndef F_EXP_WORD_IS_ZERO_OR_DENORM
#define F_EXP_WORD_IS_ZERO_OR_DENORM(exp_word) \
        (!((exp_word) & F_EXP_MASK))
#endif

#ifndef B_EXP_WORD_IS_ZERO_OR_DENORM
#define B_EXP_WORD_IS_ZERO_OR_DENORM(exp_word) \
        (!((exp_word) & B_EXP_MASK))
#endif

#ifndef F_EXP_WORD_IS_NEG
#define F_EXP_WORD_IS_NEG(exp_word) \
        ((exp_word) & F_SIGN_BIT_MASK)
#endif

#ifndef B_EXP_WORD_IS_NEG
#define B_EXP_WORD_IS_NEG(exp_word) \
        ((exp_word) & B_SIGN_BIT_MASK)
#endif

#ifndef F_EXP_WORD_IS_POS
#define F_EXP_WORD_IS_POS(exp_word) \
        (!((exp_word) & F_SIGN_BIT_MASK))
#endif


#ifndef SET_BIT
#    define SET_BIT(pos) ((U_WORD)1 << (pos))
#endif

#ifndef MAKE_MASK
#    define MAKE_MASK(width,pos) ((((U_WORD)1 << (width)) - 1) << (pos))
#endif


/* Rounding modes are done in an architecture specific way.  If no
specific macros were defined, assume there are no rounding modes. */

#ifndef GET_ROUNDING_MODE
#define GET_ROUNDING_MODE(old)
#endif

#ifndef SET_ROUNDING_MODE
#define SET_ROUNDING_MODE(new)
#endif

#ifndef SWAP_ROUNDING_MODE
#define SWAP_ROUNDING_MODE(new,old)
#endif

#ifndef FPU_STATUS_WORD_TYPE
#define FPU_STATUS_WORD_TYPE WORD
#endif

#ifndef INIT_FPU_STATE_AND_ROUND_TO_NEAREST
#define INIT_FPU_STATE_AND_ROUND_TO_NEAREST(status_word)
#endif

#ifndef INIT_FPU_STATE_AND_ROUND_TO_ZERO
#define INIT_FPU_STATE_AND_ROUND_TO_ZERO(status_word)
#endif

#ifndef RESTORE_FPU_STATE
#define RESTORE_FPU_STATE(status_word)
#endif



/*  Constants in bytes, for table indexing */

#define BYTES_PER_S_TYPE    (BITS_PER_S_TYPE/BITS_PER_CHAR)
#define BYTES_PER_D_TYPE    (BITS_PER_D_TYPE/BITS_PER_CHAR)
#define BYTES_PER_Q_TYPE    (BITS_PER_Q_TYPE/BITS_PER_CHAR)
#define BYTES_PER_B_TYPE    (BITS_PER_B_TYPE/BITS_PER_CHAR)
#define BYTES_PER_R_TYPE    (BITS_PER_R_TYPE/BITS_PER_CHAR)
  


/* Make_float primitives */

#define S_MAKE_FLOAT(i,s)       { \
                                S_UNION u; \
                                u.S_HI_WORD = (i); \
                                s = u.f; \
}
#if WORDS_PER_D_TYPE == 1
#    define D_MAKE_FLOAT(i,s)   { \
                                D_UNION u; \
                                u.D_HI_WORD = (i); \
                                s = u.f; \
}
#elif WORDS_PER_D_TYPE == 2
#    define D_MAKE_FLOAT(i,s)   { \
                                D_UNION u; \
                                u.D_HI_WORD = (i); \
                                u.D_LO_WORD = 0; \
                                s = u.f; \
}
#else
#    error Surprising number of words per D_FLOAT
#endif

#define D_MAKE_FLOAT_64(i,s)   { \
                                D_UNION u; \
                                u.D_UNSIGNED_HI_64 = (i); \
                                s = u.f; \
}

#define Q_MAKE_FLOAT(i,s)       { \
                                Q_UNION u; \
                                u.f = 0.0; \
                                u.Q_HI_WORD = (i); \
                                s = u.f; \
}

#define F_EXP_MAKE_FLOAT        PASTE_2(F_PREC_CHAR,_MAKE_FLOAT)
#define B_EXP_MAKE_FLOAT        PASTE_2(B_PREC_CHAR,_MAKE_FLOAT)

#define F_MAKE_FLOAT(i,s)       F_EXP_MAKE_FLOAT(i,s)
#define B_MAKE_FLOAT(i,s)       B_EXP_MAKE_FLOAT(i,s)

/* The following several macros are intended to be used as a set.  It
is the combination of F_SAVE_SIGN_AND_GET_ABS and F_RESTORE_SIGN (or
F_NEGATE_IF_SIGN_NEG) that should be efficient (i.e. if slowing one of
them down will make the combination faster, go ahead and do it. */

#ifndef F_SIGN_TYPE

#       define F_SIGN_TYPE U_WORD

#       define F_SAVE_SIGN_AND_GET_ABS(x, sign, abs_x) { \
                F_TYPE save_x = (x); \
                F_ABS((x), (abs_x)); \
                (sign) = ((abs_x) != save_x); \
        }

#       define F_CHANGE_SIGN(sign) \
                (sign) = !(sign)

#       define F_RESTORE_SIGN(sign, x) \
                ASSERT((x) >= 0.0); \
                if (sign) F_NEGATE(x);

#       define F_NEGATE_IF_SIGN_NEG(sign, x) \
                if (sign) F_NEGATE(x);

#endif







#ifndef S_NEGATE
#define S_NEGATE(x) (x) = -(x)
#endif

#ifndef D_NEGATE
#define D_NEGATE(x) (x) = -(x)
#endif

#ifndef F_NEGATE
#define F_NEGATE(x) (x) = -(x)
#endif

#ifndef B_NEGATE
#define B_NEGATE(x) (x) = -(x)
#endif


#ifndef S_SET_NEG_BIT
#define S_SET_NEG_BIT(x) if ((x) > 0.0) S_NEGATE(x);
#endif

#ifndef D_SET_NEG_BIT
#define D_SET_NEG_BIT(x) if ((x) > 0.0) D_NEGATE(x);
#endif

#ifndef F_SET_NEG_BIT
#define F_SET_NEG_BIT(x) if ((x) > 0.0) F_NEGATE(x);
#endif

#ifndef B_SET_NEG_BIT
#define B_SET_NEG_BIT(x) if ((x) > 0.0) B_NEGATE(x);
#endif


#ifndef S_CLEAR_NEG_BIT
#define S_CLEAR_NEG_BIT(x) if ((x) < 0.0) S_NEGATE(x);
#endif

#ifndef D_CLEAR_NEG_BIT
#define D_CLEAR_NEG_BIT(x) if ((x) < 0.0) D_NEGATE(x);
#endif

#ifndef F_CLEAR_NEG_BIT
#define F_CLEAR_NEG_BIT(x) if ((x) < 0.0) F_NEGATE(x);
#endif

#ifndef B_CLEAR_NEG_BIT
#define B_CLEAR_NEG_BIT(x) if ((x) < 0.0) B_NEGATE(x);
#endif


#ifndef S_ABS
#define S_ABS(x,abs_x) { \
        (abs_x) = (x); \
        S_CLEAR_NEG_BIT(abs_x); \
}
#endif

#ifndef D_ABS
#define D_ABS(x,abs_x) { \
        (abs_x) = (x); \
        D_CLEAR_NEG_BIT(abs_x); \
}
#endif

#ifndef F_ABS
#define F_ABS(x,abs_x) { \
        (abs_x) = (x); \
        F_CLEAR_NEG_BIT(abs_x); \
}
#endif

#ifndef B_ABS
#define B_ABS(x,abs_x) { \
        (abs_x) = (x); \
        B_CLEAR_NEG_BIT(abs_x); \
}
#endif


/* Note that these copy_sign macros do not work correctly with -0.0 */

#ifndef S_COPY_SIGN
#undef  S_COPY_SIGN_IS_FAST
#define S_COPY_SIGN(value,sign,result) { \
        if ((sign) < 0.0) \
	{ \
	        S_ABS((value), (result)); \
                S_NEGATE(result); \
	} \
	else \
	        S_ABS((value), (result)); \
}
#endif

#ifndef D_COPY_SIGN
#undef  D_COPY_SIGN_IS_FAST
#define D_COPY_SIGN(value,sign,result) { \
        if ((sign) < 0.0) \
	{ \
	        D_ABS((value), (result)); \
                D_NEGATE(result); \
	} \
	else \
	        D_ABS((value), (result)); \
}
#endif

#ifndef F_COPY_SIGN
#undef  F_COPY_SIGN_IS_FAST
#define F_COPY_SIGN(value,sign,result) { \
        if ((sign) < 0.0) \
	{ \
	        F_ABS((value), (result)); \
                F_NEGATE(result); \
	} \
	else \
	        F_ABS((value), (result)); \
}
#endif

#ifndef B_COPY_SIGN
#undef  B_COPY_SIGN_IS_FAST
#define B_COPY_SIGN(value,sign,result) { \
        if ((sign) < 0.0) \
	{ \
	        B_ABS((value), (result)); \
                B_NEGATE(result); \
	} \
	else \
	        B_ABS((value), (result)); \
}
#endif


#ifndef S_COPY_SIGN_AND_EXP
#undef  S_COPY_SIGN_AND_EXP_IS_FAST
#define S_COPY_SIGN_AND_EXP(value,sign_and_exp,result) { \
        S_UNION u; \
        U_WORD new_sign_exp; \
        u.f = sign_and_exp; \
        new_sign_exp = u.S_HI_WORD & S_SIGN_EXP_MASK; \
        u.f = value; \
        u.S_HI_WORD &= ~S_SIGN_EXP_MASK; \
        u.S_HI_WORD |= new_sign_exp; \
        result = u.f; \
}
#endif

#ifndef D_COPY_SIGN_AND_EXP
#undef  D_COPY_SIGN_AND_EXP_IS_FAST
#define D_COPY_SIGN_AND_EXP(value,sign_and_exp,result) { \
        D_UNION u; \
        U_WORD new_sign_exp; \
        u.f = sign_and_exp; \
        new_sign_exp = u.D_HI_WORD & D_SIGN_EXP_MASK; \
        u.f = value; \
        u.D_HI_WORD &= ~D_SIGN_EXP_MASK; \
        u.D_HI_WORD |= new_sign_exp; \
        result = u.f; \
}
#endif

#ifndef F_COPY_SIGN_AND_EXP
#undef  F_COPY_SIGN_AND_EXP_IS_FAST
#define F_COPY_SIGN_AND_EXP(value,sign_and_exp,result) { \
        F_UNION u; \
        U_WORD new_sign_exp; \
        u.f = sign_and_exp; \
        new_sign_exp = u.F_HI_WORD & F_SIGN_EXP_MASK; \
        u.f = value; \
        u.F_HI_WORD &= ~F_SIGN_EXP_MASK; \
        u.F_HI_WORD |= new_sign_exp; \
        result = u.f; \
}
#endif

#ifndef B_COPY_SIGN_AND_EXP
#undef  B_COPY_SIGN_AND_EXP_IS_FAST
#define B_COPY_SIGN_AND_EXP(value,sign_and_exp,result) { \
        B_UNION u; \
        U_WORD new_sign_exp; \
        u.f = sign_and_exp; \
        new_sign_exp = u.B_HI_WORD & B_SIGN_EXP_MASK; \
        u.f = value; \
        u.B_HI_WORD &= ~B_SIGN_EXP_MASK; \
        u.B_HI_WORD |= new_sign_exp; \
        result = u.f; \
}
#endif







#ifndef F_COPY_NEG_SIGN

/* F_COPY_NEG_SIGN assumes the input value is non-negative.  If the
input value is negative, the sign of the result is undefined.  If the
input value is non-negative and sign is negative, the result will be
-(value).  If value is non-negative and sign is non-negative, the
result will = value. */

#if F_COPY_SIGN_IS_FAST

#       define F_COPY_NEG_SIGN(sign,abs_sign,value) \
                ASSERT((value) >= 0.0); \
                F_COPY_SIGN((value),(sign),(value))

#else

#       define F_COPY_NEG_SIGN(sign,abs_sign,value) { \
                ASSERT((value) >= 0.0); \
                if ((abs_sign) != (sign)) \
                        F_NEGATE(value); \
        }

#endif

#endif



#if (F_MAX_BIN_EXP > 2 * F_PRECISION)

#       define GOTO_CLEANUP_IF_POTENTIAL_OVERFLOW(x, t)

#else

#       define GOTO_CLEANUP_IF_POTENTIAL_OVERFLOW(x, t) { \
                F_TYPE abs_x; \
                F_ABS(x, abs_x); \
                if (abs_x >= t) \
                        goto cleanup; \
        }

#endif


#if (DPML_DEBUG)
#       define DPML_DEBUG_ABS(x) (((x) < 0.0) ? (-(x)) : (x))
#endif



#ifndef F_POS_RINT
#undef  F_POS_RINT_IS_FAST
#define F_POS_RINT_PRECISION_LIMIT (F_PRECISION - 1)
#define F_POS_RINT(x,y) { \
        F_TYPE t = F_POW_2(F_PRECISION - 1); \
        ASSERT((x) < t); \
        (y) = (x) + t; \
        (y) -= t; \
}
#endif

#ifndef F_NEG_RINT
#undef  F_NEG_RINT_IS_FAST
#define F_NEG_RINT_PRECISION_LIMIT (F_PRECISION - 1)
#define F_NEG_RINT(x,y) { \
        F_TYPE t = F_POW_2(F_PRECISION - 1); \
        ASSERT((x) > -t); \
        (y) = (x) - t; \
        (y) += t; \
}
#endif


#ifndef S_RINT
#define S_RINT_PRECISION_LIMIT (F_PRECISION - 1)
#define S_RINT(x,y) { \
        S_TYPE t = S_POW_2(S_PRECISION - 1); \
        ASSERT(DPML_DEBUG_ABS(x) < t); \
        S_COPY_SIGN(t, (x), t); \
        (y) = (x) + t; \
        (y) -= t; \
}
#endif

#ifndef D_RINT
#define D_RINT_PRECISION_LIMIT (F_PRECISION - 1)
#define D_RINT(x,y) { \
        D_TYPE t = D_POW_2(D_PRECISION - 1); \
        ASSERT(DPML_DEBUG_ABS(x) < t); \
        D_COPY_SIGN(t, (x), t); \
        (y) = (x) + t; \
        (y) -= t; \
}
#endif

#ifndef F_RINT
#undef  F_RINT_IS_FAST
#define F_RINT_PRECISION_LIMIT (F_PRECISION - 1)
#define F_RINT(x,y) { \
        F_TYPE t = F_POW_2(F_PRECISION - 1); \
        ASSERT(DPML_DEBUG_ABS(x) < t); \
        F_COPY_SIGN(t, (x), t); \
        (y) = (x) + t; \
        (y) -= t; \
}
#endif

#ifndef B_RINT
#undef  B_RINT_IS_FAST
#define B_RINT_PRECISION_LIMIT (B_PRECISION - 1)
#define B_RINT(x,y) { \
        B_TYPE t = B_POW_2(B_PRECISION - 1); \
        ASSERT(DPML_DEBUG_ABS(x) < t); \
        B_COPY_SIGN(t, (x), t); \
        (y) = (x) + t; \
        (y) -= t; \
}
#endif


#ifndef S_RINT_TO_FLOATING_AND_WORD
#define S_RINT_TO_FLOATING_AND_WORD_PRECISION_LIMIT (S_RINT_PRECISION_LIMIT)
#define S_RINT_TO_FLOATING_AND_WORD(x, flt_int_x, int_x) { \
        S_RINT((x), (flt_int_x)); \
        (int_x) = (WORD) (flt_int_x); \
}
#endif

#ifndef D_RINT_TO_FLOATING_AND_WORD
#define D_RINT_TO_FLOATING_AND_WORD_PRECISION_LIMIT (D_RINT_PRECISION_LIMIT)
#define D_RINT_TO_FLOATING_AND_WORD(x, flt_int_x, int_x) { \
        D_RINT((x), (flt_int_x)); \
        (int_x) = (WORD) (flt_int_x); \
}
#endif

#ifndef F_RINT_TO_FLOATING_AND_WORD
#define F_RINT_TO_FLOATING_AND_WORD_PRECISION_LIMIT (F_RINT_PRECISION_LIMIT)
#define F_RINT_TO_FLOATING_AND_WORD(x, flt_int_x, int_x) { \
        F_RINT((x), (flt_int_x)); \
        (int_x) = (WORD) (flt_int_x); \
}
#endif

#ifndef B_RINT_TO_FLOATING_AND_WORD
#define B_RINT_TO_FLOATING_AND_WORD_PRECISION_LIMIT (B_RINT_PRECISION_LIMIT)
#define B_RINT_TO_FLOATING_AND_WORD(x, flt_int_x, int_x) { \
        B_RINT((x), (flt_int_x)); \
        (int_x) = (WORD) (flt_int_x); \
}
#endif


#ifndef F_POS_TRUNC
#undef  F_POS_TRUNC_IS_FAST
#define F_POS_TRUNC_PRECISION_LIMIT (F_PRECISION - 1)
#define F_POS_TRUNC(x,y) { \
        F_TYPE orig_x = (x); \
        F_TYPE t = F_POW_2(F_PRECISION - 1); \
        ASSERT((x) < t); \
        (y) = x + t; \
        (y) -= t; \
        if ((y) > orig_x) \
                (y) -= 1.0; \
}
#endif

#ifndef F_NEG_TRUNC
#undef  F_NEG_TRUNC_IS_FAST
#define F_NEG_TRUNC_PRECISION_LIMIT (F_PRECISION - 1)
#define F_NEG_TRUNC(x,y) { \
        F_TYPE orig_x = (x); \
        F_TYPE t = F_POW_2(F_PRECISION - 1); \
        ASSERT((x) > -t); \
        (y) = x - t; \
        (y) += t; \
        if ((y) < orig_x) \
                (y) += 1.0; \
}
#endif

#ifndef F_TRUNC
#undef  F_TRUNC_IS_FAST
#define F_TRUNC_PRECISION_LIMIT (F_PRECISION - 1)
#define F_TRUNC(x,y) { \
        F_TYPE orig_x = (x); \
        F_TYPE abs_x, t = F_POW_2(F_PRECISION - 1); \
        F_ABS(orig_x, abs_x); \
        ASSERT(abs_x < t); \
        (y) = abs_x + t; \
        (y) -= t; \
        if ((y) > abs_x) \
                (y) -= 1.0; \
        if (abs_x != orig_x) \
                F_NEGATE(y); \
}
#endif


#ifndef F_CVT_TO_WORD_CHOPPED
#undef  F_CVT_TO_WORD_CHOPPED_IS_FAST
#define F_CVT_TO_WORD_CHOPPED_PRECISION_LIMIT (BITS_PER_WORD - 1)
#define F_CVT_TO_WORD_CHOPPED(x,i) (i) = (WORD)(x)
#endif

#ifndef F_CVT_TO_WORD_ROUNDED
#undef  F_CVT_TO_WORD_ROUNDED_IS_FAST
#define F_CVT_TO_WORD_ROUNDED_PRECISION_LIMIT (F_PRECISION - 1)
#define F_CVT_TO_WORD_ROUNDED(x,i) { \
        U_WORD status_word; \
        F_TYPE y, t; \
        t = F_POW_2(F_PRECISION - 1); \
        ASSERT(DPML_DEBUG_ABS(x) < t); \
        F_COPY_SIGN(t, (x), t); \
        INIT_FPU_STATE_AND_ROUND_TO_NEAREST(status_word); \
        y = (x) + t; \
        RESTORE_FPU_STATE(status_word); \
        y -= t; \
        (i) = (WORD)y; \
}
#endif

#ifndef F_CVT_TO_WORD_ROUNDED_UP
#undef  F_CVT_TO_WORD_ROUNDED_UP_IS_FAST
#define F_CVT_TO_WORD_ROUNDED_UP_PRECISION_LIMIT (F_PRECISION - 1)
#define F_CVT_TO_WORD_ROUNDED_UP(x,i) { \
        F_TYPE y, t; \
        t = F_POW_2(F_PRECISION - 1); \
        ASSERT(DPML_DEBUG_ABS(x) < t); \
        F_COPY_SIGN(t, (x), t); \
        y = (x) + t; \
        y -= t; \
        if (y < x) \
                y += 1.0; \
        (i) = (WORD)y; \
}
#endif

#ifndef F_CVT_TO_WORD_ROUNDED_DOWN
#undef  F_CVT_TO_WORD_ROUNDED_DOWN_IS_FAST
#define F_CVT_TO_WORD_ROUNDED_DOWN_PRECISION_LIMIT (F_PRECISION - 1)
#define F_CVT_TO_WORD_ROUNDED_DOWN(x,i) { \
        F_TYPE y, t; \
        t = F_POW_2(F_PRECISION - 1); \
        ASSERT(DPML_DEBUG_ABS(x) < t); \
        F_COPY_SIGN(t, (x), t); \
        y = (x) + t; \
        y -= t; \
        if (y > x) \
                y -= 1.0; \
        (i) = (WORD)y; \
}
#endif



#if 0

These do not yet have generic definitions:

#define ARITH_SHIFT_WORD_RIGHT(i,j)
#define F_ADD_CHOPPED
#define F_ADD_ROUNDED_UP
#define F_ADD_ROUNDED_DOWN
#define F_MUL_CHOPPED
#define F_MUL_ROUNDED_UP
#define F_MUL_ROUNDED_DOWN

#endif



#ifndef EXT_MUL
#define EXT_MUL(i,j,lo,hi) { \
        WORD I = (i); \
        WORD J = (j); \
        U_WORD sign, i_neg, j_neg; \
        i_neg = (I < 0); \
        sign = i_neg; \
        if (i_neg) { I = ~((U_WORD)(I)) + 1; i_neg = (I < 0); } \
        j_neg = (J < 0);  \
        if (j_neg) { sign ^= 1; J = ~((U_WORD)J) + 1; j_neg = (J < 0); } \
        if (i_neg | j_neg) { \
                if (i_neg) { \
                        (lo) = (U_WORD)J << (BITS_PER_WORD - 1); \
                        (hi) = (U_WORD)J >> 1;  \
                } else { \
                        (lo) = (U_WORD)I << (BITS_PER_WORD - 1); \
                        (hi) = (U_WORD)I >> 1; \
                } \
        } else { \
                EXT_UMUL(I,J,(lo),(hi)); \
        } \
        if (sign) { \
                (lo) = ~((U_WORD)(lo)) + 1; \
                (hi) = ~((U_WORD)(hi)); \
                if (!lo) (hi) += 1; \
        } \
}
#endif



#ifndef EXT_MULH
#define EXT_MULH(i,j,hi) { \
        WORD lo; \
        EXT_MUL((i),(j),(lo),(hi)); \
}
#endif



#ifndef EXT_MUL1
#define EXT_MUL1(i,u1,u2) EXT_MUL((i),(u1),(u1),(u2))
#endif



#ifndef EXT_UMUL
#define EXT_UMUL(i,j,lo,hi) { \
        U_WORD i1, i2, j1, j2, p1, p2; \
        i2 = (U_WORD)(i) >> (BITS_PER_WORD / 2); \
        j2 = (U_WORD)(j) >> (BITS_PER_WORD / 2); \
        p2 = i2 * j2; \
        i1 = (U_WORD)((i) << (BITS_PER_WORD / 2)) >> (BITS_PER_WORD / 2); \
        p1 = i1 * j2; \
        j1 = (U_WORD)((j) << (BITS_PER_WORD / 2)) >> (BITS_PER_WORD / 2); \
        (lo) = i1 * j1; \
        (hi) = p2; \
        (hi) += (U_WORD)(p1 >> (BITS_PER_WORD / 2)); \
        ADD_AND_CARRY((p1 << (BITS_PER_WORD / 2)), (lo), (hi)); \
        p2 = i2 * j1; \
        (hi) += (U_WORD)(p2 >> (BITS_PER_WORD / 2)); \
        ADD_AND_CARRY((p2 << (BITS_PER_WORD / 2)), (lo), (hi)); \
}
#endif



#ifndef EXT_UMULH
#define EXT_UMULH(i,j,hi) { \
        U_WORD lo; \
        EXT_UMUL((i),(j),(lo),(hi)); \
}
#endif



#ifndef EXT_UMUL1
#define EXT_UMUL1(i,u1,u2) EXT_UMUL((i),(u1),(u1),(u2))
#endif



#ifndef EXT_UMUL2
#define EXT_UMUL2(i,u1,u2,u3) { \
        U_WORD c1, c2, i1, i2, j1, j2, j3, j4, p1, p2, p3; \
        i2 = (U_WORD)(i) >> (BITS_PER_WORD / 2); \
        j2 = (U_WORD)(u1) >> (BITS_PER_WORD / 2); \
        p2 = i2 * j2; \
        i1 = (U_WORD)((i) << (BITS_PER_WORD / 2)) >> (BITS_PER_WORD / 2); \
        j1 = (U_WORD)((u1) << (BITS_PER_WORD / 2)) >> (BITS_PER_WORD / 2); \
        j3 = (U_WORD)((u2) << (BITS_PER_WORD / 2)) >> (BITS_PER_WORD / 2); \
        j4 = (U_WORD)(u2) >> (BITS_PER_WORD / 2); \
        u2 = i1 * j3; \
        u3 = i2 * j4; \
        u1 = i1 * j1; \
        ADD_AND_CARRY(p2, u2, u3); \
        p1 = i1 * j2; \
        p2 = i2 * j1; \
        p1 += p2; \
        c1 = (p1 < p2); \
        p2 = i1 * j4; \
        p3 = i2 * j3; \
        p2 += p3; \
        c2 = (p2 < p3); \
        p2 += c1; \
        c1 = (p2 < c1); \
        c1 += c2; \
        u3 += (c1 << (BITS_PER_WORD / 2)); \
        ADD_AND_CARRY_2( (p1 << (BITS_PER_WORD / 2)), u1, u2, u3); \
        ADD_AND_CARRY( (p1 >> (BITS_PER_WORD / 2)), u2, u3); \
        ADD_AND_CARRY( (p2 << (BITS_PER_WORD / 2)), u2, u3); \
        u3 += (p2 >> (BITS_PER_WORD / 2)); \
}
#endif



#ifndef EXT_UMUL3
#define EXT_UMUL3(i,u1,u2,u3,u4) { \
        U_WORD c1, c2, c3, i1, i2, j1, j2, j3, j4, j5, j6, p1, p2, p3, p4; \
        i2 = (U_WORD)(i) >> (BITS_PER_WORD / 2); \
        j2 = (U_WORD)(u1) >> (BITS_PER_WORD / 2); \
        p2 = i2 * j2; \
        i1 = (U_WORD)((i) << (BITS_PER_WORD / 2)) >> (BITS_PER_WORD / 2); \
        j1 = (U_WORD)((u1) << (BITS_PER_WORD / 2)) >> (BITS_PER_WORD / 2); \
        j3 = (U_WORD)((u2) << (BITS_PER_WORD / 2)) >> (BITS_PER_WORD / 2); \
        p1 = i1 * j3; \
        j4 = (U_WORD)(u2) >> (BITS_PER_WORD / 2); \
        j5 = (U_WORD)((u3) << (BITS_PER_WORD / 2)) >> (BITS_PER_WORD / 2); \
        j6 = (U_WORD)(u3) >> (BITS_PER_WORD / 2); \
        u1 = i1 * j1; \
        u2 = p1; \
        u3 = i2 * j4; \
        p1 = i1 * j5; \
        u4 = i2 * j6; \
        ADD_AND_CARRY_2(p2, u2, u3, u4); \
        ADD_AND_CARRY(p1, u3, u4); \
        p1 = i1 * j2; \
        p2 = i2 * j1; \
        p1 += p2; \
        c1 = (p1 < p2); \
        p2 = i1 * j4; \
        p3 = i2 * j3; \
        p2 += p3; \
        c2 = (p2 < p3); \
        p3 = i1 * j6; \
        p4 = i2 * j5; \
        p3 += p4; \
        c3 = (p3 < p4); \
        p2 += c1; \
        c1 = (p2 < c1); \
        c2 += c1; \
        p3 += c2; \
        c2 = (p3 < c2); \
        c3 += c2; \
        u4 += (c3 << (BITS_PER_WORD / 2)); \
        ADD_AND_CARRY_3( (p1 << (BITS_PER_WORD / 2)), u1, u2, u3, u4); \
        ADD_AND_CARRY_2( (p1 >> (BITS_PER_WORD / 2)), u2, u3, u4); \
        ADD_AND_CARRY_2( (p2 << (BITS_PER_WORD / 2)), u2, u3, u4); \
        ADD_AND_CARRY( (p2 >> (BITS_PER_WORD / 2)), u3, u4); \
        ADD_AND_CARRY( (p3 << (BITS_PER_WORD / 2)), u3, u4); \
        u4 += (p3 >> (BITS_PER_WORD / 2)); \
}
#endif



#if (BITS_PER_WORD == 32) && !defined(UMUL32_64_BY_64_GIVING_96)
#define UMUL32_64_BY_64_GIVING_96(x0,x1,y0,y1,z1,z2,z3) { \
        U_WORD z0, c1, c2, c3, i1, i2, i3, i4, j1, j2, j3, j4, p1, p2, p3, p4; \
        i2 = (U_WORD)(x0) >> (BITS_PER_WORD / 2); \
        j2 = (U_WORD)(y0) >> (BITS_PER_WORD / 2); \
        p2 = i2 * j2; \
        i4 = (U_WORD)(x1) >> (BITS_PER_WORD / 2); \
        j4 = (U_WORD)(y1) >> (BITS_PER_WORD / 2); \
        i1 = (U_WORD)((x0) << (BITS_PER_WORD / 2)) >> (BITS_PER_WORD / 2); \
        j1 = (U_WORD)((y0) << (BITS_PER_WORD / 2)) >> (BITS_PER_WORD / 2); \
        p4 = i4 * j4; \
        i3 = (U_WORD)((x1) << (BITS_PER_WORD / 2)) >> (BITS_PER_WORD / 2); \
        j3 = (U_WORD)((y1) << (BITS_PER_WORD / 2)) >> (BITS_PER_WORD / 2); \
        z0 = i1 * j1; \
        z0 >> (BITS_PER_WORD / 2); \
        p1 = i1 * j2; \
        p1 += z0; \
        p1 >> (BITS_PER_WORD / 2); \
        z1 = i1 * j3; \
        z1 += p1; \
        p1 = i2 * j1; \
        p1 >> (BITS_PER_WORD / 2); \
        p2 += p1; \
        z1 += p2; \
        c1 = (z1 < p2); \
        p1 = i3 * j1; \
        z1 += p1; \
        c1 += (z1 < p1); \
        z2 = i2 * j4; \
        p1 = i3 * j3; \
        z2 += p1; \
        c2 = (z2 < p1); \
        p1 = i4 * j2; \
        z2 += p1; \
        c2 += (z2 < p1); \
        z2 += c1; \
        c2 += (z2 < c1); \
        z3 = p4 + c2; \
        p2 = i1 * j4; \
        p1 = i2 * j3; \
        p2 += p1; \
        c2 = (p2 < p1); \
        p1 = i3 * j2; \
        p2 += p1; \
        c2 += (p2 < p1); \
        p1 = i4 * j1; \
        p2 += p1; \
        c2 += (p2 < p1); \
        p3 = i3 * j4; \
        p1 = i4 * j3; \
        p3 += p1; \
        c3 = (p3 < p1); \
        p3 += c2; \
        c3 += (p3 < c2); \
        z3 += (c3 << (BITS_PER_WORD / 2)); \
        z3 += (p3 >> (BITS_PER_WORD / 2)); \
        ADD_AND_CARRY( (p3 << (BITS_PER_WORD / 2)), z2, z3); \
        ADD_AND_CARRY( (p2 >> (BITS_PER_WORD / 2)), z2, z3); \
        ADD_AND_CARRY_2( (p2 << (BITS_PER_WORD / 2)), z1, z2, z3); \
}
#endif



#ifndef ADD_AND_CARRY
#define ADD_AND_CARRY(i,u1,u2) { \
        U_WORD carry; \
        (u1) += (i); \
        carry = ((u1) < (i)); \
        (u2) += carry; \
}
#endif


#ifndef ADD_AND_CARRY_2
#define ADD_AND_CARRY_2(i,u1,u2,u3) { \
        U_WORD carry; \
        (u1) += (i); \
        carry = ((u1) < (i)); \
        (u2) += carry; \
        carry = ((u2) < carry); \
        (u3) += carry; \
}
#endif


#ifndef ADD_AND_CARRY_3
#define ADD_AND_CARRY_3(i,u1,u2,u3,u4) { \
        U_WORD carry; \
        (u1) += (i); \
        carry = ((u1) < (i)); \
        (u2) += carry; \
        carry = ((u2) < carry); \
        (u3) += carry; \
        carry = ((u3) < carry); \
        (u4) += carry; \
}
#endif



#ifndef U_MUL_BY_10
#define U_MUL_BY_10(i) { \
        (i) = (U_WORD)(i) + ((U_WORD)(i) << 2); \
        (i) = (U_WORD)(i) << 1; \
}
#endif

#ifndef LEFT_NORMALIZE_WORD
#define LEFT_NORMALIZE_WORD(i,j) { \
        (j) = 0; \
        while ((WORD)(i) > 0) { \
                (i) <<= 1; \
                (j) += 1; \
        } \
}
#endif

#ifndef SHIFT_WORD_LEFT
#define SHIFT_WORD_LEFT(shift, u) { \
        (u) <<= (shift); \
}
#endif

#ifndef SHIFT_2_WORDS_LEFT
#define SHIFT_2_WORDS_LEFT(shift, u1, u2) { \
        ASSERT((shift) != 0); \
        (u1) <<= (shift); \
        (u1) |= ((u2) >> (BITS_PER_WORD - (shift))); \
        (u2) <<= (shift); \
}
#endif

#ifndef SHIFT_3_WORDS_LEFT
#define SHIFT_3_WORDS_LEFT(shift, u1, u2, u3) { \
        ASSERT((shift) != 0); \
        (u1) <<= (shift); \
        (u1) |= ((u2) >> (BITS_PER_WORD - (shift))); \
        (u2) <<= (shift); \
        (u2) |= ((u3) >> (BITS_PER_WORD - (shift))); \
        (u3) <<= (shift); \
}
#endif

#ifndef SHIFT_4_WORDS_LEFT
#define SHIFT_4_WORDS_LEFT(shift, u1, u2, u3, u4) { \
        ASSERT((shift) != 0); \
        (u1) <<= (shift); \
        (u1) |= ((u2) >> (BITS_PER_WORD - (shift))); \
        (u2) <<= (shift); \
        (u2) |= ((u3) >> (BITS_PER_WORD - (shift))); \
        (u3) <<= (shift); \
        (u3) |= ((u4) >> (BITS_PER_WORD - (shift))); \
        (u4) <<= (shift); \
}
#endif

#ifndef SHIFT_WORD_RIGHT
#define SHIFT_WORD_RIGHT(shift, u) { \
        (u) >>= (shift); \
}
#endif

#ifndef SHIFT_2_WORDS_RIGHT
#define SHIFT_2_WORDS_RIGHT(shift, u1, u2) { \
        ASSERT((shift) != 0); \
        (u1) >>= (shift); \
        (u1) |= ((u2) << (BITS_PER_WORD - (shift))); \
        (u2) >>= (shift); \
}
#endif

#ifndef SHIFT_3_WORDS_RIGHT
#define SHIFT_3_WORDS_RIGHT(shift, u1, u2, u3) { \
        ASSERT((shift) != 0); \
        (u1) >>= (shift); \
        (u1) |= ((u2) << (BITS_PER_WORD - (shift))); \
        (u2) >>= (shift); \
        (u2) |= ((u3) << (BITS_PER_WORD - (shift))); \
        (u3) >>= (shift); \
}
#endif

#ifndef SHIFT_4_WORDS_RIGHT
#define SHIFT_4_WORDS_RIGHT(shift, u1, u2, u3, u4) { \
        ASSERT((shift) != 0); \
        (u1) >>= (shift); \
        (u1) |= ((u2) << (BITS_PER_WORD - (shift))); \
        (u2) >>= (shift); \
        (u2) |= ((u3) << (BITS_PER_WORD - (shift))); \
        (u3) >>= (shift); \
        (u3) |= ((u4) << (BITS_PER_WORD - (shift))); \
        (u4) >>= (shift); \
}
#endif




#ifndef D_GET_EXP_WORD
#define D_GET_EXP_WORD(x,exp_word) { \
        D_UNION u; \
        u.f = (x); \
        (exp_word) = u.D_HI_WORD; \
}
#endif

#ifndef GET_EXP_WORD
#define GET_EXP_WORD(x,exp_word) { \
        F_UNION u; \
        u.f = (x); \
        (exp_word) = u.F_HI_WORD; \
}
#endif


#ifndef D_PUT_EXP_WORD
#define D_PUT_EXP_WORD(x,exp_word) { \
        D_UNION u; \
        u.f = (x); \
        u.D_HI_WORD = (exp_word); \
        (x) = u.f; \
}
#endif

#ifndef PUT_EXP_WORD
#define PUT_EXP_WORD(x,exp_word) { \
        F_UNION u; \
        u.f = (x); \
        u.F_HI_WORD = (exp_word); \
        (x) = u.f; \
}
#endif


#ifndef GET_SIGN_WORD
#define GET_SIGN_WORD(x,sign_word) { \
        F_UNION u; \
        u.f = (x); \
        (sign_word) = u.F_HI_WORD; \
}
#endif

#ifndef PUT_SIGN_WORD
#define PUT_SIGN_WORD(x,sign_word) { \
        F_UNION u; \
        u.f = (x); \
        u.F_HI_WORD = (sign_word); \
        (x) = u.f; \
}
#endif

#ifndef GET_HI_FRAC_WORD
#define GET_HI_FRAC_WORD(x,hi_frac_word) { \
        F_UNION u; \
        u.f = (x); \
        (hi_frac_word) = u.F_HI_WORD; \
}
#endif

#ifndef PUT_HI_FRAC_WORD
#define PUT_HI_FRAC_WORD(x,hi_frac_word) { \
        F_UNION u; \
        u.f = (x); \
        u.F_HI_WORD = (hi_frac_word); \
        (x) = u.f; \
}
#endif

#ifndef GET_LO_FRAC_WORD
#define GET_LO_FRAC_WORD(x,lo_frac_word) { \
        F_UNION u; \
        u.f = (x); \
        (lo_frac_word) = u.F_LO_WORD; \
}
#endif

#ifndef PUT_LO_FRAC_WORD
#define PUT_LO_FRAC_WORD(x,lo_frac_word) { \
        F_UNION u; \
        u.f = (x); \
        u.F_LO_WORD = (lo_frac_word); \
        (x) = u.f; \
}
#endif


#ifndef GET_EXP_BITS
#define GET_EXP_BITS(x,mask,exp_bits) { \
        GET_EXP_WORD((x),(exp_bits)); \
        (exp_bits) &= (mask); \
}
#endif

#ifndef PUT_EXP_BITS
#define PUT_EXP_BITS(x,mask,exp_bits) { \
        F_UNION u; \
        u.f = (x); \
        u.F_HI_WORD &= ~(mask); \
        u.F_HI_WORD |= (exp_bits); \
        (x) = u.f; \
}
#endif

#ifndef D_PUT_EXP_BITS
#define D_PUT_EXP_BITS(x,mask,exp_bits) { \
        D_UNION u; \
        u.f = (x); \
        u.D_HI_WORD &= ~(mask); \
        u.D_HI_WORD |= (exp_bits); \
        (x) = u.f; \
}
#endif

#ifndef GET_EXP_FIELD
#define GET_EXP_FIELD(x,exp_field) { \
        GET_EXP_BITS((x),F_EXP_MASK,(exp_field)); \
}
#endif

#ifndef F_GET_EXP_FIELD
#define F_GET_EXP_FIELD(x, exp_word) { \
        F_UNION u; \
        u.f = (x); \
        (exp_word) = u.F_HI_WORD; \
        (exp_word) &= F_EXP_MASK; \
}
#endif

#ifndef B_GET_EXP_FIELD
#define B_GET_EXP_FIELD(x, exp_word) { \
        B_UNION u; \
        u.f = (x); \
        (exp_word) = u.B_HI_WORD; \
        (exp_word) &= B_EXP_MASK; \
}
#endif

#ifndef S_GET_EXP_FIELD
#define S_GET_EXP_FIELD(x, exp_word) { \
        S_UNION u; \
        u.f = (x); \
        (exp_word) = u.S_HI_WORD; \
        (exp_word) &= S_EXP_MASK; \
}
#endif

#ifndef D_GET_EXP_FIELD
#define D_GET_EXP_FIELD(x, exp_word) { \
        D_UNION u; \
        u.f = (x); \
        (exp_word) = u.D_HI_WORD; \
        (exp_word) &= D_EXP_MASK; \
}
#endif


#ifndef PUT_EXP_FIELD
#define PUT_EXP_FIELD(x,exp_field) { \
        PUT_EXP_BITS((x),F_EXP_MASK,(exp_field)); \
}
#endif


#ifndef ALIGN_W_EXP_FIELD
#define ALIGN_W_EXP_FIELD(w) ((U_WORD)(w) << F_EXP_POS)
#endif

#ifndef D_ALIGN_W_EXP_FIELD
#define D_ALIGN_W_EXP_FIELD(w) ((U_WORD)(w) << D_EXP_POS)
#endif

#ifndef B_ALIGN_W_EXP_FIELD
#define B_ALIGN_W_EXP_FIELD(w) ((U_WORD)(w) << B_EXP_POS)
#endif


#ifndef ALIGN_EXP_FIELD_W_WORD
#define ALIGN_EXP_FIELD_W_WORD(w) (((U_WORD)(w)) >> F_EXP_POS)
#endif

#ifndef D_ALIGN_EXP_FIELD_W_WORD
#define D_ALIGN_EXP_FIELD_W_WORD(w) (((U_WORD)(w)) >> D_EXP_POS)
#endif

#ifndef B_ALIGN_EXP_FIELD_W_WORD
#define B_ALIGN_EXP_FIELD_W_WORD(w) (((U_WORD)(w)) >> B_EXP_POS)
#endif


#ifndef GET_SIGN_EXP_FIELD
#define GET_SIGN_EXP_FIELD(x,sign_exp_field) { \
        GET_EXP_BITS((x),F_SIGN_EXP_MASK,(sign_exp_field)); \
}
#endif

#ifndef PUT_SIGN_EXP_FIELD
#define PUT_SIGN_EXP_FIELD(x,sign_exp_field) { \
        PUT_EXP_BITS((x),F_SIGN_EXP_MASK,(sign_exp_field)); \
}
#endif

#ifndef D_PUT_SIGN_EXP_FIELD
#define D_PUT_SIGN_EXP_FIELD(x,sign_exp_field) { \
        D_PUT_EXP_BITS((x),D_SIGN_EXP_MASK,(sign_exp_field)); \
}
#endif

#ifndef ADD_TO_EXP_WORD
#define ADD_TO_EXP_WORD(x,increment) { \
        F_UNION u; \
        u.f = (x); \
        u.F_HI_WORD += (increment); \
        (x) = u.f; \
}
#endif
#ifndef B_ADD_TO_EXP_WORD
#define B_ADD_TO_EXP_WORD(x,increment) { \
        B_UNION u; \
        u.f = (x); \
        u.B_HI_WORD += (increment); \
        (x) = u.f; \
}
#endif

#ifndef ADD_TO_EXP_FIELD
#define ADD_TO_EXP_FIELD(x,increment) { \
        ADD_TO_EXP_WORD((x),((U_WORD)(increment) << F_EXP_POS)); \
}
#endif
#ifndef B_ADD_TO_EXP_FIELD
#define B_ADD_TO_EXP_FIELD(x,increment) { \
        B_ADD_TO_EXP_WORD((x),((U_WORD)(increment) << B_EXP_POS)); \
}
#endif

#ifndef SUB_FROM_EXP_WORD
#define SUB_FROM_EXP_WORD(x,decrement) { \
        F_UNION u; \
        u.f = (x); \
        u.F_HI_WORD -= (decrement); \
        (x) = u.f; \
}
#endif

#ifndef SUB_FROM_EXP_FIELD
#define SUB_FROM_EXP_FIELD(x,decrement) { \
        SUB_FROM_EXP_WORD((x),((U_WORD)(decrement) << F_EXP_POS)); \
}
#endif

#ifndef SCALE_EXPONENT_BY_INT
#define SCALE_EXPONENT_BY_INT(x,increment) { \
        ADD_TO_EXP_FIELD((x),(increment)); \
}
#endif
#ifndef B_SCALE_EXPONENT_BY_INT
#define B_SCALE_EXPONENT_BY_INT(x,increment) { \
        B_ADD_TO_EXP_FIELD((x),(increment)); \
}
#endif

#ifndef SCALE_EXPONENT_BY_FLT
#define SCALE_EXPONENT_BY_FLT(x,increment) { \
        (x) *= F_POW_2(increment); \
}
#endif
#ifndef B_SCALE_EXPONENT_BY_FLT
#define B_SCALE_EXPONENT_BY_FLT(x,increment) { \
        (x) *= B_POW_2(increment); \
}
#endif


#if (SCALE_METHOD == by_int)

#ifndef SCALE_EXPONENT
#define SCALE_EXPONENT(x,increment) SCALE_EXPONENT_BY_INT((x),(increment))
#endif
#ifndef B_SCALE_EXPONENT
#   define B_SCALE_EXPONENT(x,increment) B_SCALE_EXPONENT_BY_INT((x),(increment))
#endif

#else  /* scale by float */

#ifndef SCALE_EXPONENT
#define SCALE_EXPONENT(x,increment) SCALE_EXPONENT_BY_FLT((x),(increment))
#endif
#ifndef B_SCALE_EXPONENT
#   define B_SCALE_EXPONENT(x,increment) B_SCALE_EXPONENT_BY_FLT((x),(increment))
#endif

#endif  /* SCALE_METHOD */


#ifndef CVT_TO_HI_LO_BY_FLT
#define CVT_TO_HI_LO_BY_FLT(x,big,y) { \
    F_TYPE t = (big); \
    F_COPY_SIGN(t, (x), t); \
    HI(y) = (x) + t; \
    HI(y) -= t; \
    LO(y) = (x) - HI(y); \
}
#endif


#ifndef CVT_TO_HI_LO_BY_FLT_SIGNED
#define CVT_TO_HI_LO_BY_FLT_SIGNED(x,big,y) { \
    HI(y) = (x) + (big); \
    HI(y) -= (big); \
    LO(y) = (x) - HI(y); \
}
#endif


#ifndef CVT_TO_HI_LO_BY_INT
#define CVT_TO_HI_LO_BY_INT(x,n,y) { \
        F_UNION u; \
        u.f = (x); \
        u.F_LO_WORD &= ~(PDP_SHUFFLE(MAKE_MASK((n), 0))); \
        HI(y) = u.f; \
        LO(y) = (x) - HI(y); \
}
#endif


#ifndef SPLIT_TO_HI_LO_BY_INT
#if ((F_PRECISION / 2) <= BITS_PER_WORD)
#define SPLIT_TO_HI_LO_BY_INT(x,y) { \
        F_UNION u; \
        u.f = (x); \
        u.F_LO_WORD &= ~(PDP_SHUFFLE(MAKE_MASK((F_PRECISION / 2), 0))); \
        HI(y) = u.f; \
        LO(y) = (x) - HI(y); \
}
#else
#define SPLIT_TO_HI_LO_BY_INT(x,y) { \
        F_UNION u; \
        u.f = (x); \
        u.F_LO3_WORD = 0; \
        u.F_LO2_WORD &= ~(PDP_SHUFFLE(MAKE_MASK(((F_PRECISION / 2) - BITS_PER_WORD), 0))); \
        HI(y) = u.f; \
        LO(y) = (x) - HI(y); \
}
#endif
#endif


#if PRECISION_BACKUP_AVAILABLE

#ifndef EXTENDED_MUL_SUB
#define EXTENDED_MUL_SUB(a,b,c,y) { \
        y = (B_TYPE)(a) - ( (B_TYPE)(b) * (B_TYPE)(c) ); \
}
#endif

#ifndef QUICK_EXTENDED_MUL_SUB
#define QUICK_EXTENDED_MUL_SUB(a,b,c,y) { \
        y = (B_TYPE)(a) - ( (B_TYPE)(b) * (B_TYPE)(c) ); \
}
#endif

#else  /* no PRECISION_BACKUP_AVAILABLE */

#ifndef EXTENDED_MUL_SUB
#define EXTENDED_MUL_SUB(a,b,c,y) { \
        y = ((((a \
                - HI(b) * HI(c)) \
                - HI(b) * LO(c)) \
                - LO(b) * HI(c)) \
                - LO(b) * LO(c)); \
}
#endif

#ifndef QUICK_EXTENDED_MUL_SUB
#define QUICK_EXTENDED_MUL_SUB(a,b,c,y) { \
        y = ((a \
                - b * HI(c)) \
                - b * LO(c)); \
}
#endif

#endif  /* PRECISION_BACKUP_AVAILABLE */


#if (QUAD_PRECISION) && !(defined(merced) && !defined(VMS))
#    define C_C_PROTO(n)          C_p_PROTO(n)
#    define COMPLEX_QUAD_DECL(n)  F_COMPLEX n
#    define COMPLEX_ARGS_INIT(x)  F_TYPE PASTE(r,x)=x->r, PASTE(i,x)=x->i
#    define COMPLEX_ARGS(x)       F_COMPLEX *x
#    define PASS_CMPLX(a,b,p)     ( p.r = a, p.i = b, (&p))
#    define COMPLEX_PROTOTYPE     F_COMPLEX *
#    define COMPLEX_B_PROTOTYPE   B_COMPLEX *
#elif defined(merced) && !defined(VMS)
#    define C_C_PROTO(n)          C_s_PROTO(n)
#    define COMPLEX_QUAD_DECL(n)  F_COMPLEX n
#    define COMPLEX_ARGS_INIT(x)  F_TYPE PASTE(r,x)=x.r, PASTE(i,x)=x.i
#    define COMPLEX_ARGS(x)       F_COMPLEX x 
#    define PASS_CMPLX(a,b,p)     (p.r = a, p.i = b, p) 
#    define COMPLEX_PROTOTYPE     F_COMPLEX 
#    define COMPLEX_B_PROTOTYPE   B_COMPLEX 
#else
#    define C_C_PROTO(n)          C_FF_PROTO(n)
#    define COMPLEX_QUAD_DECL(n)
#    define COMPLEX_ARGS_INIT(x)
#    define COMPLEX_ARGS(x)       F_TYPE PASTE(r,x), F_TYPE PASTE(i,x) 
#    define PASS_CMPLX(a,b,p)     (F_TYPE) a, (F_TYPE) b 
#    define COMPLEX_PROTOTYPE     F_TYPE, F_TYPE
#    define COMPLEX_B_PROTOTYPE   B_TYPE, B_TYPE
#endif


#ifndef S_RECEIVE_COMPLEX_RESULT
#    define     S_RECEIVE_COMPLEX_RESULT(a,b,f) \
                        { S_COMPLEX _t = f; a = _t.r; b = _t.i; }
#endif
#ifndef S_RETURN_COMPLEX_RESULT
#    define     S_RETURN_COMPLEX_RESULT(a,b) \
                                { S_COMPLEX _t; _t.r = a; _t.i = b; return _t; }
#endif
#ifndef D_RECEIVE_COMPLEX_RESULT
#    define     D_RECEIVE_COMPLEX_RESULT(a,b,f) \
                        { D_COMPLEX _t = f; a = _t.r; b = _t.i; }
#endif
#ifndef D_RETURN_COMPLEX_RESULT
#    define     D_RETURN_COMPLEX_RESULT(a,b) \
                                { D_COMPLEX _t; _t.r = a; _t.i = b; return _t; }
#endif
#ifndef Q_RECEIVE_COMPLEX_RESULT
#    define	Q_RECEIVE_COMPLEX_RESULT(a,b,f) \
				{ Q_COMPLEX _t = f; a = _t.r; b = _t.i; }
#endif
#ifndef Q_RETURN_COMPLEX_RESULT
#    define	Q_RETURN_COMPLEX_RESULT(a,b) \
				{ Q_COMPLEX _t; _t.r = a; _t.i = b; return _t; }
#endif



#ifndef RECEIVE_COMPLEX_RESULT
#    if defined(SINGLE_PRECISION)
#        define RECEIVE_COMPLEX_RESULT(a,b,f)   S_RECEIVE_COMPLEX_RESULT(a,b,f)
#    elif defined(DOUBLE_PRECISION)
#        define RECEIVE_COMPLEX_RESULT(a,b,f)   D_RECEIVE_COMPLEX_RESULT(a,b,f)
#    else
#        define RECEIVE_COMPLEX_RESULT(a,b,f)	Q_RECEIVE_COMPLEX_RESULT(a,b,f)
#    endif
#endif

#ifndef RETURN_COMPLEX_RESULT
#    if defined(SINGLE_PRECISION)
#        define RETURN_COMPLEX_RESULT(a,b)      S_RETURN_COMPLEX_RESULT(a,b)
#    elif defined(DOUBLE_PRECISION)
#        define RETURN_COMPLEX_RESULT(a,b)      D_RETURN_COMPLEX_RESULT(a,b)
#    else
#        define RETURN_COMPLEX_RESULT(a,b)      Q_RETURN_COMPLEX_RESULT(a,b)
#    endif
#endif


#ifndef ADD_SUB_BIG
#	define ADD_SUB_BIG(x,big) \
		(x) += (big); (x) -= (big)
#endif

#ifndef SHORTEN_VIA_CASTS
#	define SHORTEN_VIA_CASTS(in,out) \
		(out) = (F_TYPE)((R_TYPE)(in))
#endif

#ifndef ASSIGN_WITH_F_TYPE_PRECISION
#	define ASSIGN_WITH_F_TYPE_PRECISION(x,y) \
		(y) = (F_TYPE)(x)
#endif

/*
 * The following macros are use to scale denormalized values to normalized
 * results.  All scaling is done by an implicit multiplication by a power
 * of two.  The power of two used to scale the denormalized values is 
 * defined by the macro __LOG2_DENORM_SCALE, which defaults to F_PRECISION.
 * Based on __LOG2_DENORM_SCALE, three other constants are specified for 
 * convienence:
 *
 *	__DENORM_SCALE			   2^__LOG2_DENORM_SCALE
 *	__DENORM_SCALE_BIASED_EXP	   the aligned, biased and unbiased 
 *	__DENORM_SCALE_UNBIASED_EXP	     exponent field of __DENORM_SCALE
 *	__LOG2_DENORM_SCALE_ALIGNED_W_EXP  __LOG2_DENORM_SCALE aligned with
 *					     exponent field
 *
 * The technique used for scaling involves minipulataing the exponent field
 * of the value to be scaled.  Specifically, if x is denormalized value with
 * bit pattern:
 *
 *		+-+-----------+------------------------+
 *	x:	|s|000 ... 000|          F             |
 *		+-+-----------+------------------------+
 *
 * Then x = (-1)^s*2^F_MIN_BIN_EXP*2^F_NORM*[F/2^(P_PRECISION - 1)].  Define u
 * and v, to be a floating point numbers with the following bits patterns:
 *
 *		+-+-----------+------------------------+
 *	u:	|s|     E     |          F             |
 *		+-+-----------+------------------------+
 *
 *		+-+-----------+------------------------+
 *	v:	|s|     E     |          0             |
 *		+-+-----------+------------------------+
 *
 * I.e. u has the bit pattern of x, with the exponent field set to E and v
 * is u with the fraction field cleared.  It follows that u and v have values:
 *
 * 	u = (-1)^s*2^(E-F_EXP_BIAS)*2^F_NORM*[1 + F/2^(P_PRECISION - 1)]
 * 	v = (-1)^s*2^(E-F_EXP_BIAS)*2^F_NORM
 *
 * If z is defined as u - v, then
 *
 *	z = (-1)^s*2^(E-F_EXP_BIAS)*2^F_NORM*[F/2^(P_PRECISION - 1)]
 *	  = 2^*(E-F_EXP_BIAS-F_MIN_BIN_EXP)*
 *             (-1)^s*2^F_MIN_BIN_EXP*2^F_NORM*[F/2^(P_PRECISION - 1)]
 *	  = 2^*(E-F_EXP_BIAS-F_MIN_BIN_EXP)*x
 *
 * I.e. z is x scaled up by 2^e, where e = E - F_EXP_BIAS - F_MIN_BIN_EXP.  In
 * the macros below, specifying __LOG2_DENORM_SCALE is equivalent to specifying
 * e in the above discussion.
 */

#if !defined(__LOG2_DENORM_SCALE)
#   if F_COPY_SIGN_AND_EXP_IS_FAST
#       define __LOG2_DENORM_SCALE		(F_PRECISION - F_MIN_BIN_EXP)
#   else
#       define __LOG2_DENORM_SCALE		F_PRECISION
#   endif
#endif

#undef  __DENORM_SCALE_UNBIASED_EXP
#define __DENORM_SCALE_UNBIASED_EXP	ALIGN_W_EXP_FIELD(__LOG2_DENORM_SCALE \
					  - F_NORM)
#undef  __DENORM_SCALE_BIASED_EXP
#define __DENORM_SCALE_BIASED_EXP	ALIGN_W_EXP_FIELD(__LOG2_DENORM_SCALE \
					  - F_NORM + F_EXP_BIAS)

#undef  __LOG2_DENORM_SCALE_ALIGNED_W_EXP
#define __LOG2_DENORM_SCALE_ALIGNED_W_EXP \
					ALIGN_W_EXP_FIELD(__LOG2_DENORM_SCALE)

#define	__LOG2_DENORM_CONST		(__LOG2_DENORM_SCALE + F_NORM + \
					   F_MIN_BIN_EXP)
#define	__DENORM_CONST_BIASED_EXP	ALIGN_W_EXP_FIELD(__LOG2_DENORM_CONST \
					  - F_NORM + F_EXP_BIAS)

#if F_COPY_SIGN_AND_EXP_IS_FAST && \
     (__LOG2_DENORM_CONST >= 0) && (__LOG2_DENORM_CONST <= __MAX_F_POW_2_EXP)

#   undef  __DENORM_CONST
#   define __DENORM_CONST	(F_TYPE) F_POW_2(__LOG2_DENORM_CONST)

#   if defined(__NEED_SIGNED_DENORM_TO_NORM)
#       define DENORM_TO_NORM(p,q) \
				{ \
				F_TYPE __denorm_const; \
				F_COPY_SIGN(__DENORM_CONST,p,__denorm_const); \
				F_COPY_SIGN_AND_EXP(p, __denorm_const, q); \
                                q -= __denorm_const; \
				}
#   else
#       define DENORM_TO_NORM(p,q) \
				{ \
				F_COPY_SIGN_AND_EXP(p, __DENORM_CONST, q); \
                                q -= __DENORM_CONST; \
				}
#   endif

#   define DENORM_TO_NORM_AND_EXP(p,e,q)  \
				{ DENORM_TO_NORM(p,q); GET_EXP_FIELD(q,e) }

#else

#   define __DENORM_TO_NORM_EXP	ALIGN_W_EXP_FIELD(__LOG2_DENORM_SCALE + \
				  F_NORM + F_EXP_BIAS + F_MIN_BIN_EXP)
#   define __DENORM_TO_NORM(p,q) \
				F_UNION u; \
				u.f = p; \
				u.F_HI_WORD = (u.F_HI_WORD & ~F_EXP_MASK) | \
				  __DENORM_TO_NORM_EXP; \
				q = u.f; \
				u.F_HI_WORD &= F_SIGN_EXP_MASK; \
				CLEAR_LOW_BITS(u); \
                                q -= u.f

#   define DENORM_TO_NORM(p,q)	{ __DENORM_TO_NORM(p,q); }

#   define DENORM_TO_NORM_AND_EXP(p,e,q)  \
				{ \
				__DENORM_TO_NORM(p,q); \
				u.f = q; \
				e = u.F_HI_WORD & F_EXP_MASK; \
                                }

#endif

/*
 * The following macros support extended precision multiplication of a sequence
 * of unsigned HALF_WORDs.  The basic operation is an extended integer multiply
 * and add.  It has four inputs and three results.  The inputs are an addend
 * in hi and lo parts (w_hi, w_lo), the carry in from the previous operation,
 * c_in, and the multiplier and multiplicand F and g.  The three outputs are
 * the carry out, c_out, and the hi and lo digits of the sum, z_hi and z_lo.
 * Letting B = 2^BITS_PER_WORD, the basic operation is
 *
 *	c_out*B^2 + z_hi*B + z_lo <== (w_hi*B + w_lo) + c_in*B + F*g
 *
 * The are 6 different macros, one for the basic operation and 5 special
 * cases.  E.g. ignore the carry out or carry is zero.
 *
 * They macros are defined as a group in order to be consistent.  If
 * BITS_PER_DIGIT is defined, it is assumed that the arithmetic macros have
 * been in one of the architecture specific include files.
 */

#if !defined(BITS_PER_DIGIT)

#   define BITS_PER_DIGIT	BITS_PER_HALF_WORD
#   define DIGIT_TYPE		PASTE_2(U_INT_, BITS_PER_DIGIT)
#   define SIGNED_DIGIT_TYPE	PASTE_2(INT_, BITS_PER_DIGIT)


#   define XMUL_XADDC_W_C_IN(F, g, w_hi, w_lo, c_in, c_out, z_hi, z_lo) \
		{ \
		U_WORD prod, addend, t; \
		\
		prod = ((U_WORD) F)*((U_WORD) g); \
		addend = ((U_WORD)w_hi << BITS_PER_DIGIT) + (U_WORD) w_lo; \
		t = (U_WORD) c_in << BITS_PER_DIGIT; \
		prod += t; /* no carry out possible */ \
		prod += addend; \
		c_out = (prod < addend); \
		z_hi = prod >> BITS_PER_DIGIT; \
		z_lo = prod & MAKE_MASK(BITS_PER_DIGIT, 0); \
		}

#   define XMUL_XADD_W_C_IN(F, g, w_hi, w_lo, c_in, z_hi, z_lo) \
		{ \
		U_WORD prod, addend, t; \
		\
		prod = ((U_WORD) F)*((U_WORD) g); \
		addend = ((U_WORD) w_hi << BITS_PER_DIGIT) + (U_WORD) w_lo; \
		t = (U_WORD) c_in << BITS_PER_DIGIT; \
		prod += t; /* no carry out possible */ \
		prod += addend; \
		z_hi = prod >> BITS_PER_DIGIT; \
		z_lo = prod & MAKE_MASK(BITS_PER_DIGIT, 0); \
		}

#   define XMUL_XADDC(F, g, w_hi, w_lo, c_out, z_hi, z_lo) \
		{ \
		U_WORD prod, addend; \
		\
		prod = ((U_WORD) F)*((U_WORD) g); \
		addend = ((U_WORD) w_hi << BITS_PER_DIGIT) + (U_WORD) w_lo; \
		prod += addend; \
		c_out = (prod < addend); \
		z_hi = prod >> BITS_PER_DIGIT; \
		z_lo = prod & MAKE_MASK(BITS_PER_DIGIT, 0); \
		}

#   define XMUL_XADD(F, g, w_hi, w_lo, z_hi, z_lo) \
		{ \
		U_WORD prod, addend; \
		\
		prod = ((U_WORD) F)*((U_WORD) g); \
		addend = ((U_WORD) w_hi << BITS_PER_DIGIT) + (U_WORD) w_lo; \
		prod += addend; \
		z_hi = prod >> BITS_PER_DIGIT; \
		z_lo = prod & MAKE_MASK(BITS_PER_DIGIT, 0); \
		}

#   define XMUL_ADD(F, g, w_lo, z_hi, z_lo) \
		{ \
		U_WORD prod; \
		\
		prod = ((U_WORD) F)*((U_WORD) g); \
		prod += (U_WORD) w_lo; \
		z_hi = prod >> BITS_PER_DIGIT; \
		z_lo = prod & MAKE_MASK(BITS_PER_DIGIT, 0); \
		}

#   define MUL_ADD(F, g, w_lo, z_lo)	z_lo = F*g + w_lo
    
#   define XMUL(F, g, z_hi, z_lo) \
		{ \
		U_WORD prod; \
		\
		prod = ((U_WORD) F)*((U_WORD) g); \
		z_hi = prod >> BITS_PER_DIGIT; \
		z_lo = prod & MAKE_MASK(BITS_PER_DIGIT, 0); \
		}

#endif /* !defined(BITS_PER_DIGIT) */

/*
** It is occasionally useful to access the high or low 32 bits of a double
** precison as a 32 bit integer.  Unfortunately, for some architectures,
** (notably, alpha ev6) this can result in a memory access trap cause by
** writing 32 bits and then trying to read 64 bits from the same location.
** To work around this problem, we define the "load/store" integer type and
** appropriate macros.
*/

#if defined(HAS_LOAD_WRONG_STORE_SIZE_PENALTY)
#   define BITS_PER_LS_INT_TYPE         BITS_PER_WORD
#   define LS_INT_TYPE                  WORD
#   define U_LS_INT_TYPE                U_WORD
#   define B_HI_LS_INT_TYPE             B_SIGNED_HI_WORD
#else
#   define BITS_PER_LS_INT_TYPE         BITS_PER_INT
#   define LS_INT_TYPE                  INT_32
#   define U_LS_INT_TYPE                U_INT_32
#   define B_HI_LS_INT_TYPE             B_SIGNED_HI_32
#endif

/*
**  For platforms that have hardware SQRT instructions available (e.g., EV6),
**  the performance of some DPML functions may be improved by replacing a call
**  to (or the inlining of) the SQRT function with the equivalent hardware
**  instruction.
*/

#if IEEE_FLOATING
#   define S_HW_SQRT_NAME(x) __SQRTS(x)
#   define D_HW_SQRT_NAME(x) __SQRTT(x)
#elif VAX_FLOATING
#   define S_HW_SQRT_NAME(x) __SQRTF(x)
#   define D_HW_SQRT_NAME(x) __SQRTG(x)
#endif

#define S_HW_SQRT(x,y) (y = S_HW_SQRT_NAME(x))
#define D_HW_SQRT(x,y) (y = D_HW_SQRT_NAME(x))

#if SINGLE_PRECISION
#   define F_HW_SQRT_NAME S_HW_SQRT_NAME
#   define B_HW_SQRT_NAME D_HW_SQRT_NAME
#   define F_HW_SQRT S_HW_SQRT
#   define B_HW_SQRT D_HW_SQRT
#elif DOUBLE_PRECISION
#   define F_HW_SQRT_NAME D_HW_SQRT_NAME
#   define B_HW_SQRT_NAME D_HW_SQRT_NAME
#   define F_HW_SQRT D_HW_SQRT
#   define B_HW_SQRT D_HW_SQRT
#else
#   define F_HW_SQRT_NAME F_SQRT_NAME
#   define B_HW_SQRT_NAME B_SQRT_NAME
#   define F_HW_SQRT F_SQRT
#   define B_HW_SQRT B_SQRT
#endif

#if defined(HAS_SQRT_INSTRUCTION)
#   define F_HW_OR_SW_SQRT_NAME F_HW_SQRT_NAME
#   define B_HW_OR_SW_SQRT_NAME B_HW_SQRT_NAME
#   define F_HW_OR_SW_SQRT F_HW_SQRT
#   define B_HW_OR_SW_SQRT B_HW_SQRT
#else
#   define F_HW_OR_SW_SQRT_NAME F_SQRT_NAME
#   define B_HW_OR_SW_SQRT_NAME B_SQRT_NAME
#   define F_HW_OR_SW_SQRT F_SQRT
#   define B_HW_OR_SW_SQRT B_SQRT
#endif

/* F_HW_OR_SW_PRECISE_SQRT is defined for hypot to use
** F_PRECISE_SQRT which is defined in sqrt_macros.h.
** Both F_PRECISE_SQRT and F_HW_OR_SW_PRECISE_SQRT are 
** used only in dpml_hypot.c
*/

#if defined(HAS_SQRT_INSTRUCTION)
#   define F_HW_OR_SW_PRECISE_SQRT F_HW_SQRT
#else
#   define F_HW_OR_SW_PRECISE_SQRT F_PRECISE_SQRT
# endif

#if defined GROUP 
#   define D_GROUP(x)   GROUP(x) 
#else        
#   define D_GROUP_NAME         PASTE_2(__INTERNAL_NAME(group),_d)
    extern double D_GROUP_NAME( double );
#   define D_GROUP(x)   D_GROUP_NAME(x)
#endif 

#endif  /* DPML_PRIVATE_H */

