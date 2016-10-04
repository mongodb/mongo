// bid_trans.h
// ============================================================================
/*
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
*/
// =============================================================================
//
// Abstract:
// ---------
//
// This file contains the macro definitions to allow the bid transcendental
// functions to be build in one of four ways depending on how the 80 and 128
// bit floating point types are supported. The f80 type can be supported
// as either a native compiler f80 type, a native compiler F128 bit type or
// emulated. The f128 type can be supported as either an native compiler f128
// type or emulated. The support method is determined by the values of the
// preprocessor symbols USE_COMPILER_F80_TYPE and USE_COMPILER_F128_TYPE as
// follows:
//
//	+-------------+------------+-----------+------------+
//	|    f80      |    f128    | *F80_TYPE | *F128_TYPE | 
//	+-------------+------------+-----------+------------+
//	| native f80  | native 128 |     1     |     1      |
//	| native f80  | emulated   |     1     |     0      |
//      | native f128 | native 128 |     0     |     1      |
//      | emulated    | emulated   |     0     |     0      |
//	+-------------+------------+------------------------+
//
// Edit History:
// -------------
//
// 1-0001 Initial version. RNH 31-Aug-2010
//
// =============================================================================

#include "bid_internal.h"

// =============================================================================
// Get the default setting for the F80 and F128 support
// =============================================================================

#if !defined USE_COMPILER_F128_TYPE
#   define USE_COMPILER_F128_TYPE	0
#elif USE_COMPILER_F128_TYPE != 0
#   undef  USE_COMPILER_F128_TYPE
#   define USE_COMPILER_F128_TYPE	1
#endif

#if !defined USE_COMPILER_F80_TYPE
#   define USE_COMPILER_F80_TYPE	0
#elif USE_COMPILER_F80_TYPE != 0
#   undef  USE_COMPILER_F80_TYPE
#   define USE_COMPILER_F80_TYPE	1
#endif

// =============================================================================
// Based on the evaluation method, define the basic data types
// =============================================================================

#if !USE_COMPILER_F128_TYPE
#   define BID_F128_TYPE	BID_UINT128
#else
#   if defined __INTEL_COMPILER
#       define  BID_F128_TYPE _Quad    
#   else
#       error "128-bit floating point type for this compiler is unknown"
#   endif
#endif

#if !USE_COMPILER_F80_TYPE
#   define BID_F80_TYPE		BID_F128_TYPE
#else
#   if defined __INTEL_COMPILER
#       define  BID_F80_TYPE long double    
#   else
#       error "80-bit floating point type for this compiler is unknown"
#   endif
#endif

// =============================================================================
// Support macros for token pasting. These should be moved to a more general
// locaiton
// =============================================================================

#define GLUE(a,b)	a ## b
#define PASTE(a,b)	GLUE(a,b)
#define PASTE2(a,b)	PASTE(a,b)
#define PASTE3(a,b,c)	PASTE2(a,PASTE2(b,c))

// =============================================================================
// Data structures and macros used to initial f80 or f128 constants. The
// constants are specified via their f128 hex encoding and the macros will
// convert the f128 format to the f80 format as required. Also, these macros
// take care of endian issues
// =============================================================================

typedef union {
    BID_UINT64 w[2];
    BID_F128_TYPE v;
    } BID_F128_CONST;

typedef union BID_ALIGN (16)
     {
     BID_UINT64 w[2];
     BID_F80_TYPE v;
     } BID_F80_CONST;

#define SUF64		ull
#define HEX64(a)	PASTE3(0x,a,SUF64)

#if BID_BIG_ENDIAN
#   define ENDIAN128(hi,lo)	hi , lo
#   define HI			0
#   define LO			1
#define BID128_LH_INIT(lo,hi)	{ hi, lo }
#else
#   define ENDIAN128(hi,lo)	lo , hi
#   define HI			1
#   define LO			0
#define BID128_LH_INIT(lo,hi)	{ lo, hi }
#endif

#define F128_TO_F80_HI(hi,lo)	_F128_TO_F80_HI(HEX64(hi),HEX64(lo))
#define F128_TO_F80_LO(hi,lo)	_F128_TO_F80_LO(HEX64(hi),HEX64(lo))
#define _F128_TO_F80_HI(hi,lo)	(hi >> 48)
#define _F128_TO_F80_LO(hi,lo)	(((hi << 15) | HEX64(8000000000000000) | (lo >> 49)) + ((lo >> 48) & 1))

#define BID_INIT_F128(hi,lo)	{ ENDIAN128( HEX64(hi), HEX64(lo)) }
#if USE_COMPILER_F80_TYPE
#    define BID_INIT_F80(hi,lo)	{ ENDIAN128( F128_TO_F80_HI(hi,lo), F128_TO_F80_LO(hi,lo)) } 
#else
#    define BID_INIT_F80(hi,lo)	BID_INIT_F128(hi, lo)
#endif

#define BID_F128_CONST_DEF(name,hi,lo)	static const BID_F128_CONST name = \
                                                            BID_INIT_F128(hi,lo)
#define BID_F80_CONST_DEF(name,hi,lo)	static const BID_F80_CONST name = \
                                                            BID_INIT_F80(hi,lo)

// =============================================================================
// The following macros are used to switch the bid function invocation macros
// between compiler supported f128 arithment and function calls and emulated
// arithmetic
// =============================================================================

#if USE_COMPILER_F128_TYPE

#   define __BID_F128_NAME(name)		__ ## name ## q
#   define __BID_F128_CMP(a,b,op,mask)		((a) op (b))
#   define __BID_F128_F_I_OP(r,a,op,name)	((r) = (op (a)))
#   define __BID_F128_F_F_OP(r,a,op,name)	((r) = (op (a)))
#   define __BID_F128_F_FF_OP(r,a,b,op,name)	((r) = ((a) op (b)))
#   define __BID_F128_F_F_FUNC(r,a,name)	((r) = __BID_F128_NAME(name)(a))
#   define __BID_F128_F_FF_FUNC(r,a,b,name)	((r) = __BID_F128_NAME(name)(a,b))
#   define __BID_F128_F_F_DECL(name)		extern BID_F128_TYPE __BID_F128_NAME(name) ( BID_F128_TYPE )
#   define __BID_F128_F_FF_DECL(name)		extern BID_F128_TYPE __BID_F128_NAME(name) ( BID_F128_TYPE, BID_F128_TYPE )
#   define BID_F128_ASSIGN(name,con)		name = con.v


#else

#   define __b(x)				((BID_F128_TYPE *) &(x))
#   define __BID_F128_NAME(name)		bid_f128_ ## name
#   define __BID_F128_CMP(a,b,op,mask)		(bid_f128_cmp(__b(a), __b(b), (mask)))
#   define __BID_F128_F_I_OP(r,a,op,name)	__BID_F128_NAME(name)( __b(r), a)
#   define __BID_F128_F_F_OP(r,a,op,name)	__BID_F128_NAME(name)( __b(r), __b(a))
#   define __BID_F128_F_FF_OP(r,a,b,op,name)	__BID_F128_NAME(name)( __b(r), __b(a), __b(b))
#   define __BID_F128_F_F_FUNC(r,a,name)	__BID_F128_NAME(name)( __b(r), __b(a))
#   define __BID_F128_F_FF_FUNC(r,a,b,name)	__BID_F128_NAME(name)( __b(r), __b(a), __b(b))
#   define __BID_F128_F_F_DECL(name)		extern void __BID_F128_NAME(name)( BID_F128_TYPE*, BID_F128_TYPE*)
#   define __BID_F128_F_FF_DECL(name)		extern void __BID_F128_NAME(name)( BID_F128_TYPE*, BID_F128_TYPE*, BID_F128_TYPE*)
#   define BID_F128_ASSIGN(name,con)		name.w[HI] = con.w[HI]; name.w[LO] = con.w[LO]
#endif

// =============================================================================
// The F128 function macros
// =============================================================================

#define __bid_f128_lt(a, b)		__BID_F128_CMP( a, b, <,  1) 
#define __bid_f128_eq(a, b)		__BID_F128_CMP( a, b, ==, 2) 
#define __bid_f128_le(a, b)		__BID_F128_CMP( a, b, <=, 3)   
#define __bid_f128_gt(a, b)		__BID_F128_CMP( a, b, >,  4)        
#define __bid_f128_ne(a, b)		__BID_F128_CMP( a, b, !=, 5)
#define __bid_f128_ge(a, b)		__BID_F128_CMP( a, b, >=, 6)

#define __bid_f128_neg(res, a)		__BID_F128_F_F_OP(res,a,-, neg)
#define __bid_f128_itof(res, a)		__BID_F128_F_I_OP(res,a,(_Quad),itof)

#define __bid_f128_add(res, a, b)	__BID_F128_F_FF_OP(res,a,b,+,add)
#define __bid_f128_div(res, a, b)	__BID_F128_F_FF_OP(res,a,b,/,div)
#define __bid_f128_sub(res, a, b)	__BID_F128_F_FF_OP(res,a,b,-,sub)
#define __bid_f128_mul(res, a, b)	__BID_F128_F_FF_OP(res,a,b,*,mul)

#define __bid_f128_acos(res, a)         __BID_F128_F_F_FUNC(res, a, acos)
#define __bid_f128_acosh(res, a)        __BID_F128_F_F_FUNC(res, a, acosh)
#define __bid_f128_asin(res, a)         __BID_F128_F_F_FUNC(res, a, asin)
#define __bid_f128_asinh(res, a)        __BID_F128_F_F_FUNC(res, a, asinh)
#define __bid_f128_atan(res, a)         __BID_F128_F_F_FUNC(res, a, atan)
#define __bid_f128_cbrt(res, a)         __BID_F128_F_F_FUNC(res, a, cbrt)
#define __bid_f128_cos(res, a)          __BID_F128_F_F_FUNC(res, a, cos)
#define __bid_f128_cosh(res, a)         __BID_F128_F_F_FUNC(res, a, cosh)
#define __bid_f128_erf(res, a)          __BID_F128_F_F_FUNC(res, a, erf)
#define __bid_f128_erfc(res, a)         __BID_F128_F_F_FUNC(res, a, erfc)
#define __bid_f128_exp(res, a)          __BID_F128_F_F_FUNC(res, a, exp)
#define __bid_f128_exp10(res, a)        __BID_F128_F_F_FUNC(res, a, exp10)
#define __bid_f128_exp2(res, a)         __BID_F128_F_F_FUNC(res, a, exp2)
#define __bid_f128_expm1(res, a)        __BID_F128_F_F_FUNC(res, a, expm1)
#define __bid_f128_fabs(res, a)         __BID_F128_F_F_FUNC(res, a, fabs)
#define __bid_f128_lgamma(res, a)       __BID_F128_F_F_FUNC(res, a, lgamma)
#define __bid_f128_log(res, a)          __BID_F128_F_F_FUNC(res, a, log)
#define __bid_f128_log1p(res, a)        __BID_F128_F_F_FUNC(res, a, log1p)
#define __bid_f128_log2(res, a)         __BID_F128_F_F_FUNC(res, a, log2)
#define __bid_f128_sin(res, a)          __BID_F128_F_F_FUNC(res, a, sin)
#define __bid_f128_sinh(res, a)         __BID_F128_F_F_FUNC(res, a, sinh)
#define __bid_f128_sqrt(res, a)         __BID_F128_F_F_FUNC(res, a, sqrt)
#define __bid_f128_tan(res, a)          __BID_F128_F_F_FUNC(res, a, tan)
#define __bid_f128_tanh(res, a)         __BID_F128_F_F_FUNC(res, a, tanh)

#define __bid_f128_hypot(res, a, b)     __BID_F128_F_FF_FUNC(res, a, b, hypot)
#define __bid_f128_nextafter(res, a, b) __BID_F128_F_FF_FUNC(res, a, b, nextafter)

// =============================================================================
// The f128 function declarations
// =============================================================================

#if !USE_COMPILER_F128_TYPE
    extern int bid_f128_cmp( BID_UINT128*, BID_UINT128*, int );
    extern void bid_f128_itof( BID_UINT128*, int );

    __BID_F128_F_FF_DECL(add);
    __BID_F128_F_FF_DECL(div);
    __BID_F128_F_FF_DECL(sub);
    __BID_F128_F_FF_DECL(mul);
    __BID_F128_F_F_DECL(neg);
#endif

__BID_F128_F_F_DECL(acos);
__BID_F128_F_F_DECL(acosh);
__BID_F128_F_F_DECL(asin);
__BID_F128_F_F_DECL(asinh);
__BID_F128_F_F_DECL(atan);
__BID_F128_F_F_DECL(cbrt);
__BID_F128_F_F_DECL(cos);
__BID_F128_F_F_DECL(cosh);
__BID_F128_F_F_DECL(erf);
__BID_F128_F_F_DECL(erfc);
__BID_F128_F_F_DECL(exp);
__BID_F128_F_F_DECL(exp10);
__BID_F128_F_F_DECL(exp2);
__BID_F128_F_F_DECL(expm1);
__BID_F128_F_F_DECL(fabs);
__BID_F128_F_F_DECL(lgamma);
__BID_F128_F_F_DECL(log);
__BID_F128_F_F_DECL(log1p);
__BID_F128_F_F_DECL(log10);
__BID_F128_F_F_DECL(log2);
__BID_F128_F_F_DECL(sin);
__BID_F128_F_F_DECL(sinh);
__BID_F128_F_F_DECL(sqrt);
__BID_F128_F_F_DECL(tan);
__BID_F128_F_F_DECL(tanh);
__BID_F128_F_F_DECL(tgamma);

__BID_F128_F_FF_DECL(atan2);
__BID_F128_F_FF_DECL(hypot);
__BID_F128_F_FF_DECL(nextafter);

// =============================================================================
// The following macros are used to switch the bid function invocation macros
// between compiler supported f80 arithment and function calls and emulated
// arithmetic
// =============================================================================

#if USE_COMPILER_F80_TYPE

#   define __BID_F80_NAME(name)			name ## l
#   define __BID_F80_CMP(a,b,op,mask)		((a) op (b))
#   define __BID_F80_F_I_OP(r,a,op,name)	((r) = (op (a)))
#   define __BID_F80_F_F_OP(r,a,op,name)	((r) = (op (a)))
#   define __BID_F80_F_FF_OP(r,a,b,op,name)	((r) = ((a) op (b)))
#   define __BID_F80_F_F_FUNC(r,a,name)		((r) = __BID_F80_NAME(name)(a))
#   define __BID_F80_F_FF_FUNC(r,a,b,name)	((r) = __BID_F80_NAME(name)(a,b))
#   define __BID_F80_F_F_DECL(name)		extern BID_F80_TYPE __BID_F80_NAME(name) ( BID_F80_TYPE )
#   define __BID_F80_F_FF_DECL(name)		extern BID_F80_TYPE __BID_F80_NAME(name) ( BID_F80_TYPE, BID_F80_TYPE )
#   define BID_F80_ASSIGN(name,con)		name = con.v
#   define BID_F80_PACK_TRIG(t,sf,ef,p)		t.w[HI] = (((BID_UINT64)(sf)) << 15) | (ef); t.w[LO] = (p)

#else

#   define __BID_F80_NAME(name)			__BID_F128_NAME(name)
#   define __BID_F80_CMP(a,b,op,mask)		__BID_F128_CMP(a,b,op,mask)
#   define __BID_F80_F_I_OP(r,a,op,name)	__BID_F128_F_I_OP(r,a,op,name)
#   define __BID_F80_F_F_OP(r,a,op,name)	__BID_F128_F_F_OP(r,a,op,name)
#   define __BID_F80_F_FF_OP(r,a,b,op,name)	__BID_F128_F_FF_OP(r,a,b,op,name)
#   define __BID_F80_F_F_FUNC(r,a,name)		__BID_F128_F_F_FUNC(r,a,name)
#   define __BID_F80_F_FF_FUNC(r,a,b,name)	__BID_F128_F_FF_FUNC(r,a,b,name)
#   define __BID_F80_F_F_DECL(name)		__BID_F128_F_F_DECL(name)
#   define __BID_F80_F_FF_DECL(name)		__BID_F128_F_FF_DECL(name)
#   define BID_F80_ASSIGN(name,con)		BID_F128_ASSIGN(name,con)
#   define BID_F80_PACK_TRIG(t,sf,ef,p)		t.w[HI] = (((((BID_UINT64)(sf)) << 15) | (ef)) << 48) | (((p) << 1) >> 16); \
                                                t.w[LO]  = ((p) << 49)
#   undef  binary80_to_bid64
#   undef  bid64_to_binary80
#   define binary80_to_bid64			binary128_to_bid64
#   define bid64_to_binary80			bid64_to_binary128

#endif

// =============================================================================
// The f80 function definitions
// =============================================================================

#define __bid_f80_lt(a, b)		__BID_F80_CMP( a, b, <,  1) 
#define __bid_f80_eq(a, b)		__BID_F80_CMP( a, b, ==, 2) 
#define __bid_f80_le(a, b)		__BID_F80_CMP( a, b, <=, 3)   
#define __bid_f80_gt(a, b)		__BID_F80_CMP( a, b, >,  4)        
#define __bid_f80_ne(a, b)		__BID_F80_CMP( a, b, !=, 5)
#define __bid_f80_ge(a, b)		__BID_F80_CMP( a, b, >=, 6)

#define __bid_f80_neg(res, a)   	__BID_F80_F_F_OP(res,a,-, neg)
#define __bid_f80_itof(res, a)		__BID_F80_F_I_OP(res,a,(_Quad),itof)

#define __bid_f80_add(res, a, b)	__BID_F80_F_FF_OP(res,a,b,+,add)
#define __bid_f80_div(res, a, b)	__BID_F80_F_FF_OP(res,a,b,/,div)
#define __bid_f80_sub(res, a, b)	__BID_F80_F_FF_OP(res,a,b,-,sub)
#define __bid_f80_mul(res, a, b)	__BID_F80_F_FF_OP(res,a,b,*,mul)

#define __bid_f80_acos(res, a)         __BID_F80_F_F_FUNC(res, a, acos)
#define __bid_f80_acosh(res, a)        __BID_F80_F_F_FUNC(res, a, acosh)
#define __bid_f80_asin(res, a)         __BID_F80_F_F_FUNC(res, a, asin)
#define __bid_f80_asinh(res, a)        __BID_F80_F_F_FUNC(res, a, asinh)
#define __bid_f80_atan(res, a)         __BID_F80_F_F_FUNC(res, a, atan)
#define __bid_f80_cbrt(res, a)         __BID_F80_F_F_FUNC(res, a, cbrt)
#define __bid_f80_cos(res, a)          __BID_F80_F_F_FUNC(res, a, cos)
#define __bid_f80_cosh(res, a)         __BID_F80_F_F_FUNC(res, a, cosh)
#define __bid_f80_erf(res, a)          __BID_F80_F_F_FUNC(res, a, erf)
#define __bid_f80_erfc(res, a)         __BID_F80_F_F_FUNC(res, a, erfc)
#define __bid_f80_exp(res, a)          __BID_F80_F_F_FUNC(res, a, exp)
#define __bid_f80_exp10(res, a)        __BID_F80_F_F_FUNC(res, a, exp10)
#define __bid_f80_exp2(res, a)         __BID_F80_F_F_FUNC(res, a, exp2)
#define __bid_f80_expm1(res, a)        __BID_F80_F_F_FUNC(res, a, expm1)
#define __bid_f80_fabs(res, a)         __BID_F80_F_F_FUNC(res, a, fabs)
#define __bid_f80_lgamma(res, a)       __BID_F80_F_F_FUNC(res, a, lgamma)
#define __bid_f80_log(res, a)          __BID_F80_F_F_FUNC(res, a, log)
#define __bid_f80_log10(res, a)        __BID_F80_F_F_FUNC(res, a, log10)
#define __bid_f80_log1p(res, a)        __BID_F80_F_F_FUNC(res, a, log1p)
#define __bid_f80_log2(res, a)         __BID_F80_F_F_FUNC(res, a, log2)
#define __bid_f80_sin(res, a)          __BID_F80_F_F_FUNC(res, a, sin)
#define __bid_f80_sinh(res, a)         __BID_F80_F_F_FUNC(res, a, sinh)
#define __bid_f80_sqrt(res, a)         __BID_F80_F_F_FUNC(res, a, sqrt)
#define __bid_f80_tan(res, a)          __BID_F80_F_F_FUNC(res, a, tan)
#define __bid_f80_tanh(res, a)         __BID_F80_F_F_FUNC(res, a, tanh)
#define __bid_f80_tgamma(res, a)       __BID_F80_F_F_FUNC(res, a, tgamma)

#define __bid_f80_atan2(res, a, b)     __BID_F80_F_FF_FUNC(res, a, b, atan2)
#define __bid_f80_hypot(res, a, b)     __BID_F80_F_FF_FUNC(res, a, b, hypot)

// =============================================================================
// The f80 function declarations
// =============================================================================

#if USE_COMPILER_F80_TYPE

    __BID_F80_F_F_DECL(acos);
    __BID_F80_F_F_DECL(acosh);
    __BID_F80_F_F_DECL(asin);
    __BID_F80_F_F_DECL(asinh);
    __BID_F80_F_F_DECL(atan);
    __BID_F80_F_F_DECL(cbrt);
    __BID_F80_F_F_DECL(cos);
    __BID_F80_F_F_DECL(cosh);
    __BID_F80_F_F_DECL(erf);
    __BID_F80_F_F_DECL(erfc);
    __BID_F80_F_F_DECL(exp);
    __BID_F80_F_F_DECL(exp10);
    __BID_F80_F_F_DECL(exp2);
    __BID_F80_F_F_DECL(expm1);
    __BID_F80_F_F_DECL(fabs);
    __BID_F80_F_F_DECL(lgamma);
    __BID_F80_F_F_DECL(log);
    __BID_F80_F_F_DECL(log10);
    __BID_F80_F_F_DECL(log1p);
    __BID_F80_F_F_DECL(log2);
    __BID_F80_F_F_DECL(sin);
    __BID_F80_F_F_DECL(sinh);
    __BID_F80_F_F_DECL(sqrt);
    __BID_F80_F_F_DECL(tan);
    __BID_F80_F_F_DECL(tanh);
    __BID_F80_F_F_DECL(tgamma);

    __BID_F80_F_FF_DECL(atan2);
    __BID_F80_F_FF_DECL(hypot);

#endif
