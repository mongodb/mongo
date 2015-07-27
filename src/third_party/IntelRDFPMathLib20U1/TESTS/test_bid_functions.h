/******************************************************************************
  Copyright (c) 2011, Intel Corp.
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

#if defined(__cplusplus)
#define BID_EXTERN_C extern "C"
#else
#define BID_EXTERN_C extern
#endif

#ifndef _BID_FUNCTIONS_H
#define _BID_FUNCTIONS_H

#if !defined (__GNUC__) || defined(__QNX__)
#include <wchar.h>
#endif
#include <ctype.h>

// Fix system header issue on Sun solaris and define required type by ourselves
#if !defined(_WCHAR_T) && !defined(_WCHAR_T_DEFINED) && !defined(__QNX__)
typedef int   wchar_t;
#endif


#ifdef IN_LIBGCC2
// When we are built as the part of the gcc runtime library, libgcc,
// we will use gcc types defined in bid_gcc_intrinsics.h.
#include "bid_gcc_intrinsics.h"

#define BID_ALIGN(n) __attribute__ ((aligned(n)))
#else
typedef char BID_SINT8;
typedef unsigned char BID_UINT8;
typedef unsigned BID_UINT32;
typedef signed BID_SINT32;

#ifdef __GNUC__
#define bid__int64 long long
#else
#define bid__int64 __int64
#endif

#if defined __GNUC__ || defined LINUX || defined SUNOS
typedef unsigned long long BID_UINT64;
typedef signed long long BID_SINT64;
#else
typedef unsigned bid__int64 BID_UINT64;
typedef signed bid__int64 BID_SINT64;
#endif

#if defined _MSC_VER
#if defined _M_IX86 && !defined __INTEL_COMPILER        // Win IA-32, MS compiler
#define BID_ALIGN(n)
#else
#define BID_ALIGN(n) __declspec(align(n))
#endif
#else
#if !defined HPUX_OS
#define BID_ALIGN(n) __attribute__ ((aligned(n)))
#else
#define BID_ALIGN(n)
#endif
#endif

// bid_gcc_intrinsics.h will also define this.
typedef struct BID_ALIGN (16)
     {
       BID_UINT64 w[2];
     } BID_UINT128;
#endif

#if !defined (__INTEL_COMPILER)
typedef BID_UINT128 _Quad;
#endif

#if !defined _MSC_VER || defined __INTEL_COMPILER
#define __ENABLE_BINARY80__  1
#endif

// For building the open source tests:  
// set USE_COMPILER_F128_TYPE=1 when using Intel compiler (_Quad is available)
// unless otherwise specified by user
#if defined (__INTEL_COMPILER) && !defined(USE_COMPILER_F128_TYPE)
#define USE_COMPILER_F128_TYPE 1
#endif

#ifndef HPUX_OS
  #define BINARY80 long double
  #if defined (__INTEL_COMPILER) && USE_COMPILER_F128_TYPE
    #define BINARY128 _Quad
  #else
    #define BINARY128 BID_UINT128
  #endif
  #define SQRT80 sqrtl
#else
  #define BINARY80 __float80
  #define BINARY128 __float128
  #define SQRT80 sqrtw
#endif

     typedef struct BID_ALIGN (16)
     {
       BID_UINT64 w[3];
     } BID_UINT192;
     typedef struct BID_ALIGN (16)
     {
       BID_UINT64 w[4];
     } BID_UINT256;
     typedef unsigned int BID_FPSC; // floating-point status and control

// TYPE parameters
#define BID128_MAXDIGITS        34
#define BID64_MAXDIGITS         16
#define BID32_MAXDIGITS         7

// rounding modes
#define BID_ROUNDING_TO_NEAREST     0x00000
#define BID_ROUNDING_DOWN           0x00001
#define BID_ROUNDING_UP             0x00002
#define BID_ROUNDING_TO_ZERO        0x00003
#define BID_ROUNDING_TIES_AWAY      0x00004

#define BID_RMODE_MASK (BID_ROUNDING_TO_NEAREST | BID_ROUNDING_DOWN | BID_ROUNDING_UP | BID_ROUNDING_TO_ZERO | BID_ROUNDING_TIES_AWAY)

// status
#define BID_FLAG_MASK               0x0000003f
#define DEC_FE_ALL_EXCEPT       0x0000003f
#define BID_IEEE_FLAGS          0x0000003d
#define BID_EXACT_STATUS            0x00000000

///////////////////////////////////////////////////////
//  This section may move to fenv_support.h

#if !defined(__FENV_H_INCLUDED) && !defined (_FENV_H) && !defined(_FENV_INCLUDED)          /* Otherwise we already defined fexcept_t type */
#if defined(__ECL) || defined(__ECC)            /* Intel(R) Itanium(R) architecture */
/* Default 64-bit Floating Point Status Register   */
#if defined(__linux__)
typedef unsigned    long fexcept_t;
#else
typedef unsigned bid__int64 fexcept_t;
#endif
#else
#ifdef __QNX__
#include <fenv.h>
#else
typedef unsigned short int fexcept_t;
#endif
#endif
#endif

#define DEC_FE_INVALID      0x01
#define DEC_FE_UNNORMAL     0x02
#define DEC_FE_DIVBYZERO    0x04
#define DEC_FE_OVERFLOW     0x08
#define DEC_FE_UNDERFLOW    0x10
#define DEC_FE_INEXACT      0x20

////////////////////////////////////////////////////////

#define BID_INEXACT_EXCEPTION       DEC_FE_INEXACT
#define BID_UNDERFLOW_EXCEPTION     DEC_FE_UNDERFLOW
#define BID_OVERFLOW_EXCEPTION      DEC_FE_OVERFLOW
#define BID_ZERO_DIVIDE_EXCEPTION   DEC_FE_DIVBYZERO
#define BID_DENORMAL_EXCEPTION      DEC_FE_UNNORMAL
#define BID_INVALID_EXCEPTION       DEC_FE_INVALID
#define BID_UNDERFLOW_INEXACT_EXCEPTION (DEC_FE_UNDERFLOW|DEC_FE_INEXACT)
#define BID_OVERFLOW_INEXACT_EXCEPTION (DEC_FE_OVERFLOW|DEC_FE_INEXACT)

#define BID_MODE_MASK               0x00001f80
#define BID_INEXACT_MODE            0x00001000
#define BID_UNDERFLOW_MODE          0x00000800
#define BID_OVERFLOW_MODE           0x00000400
#define BID_ZERO_DIVIDE_MODE        0x00000200
#define BID_DENORMAL_MODE           0x00000100
#define BID_INVALID_MODE            0x00000080

#if defined LINUX || defined SUNOS
#define BID_LX16  "%016llx"
#define BID_LX    "%llx"
#define BID_LD4   "%4llu"
#define BID_LD16  "%016lld"
#define BID_LD    "%lld"
#define BID_LUD   "%llu"
#define BID_LUD16 "%016llu"
#define BID_X8    "%08x"
#define BID_X4    "%04x"

#define BID_FMT_LLX16  "%016llx"
#define BID_FMT_LLX    "%llx"
#define BID_FMT_LLU4   "%4llu"
#define BID_FMT_LLD16  "%016lld"
#define BID_FMT_LLD    "%lld"
#define BID_FMT_LLU    "%llu"
#define BID_FMT_LLU16  "%016llu"
#define BID_FMT_X8     "%08x"
#define BID_FMT_X4     "%04x"
#else
#define BID_LX16  "%016I64x"
#define BID_LX    "%I64x"
#define BID_LD16  "%016I64d"
#define BID_LD4   "%4I64u"
#define BID_LD    "%I64d"
#define BID_LUD    "%I64u"
#define BID_LUD16 "%016I64u"
#define BID_X8    "%08x"
#define BID_X4    "%04x"

#define BID_FMT_LLX16 "%016I64x"
#define BID_FMT_LLX   "%I64x"
#define BID_FMT_LLD16 "%016I64d"
#define BID_FMT_LLU4  "%4I64u"
#define BID_FMT_LLD   "%I64d"
#define BID_FMT_LLU   "%I64u"
#define BID_FMT_LLU16 "%016I64u"
#define BID_FMT_X8    "%08x"
#define BID_FMT_X4    "%04x"
#endif

/* rounding modes */
// typedef unsigned int _IDEC_round;
     BID_EXTERN_C _IDEC_round _IDEC_gblround; // initialized to BID_ROUNDING_TO_NEAREST

/* exception flags */
// typedef unsigned int _IDEC_flags;  // could be a struct with diagnostic info
     BID_EXTERN_C _IDEC_flags _IDEC_gblflags; // initialized to BID_EXACT_STATUS

/* exception masks */
     typedef unsigned int _IDEC_exceptionmasks;
     BID_EXTERN_C _IDEC_exceptionmasks _IDEC_gblexceptionmasks;       // initialized to BID_MODE_MASK

#if DECIMAL_ALTERNATE_EXCEPTION_HANDLING

/* exception information */

     typedef struct {
       unsigned int inexact_result:1;
       unsigned int underflow:1;
       unsigned int overflow:1;
       unsigned int zero_divide:1;
       unsigned int invalid_operation:1;
     } BID_fpieee_exception_flags_t;

     typedef enum {
       _fp_round_nearest,
       _fp_round_minus_infinity,
       _fp_round_plus_infinity,
       _fp_round_chopped,
       _fp_round_away
     } BID_fpieee_rounding_mode_t;

     typedef enum {
       _fp_precision24,
       _fp_precision63,
       _fp_precision64,
       _fp_precision7,
       _fp_precision16,
       _fp_precision34
     } _fpieee_precision_t;

     typedef enum {
       _fp_code_unspecified,
       _fp_code_add,
       _fp_code_subtract,
       _fp_code_multiply,
       _fp_code_divide,
       _fp_code_square_root,
       _fp_code_compare,
       _fp_code_convert,
       _fp_code_convert_to_integer_neareven,
       _fp_code_convert_to_integer_down,
       _fp_code_convert_to_integer_up,
       _fp_code_convert_to_integer_truncate,
       _fp_code_convert_to_integer_nearaway,
       _fp_code_fma,
       _fp_code_fmin,
       _fp_code_fmax,
       _fp_code_famin,
       _fp_code_famax,
       _fp_code_round_to_integral,
       _fp_code_minnum,
       _fp_code_maxnum,
       _fp_code_minnummag,
       _fp_code_maxnummag,
       _fp_code_quantize,
       _fp_code_logb,
       _fp_code_scaleb,
       _fp_code_remainder,
       _fp_code_nextup,
       _fp_code_nextdown,
       _fp_code_nextafter,
     } BID_fp_operation_code_t;

     typedef enum {
       _fp_compare_equal,
       _fp_compare_greater,
       _fp_compare_less,
       _fp_compare_unordered
     } fpieee_compare_result_t;

     typedef enum {
       _fp_format_fp32,
       _fp_format_fp64,
       _fp_format_fp80,
       _fp_format_fp128,
       _fp_format_dec_fp32,
       _fp_format_dec_fp64,
       _fp_format_dec_fp128,
       _fp_format_i8,           /* 8-bit integer */
       _fp_format_i16,          /* 16-bit integer */
       _fp_format_i32,          /* 32-bit integer */
       _fp_format_i64,          /* 64-bit integer */
       _fp_format_u8,           /* 8-bit unsigned integer */
       _fp_format_u16,          /* 16-bit unsigned integer */
       _fp_format_u32,          /* 32-bit unsigned integer */
       _fp_format_u64,          /* 64-bit unsigned integer */
       _fp_format_compare,      /* compare value format */
       _fp_format_decimal_char, /* decimal character */
       _fp_format_string        /* string */
     } BID_fpieee_format_t;

     typedef struct {
       unsigned short W[5];
     } _float80_t;

     typedef struct {
       unsigned int W[4];
     } _float128_t;

     typedef struct {
       union {
         float fp32_value;
         double fp64_value;
         _float80_t fp80_value;
         _float128_t fp128_value;
         BID_UINT32 decfp32_value;
         BID_UINT64 decfp64_value;
         BID_UINT128 decfp128_value;
         char i8_value;
         short i16_value;
         int i32_value;
         BID_SINT64 i64_value;
         unsigned char u8_value;
         unsigned short u16_value;
         unsigned int u32_value;
         unsigned long u64_value;
         fpieee_compare_result_t compare_value;
         unsigned char s[256];
       } value;
       unsigned int operand_valid:1;
       BID_fpieee_format_t format:5;
     } BID_fpieee_value_t;

     typedef struct {
       unsigned int rounding_mode:3;
       unsigned int precision:3;
       unsigned int operation:26;
       BID_fpieee_exception_flags_t cause;
       BID_fpieee_exception_flags_t enable;
       BID_fpieee_exception_flags_t status;
       BID_fpieee_value_t operand1;
       BID_fpieee_value_t operand2;
       BID_fpieee_value_t operand3;
       BID_fpieee_value_t result;
     } _IDEC_excepthandling;
     BID_EXTERN_C _IDEC_excepthandling _IDEC_glbexcepthandling;

#endif

#if DECIMAL_CALL_BY_REFERENCE

     BID_EXTERN_C void bid_to_dpd32 (BID_UINT32 * pres, BID_UINT32 * px);
     BID_EXTERN_C void bid_to_dpd64 (BID_UINT64 * pres, BID_UINT64 * px);
     BID_EXTERN_C void bid_to_dpd128 (BID_UINT128 * pres, BID_UINT128 * px);
     BID_EXTERN_C void bid_dpd_to_bid32 (BID_UINT32 * pres, BID_UINT32 * px);
     BID_EXTERN_C void bid_dpd_to_bid64 (BID_UINT64 * pres, BID_UINT64 * px);
     BID_EXTERN_C void bid_dpd_to_bid128 (BID_UINT128 * pres, BID_UINT128 * px);

     BID_EXTERN_C void bid128dd_add (BID_UINT128 * pres, BID_UINT64 * px,
                               BID_UINT64 * py
                               _RND_MODE_PARAM _EXC_FLAGS_PARAM
                               _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128dq_add (BID_UINT128 * pres, BID_UINT64 * px,
                               BID_UINT128 * py
                               _RND_MODE_PARAM _EXC_FLAGS_PARAM
                               _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128qd_add (BID_UINT128 * pres, BID_UINT128 * px,
                               BID_UINT64 * py
                               _RND_MODE_PARAM _EXC_FLAGS_PARAM
                               _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_add (BID_UINT128 * pres, BID_UINT128 * px,
                             BID_UINT128 *
                             py _RND_MODE_PARAM _EXC_FLAGS_PARAM
                             _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128dd_sub (BID_UINT128 * pres, BID_UINT64 * px,
                               BID_UINT64 * py
                               _RND_MODE_PARAM _EXC_FLAGS_PARAM
                               _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128dq_sub (BID_UINT128 * pres, BID_UINT64 * px,
                               BID_UINT128 * py
                               _RND_MODE_PARAM _EXC_FLAGS_PARAM
                               _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128qd_sub (BID_UINT128 * pres, BID_UINT128 * px,
                               BID_UINT64 * py
                               _RND_MODE_PARAM _EXC_FLAGS_PARAM
                               _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_sub (BID_UINT128 * pres, BID_UINT128 * px,
                             BID_UINT128 *
                             py _RND_MODE_PARAM _EXC_FLAGS_PARAM
                             _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128dd_mul (BID_UINT128 * pres, BID_UINT64 * px,
                               BID_UINT64 * py
                               _RND_MODE_PARAM _EXC_FLAGS_PARAM
                               _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128dq_mul (BID_UINT128 * pres, BID_UINT64 * px,
                               BID_UINT128 * py
                               _RND_MODE_PARAM _EXC_FLAGS_PARAM
                               _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128qd_mul (BID_UINT128 * pres, BID_UINT128 * px,
                               BID_UINT64 * py
                               _RND_MODE_PARAM _EXC_FLAGS_PARAM
                               _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_mul (BID_UINT128 * pres, BID_UINT128 * px,
                             BID_UINT128 * py
                             _RND_MODE_PARAM _EXC_FLAGS_PARAM
                             _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_div (BID_UINT128 * pres, BID_UINT128 * px,
                             BID_UINT128 *
                             py _RND_MODE_PARAM _EXC_FLAGS_PARAM
                             _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128dd_div (BID_UINT128 * pres, BID_UINT64 * px,
                               BID_UINT64 * py
                               _RND_MODE_PARAM _EXC_FLAGS_PARAM
                               _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128dq_div (BID_UINT128 * pres, BID_UINT64 * px,
                               BID_UINT128 * py
                               _RND_MODE_PARAM _EXC_FLAGS_PARAM
                               _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128qd_div (BID_UINT128 * pres, BID_UINT128 * px,
                               BID_UINT64 * py
                               _RND_MODE_PARAM _EXC_FLAGS_PARAM
                               _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_fma (BID_UINT128 * pres, BID_UINT128 * px,
                             BID_UINT128 * py, BID_UINT128 * pz
                             _RND_MODE_PARAM _EXC_FLAGS_PARAM
                             _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128ddd_fma (BID_UINT128 * pres, BID_UINT64 * px,
                                BID_UINT64 * py, BID_UINT64 * pz
                                _RND_MODE_PARAM _EXC_FLAGS_PARAM
                                _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128ddq_fma (BID_UINT128 * pres, BID_UINT64 * px,
                                BID_UINT64 * py, BID_UINT128 * pz
                                _RND_MODE_PARAM _EXC_FLAGS_PARAM
                                _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128dqd_fma (BID_UINT128 * pres, BID_UINT64 * px,
                                BID_UINT128 * py, BID_UINT64 * pz
                                _RND_MODE_PARAM _EXC_FLAGS_PARAM
                                _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128dqq_fma (BID_UINT128 * pres, BID_UINT64 * px,
                                BID_UINT128 * py, BID_UINT128 * pz
                                _RND_MODE_PARAM _EXC_FLAGS_PARAM
                                _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128qdd_fma (BID_UINT128 * pres, BID_UINT128 * px,
                                BID_UINT64 * py, BID_UINT64 * pz
                                _RND_MODE_PARAM _EXC_FLAGS_PARAM
                                _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128qdq_fma (BID_UINT128 * pres, BID_UINT128 * px,
                                BID_UINT64 * py, BID_UINT128 * pz
                                _RND_MODE_PARAM _EXC_FLAGS_PARAM
                                _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128qqd_fma (BID_UINT128 * pres, BID_UINT128 * px,
                                BID_UINT128 * py, BID_UINT64 * pz
                                _RND_MODE_PARAM _EXC_FLAGS_PARAM
                                _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     // Note: bid128qqq_fma is represented by bid128_fma
     // Note: bid64ddd_fma is represented by bid64_fma
     BID_EXTERN_C void bid64ddq_fma (BID_UINT64 * pres, BID_UINT64 * px,
                               BID_UINT64 * py, BID_UINT128 * pz
                               _RND_MODE_PARAM _EXC_FLAGS_PARAM
                               _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64dqd_fma (BID_UINT64 * pres, BID_UINT64 * px,
                               BID_UINT128 * py, BID_UINT64 * pz
                               _RND_MODE_PARAM _EXC_FLAGS_PARAM
                               _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64dqq_fma (BID_UINT64 * pres, BID_UINT64 * px,
                               BID_UINT128 * py, BID_UINT128 * pz
                               _RND_MODE_PARAM _EXC_FLAGS_PARAM
                               _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64qdd_fma (BID_UINT64 * pres, BID_UINT128 * px,
                               BID_UINT64 * py, BID_UINT64 * pz
                               _RND_MODE_PARAM _EXC_FLAGS_PARAM
                               _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64qdq_fma (BID_UINT64 * pres, BID_UINT128 * px,
                               BID_UINT64 * py, BID_UINT128 * pz
                               _RND_MODE_PARAM _EXC_FLAGS_PARAM
                               _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64qqd_fma (BID_UINT64 * pres, BID_UINT128 * px,
                               BID_UINT128 * py, BID_UINT64 * pz
                               _RND_MODE_PARAM _EXC_FLAGS_PARAM
                               _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64qqq_fma (BID_UINT64 * pres, BID_UINT128 * px,
                               BID_UINT128 * py, BID_UINT128 * pz
                               _RND_MODE_PARAM _EXC_FLAGS_PARAM
                               _EXC_MASKS_PARAM _EXC_INFO_PARAM);

     BID_EXTERN_C void bid128_sqrt (BID_UINT128 * pres,
                              BID_UINT128 *
                              px _RND_MODE_PARAM _EXC_FLAGS_PARAM
                              _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128d_sqrt (BID_UINT128 * pres, BID_UINT64 * px
                               _RND_MODE_PARAM _EXC_FLAGS_PARAM
                               _EXC_MASKS_PARAM _EXC_INFO_PARAM);

     BID_EXTERN_C void bid128_cbrt (BID_UINT128 * pres,
                              BID_UINT128 *
                              px _RND_MODE_PARAM _EXC_FLAGS_PARAM
                              _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_exp (BID_UINT32 * pres, BID_UINT32 * px
                            _RND_MODE_PARAM _EXC_FLAGS_PARAM
                            _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_log (BID_UINT32 * pres, BID_UINT32 * px
                            _RND_MODE_PARAM _EXC_FLAGS_PARAM
                            _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_pow (BID_UINT32 * pres, BID_UINT32 * px, BID_UINT32 * py
                            _RND_MODE_PARAM _EXC_FLAGS_PARAM
                            _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_atan2 (BID_UINT32 * pres, BID_UINT32 * px, BID_UINT32 * py
                            _RND_MODE_PARAM _EXC_FLAGS_PARAM
                            _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_fmod (BID_UINT32 * pres, BID_UINT32 * px, BID_UINT32 * py
                             _EXC_FLAGS_PARAM
                            _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_modf (BID_UINT32 * pres, BID_UINT32 * px, BID_UINT32 * py
                            _EXC_FLAGS_PARAM
                            _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_hypot (BID_UINT32 * pres, BID_UINT32 * px, BID_UINT32 * py
                            _RND_MODE_PARAM _EXC_FLAGS_PARAM
                            _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_sin (BID_UINT32 * pres, BID_UINT32 * px
                            _RND_MODE_PARAM _EXC_FLAGS_PARAM
                            _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_cos (BID_UINT32 * pres, BID_UINT32 * px
                            _RND_MODE_PARAM _EXC_FLAGS_PARAM
                            _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_tan (BID_UINT32 * pres, BID_UINT32 * px
                            _RND_MODE_PARAM _EXC_FLAGS_PARAM
                            _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_asin (BID_UINT32 * pres, BID_UINT32 * px
                            _RND_MODE_PARAM _EXC_FLAGS_PARAM
                            _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_acos (BID_UINT32 * pres, BID_UINT32 * px
                            _RND_MODE_PARAM _EXC_FLAGS_PARAM
                            _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_atan (BID_UINT32 * pres, BID_UINT32 * px
                            _RND_MODE_PARAM _EXC_FLAGS_PARAM
                            _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_sinh (BID_UINT32 * pres, BID_UINT32 * px
                            _RND_MODE_PARAM _EXC_FLAGS_PARAM
                            _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_cosh (BID_UINT32 * pres, BID_UINT32 * px
                            _RND_MODE_PARAM _EXC_FLAGS_PARAM
                            _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_tanh (BID_UINT32 * pres, BID_UINT32 * px
                            _RND_MODE_PARAM _EXC_FLAGS_PARAM
                            _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_asinh (BID_UINT32 * pres, BID_UINT32 * px
                            _RND_MODE_PARAM _EXC_FLAGS_PARAM
                            _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_acosh (BID_UINT32 * pres, BID_UINT32 * px
                            _RND_MODE_PARAM _EXC_FLAGS_PARAM
                            _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_atanh (BID_UINT32 * pres, BID_UINT32 * px
                            _RND_MODE_PARAM _EXC_FLAGS_PARAM
                            _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_log1p (BID_UINT32 * pres, BID_UINT32 * px
                            _RND_MODE_PARAM _EXC_FLAGS_PARAM
                            _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_expm1 (BID_UINT32 * pres, BID_UINT32 * px
                            _RND_MODE_PARAM _EXC_FLAGS_PARAM
                            _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_log10 (BID_UINT32 * pres, BID_UINT32 * px
                            _RND_MODE_PARAM _EXC_FLAGS_PARAM
                            _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_log2 (BID_UINT32 * pres, BID_UINT32 * px
                            _RND_MODE_PARAM _EXC_FLAGS_PARAM
                            _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_exp2 (BID_UINT32 * pres, BID_UINT32 * px
                            _RND_MODE_PARAM _EXC_FLAGS_PARAM
                            _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_exp10 (BID_UINT32 * pres, BID_UINT32 * px
                            _RND_MODE_PARAM _EXC_FLAGS_PARAM
                            _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_erf (BID_UINT32 * pres, BID_UINT32 * px
                            _RND_MODE_PARAM _EXC_FLAGS_PARAM
                            _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_erfc (BID_UINT32 * pres, BID_UINT32 * px
                            _RND_MODE_PARAM _EXC_FLAGS_PARAM
                            _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_tgamma (BID_UINT32 * pres, BID_UINT32 * px
                            _RND_MODE_PARAM _EXC_FLAGS_PARAM
                            _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_lgamma (BID_UINT32 * pres, BID_UINT32 * px
                            _RND_MODE_PARAM _EXC_FLAGS_PARAM
                            _EXC_MASKS_PARAM _EXC_INFO_PARAM);

     BID_EXTERN_C void bid64_exp (BID_UINT64 * pres, BID_UINT64 * px
                            _RND_MODE_PARAM _EXC_FLAGS_PARAM
                            _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_log (BID_UINT64 * pres, BID_UINT64 * px
                            _RND_MODE_PARAM _EXC_FLAGS_PARAM
                            _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_pow (BID_UINT64 * pres, BID_UINT64 * px, BID_UINT64 * py
                            _RND_MODE_PARAM _EXC_FLAGS_PARAM
                            _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_atan2 (BID_UINT64 * pres, BID_UINT64 * px, BID_UINT64 * py
                            _RND_MODE_PARAM _EXC_FLAGS_PARAM
                            _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_fmod (BID_UINT64 * pres, BID_UINT64 * px, BID_UINT64 * py
                             _EXC_FLAGS_PARAM
                            _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_modf (BID_UINT64 * pres, BID_UINT64 * px, BID_UINT64 * py
                            _EXC_FLAGS_PARAM
                            _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_hypot (BID_UINT64 * pres, BID_UINT64 * px, BID_UINT64 * py
                            _RND_MODE_PARAM _EXC_FLAGS_PARAM
                            _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_sin (BID_UINT64 * pres, BID_UINT64 * px
                            _RND_MODE_PARAM _EXC_FLAGS_PARAM
                            _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_cos (BID_UINT64 * pres, BID_UINT64 * px
                            _RND_MODE_PARAM _EXC_FLAGS_PARAM
                            _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_tan (BID_UINT64 * pres, BID_UINT64 * px
                            _RND_MODE_PARAM _EXC_FLAGS_PARAM
                            _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_asin (BID_UINT64 * pres, BID_UINT64 * px
                            _RND_MODE_PARAM _EXC_FLAGS_PARAM
                            _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_acos (BID_UINT64 * pres, BID_UINT64 * px
                            _RND_MODE_PARAM _EXC_FLAGS_PARAM
                            _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_atan (BID_UINT64 * pres, BID_UINT64 * px
                            _RND_MODE_PARAM _EXC_FLAGS_PARAM
                            _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_sinh (BID_UINT64 * pres, BID_UINT64 * px
                            _RND_MODE_PARAM _EXC_FLAGS_PARAM
                            _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_cosh (BID_UINT64 * pres, BID_UINT64 * px
                            _RND_MODE_PARAM _EXC_FLAGS_PARAM
                            _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_tanh (BID_UINT64 * pres, BID_UINT64 * px
                            _RND_MODE_PARAM _EXC_FLAGS_PARAM
                            _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_asinh (BID_UINT64 * pres, BID_UINT64 * px
                            _RND_MODE_PARAM _EXC_FLAGS_PARAM
                            _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_acosh (BID_UINT64 * pres, BID_UINT64 * px
                            _RND_MODE_PARAM _EXC_FLAGS_PARAM
                            _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_atanh (BID_UINT64 * pres, BID_UINT64 * px
                            _RND_MODE_PARAM _EXC_FLAGS_PARAM
                            _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_log1p (BID_UINT64 * pres, BID_UINT64 * px
                            _RND_MODE_PARAM _EXC_FLAGS_PARAM
                            _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_expm1 (BID_UINT64 * pres, BID_UINT64 * px
                            _RND_MODE_PARAM _EXC_FLAGS_PARAM
                            _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_log10 (BID_UINT64 * pres, BID_UINT64 * px
                            _RND_MODE_PARAM _EXC_FLAGS_PARAM
                            _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_log2 (BID_UINT64 * pres, BID_UINT64 * px
                            _RND_MODE_PARAM _EXC_FLAGS_PARAM
                            _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_exp2 (BID_UINT64 * pres, BID_UINT64 * px
                            _RND_MODE_PARAM _EXC_FLAGS_PARAM
                            _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_exp10 (BID_UINT64 * pres, BID_UINT64 * px
                            _RND_MODE_PARAM _EXC_FLAGS_PARAM
                            _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_erf (BID_UINT64 * pres, BID_UINT64 * px
                            _RND_MODE_PARAM _EXC_FLAGS_PARAM
                            _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_erfc (BID_UINT64 * pres, BID_UINT64 * px
                            _RND_MODE_PARAM _EXC_FLAGS_PARAM
                            _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_tgamma (BID_UINT64 * pres, BID_UINT64 * px
                            _RND_MODE_PARAM _EXC_FLAGS_PARAM
                            _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_lgamma (BID_UINT64 * pres, BID_UINT64 * px
                            _RND_MODE_PARAM _EXC_FLAGS_PARAM
                            _EXC_MASKS_PARAM _EXC_INFO_PARAM);

     BID_EXTERN_C void bid128_exp (BID_UINT128 * pres, BID_UINT128 * px
                            _RND_MODE_PARAM _EXC_FLAGS_PARAM
                            _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_log (BID_UINT128 * pres, BID_UINT128 * px
                            _RND_MODE_PARAM _EXC_FLAGS_PARAM
                            _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_pow (BID_UINT128 * pres, BID_UINT128 * px, BID_UINT128 * py
                            _RND_MODE_PARAM _EXC_FLAGS_PARAM
                            _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_atan2 (BID_UINT128 * pres, BID_UINT128 * px, BID_UINT128 * py
                            _RND_MODE_PARAM _EXC_FLAGS_PARAM
                            _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_fmod (BID_UINT128 * pres, BID_UINT128 * px, BID_UINT128 * py
                             _EXC_FLAGS_PARAM
                            _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_modf (BID_UINT128 * pres, BID_UINT128 * px, BID_UINT128 * py
                            _EXC_FLAGS_PARAM
                            _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_hypot (BID_UINT128 * pres, BID_UINT128 * px, BID_UINT128 * py
                            _RND_MODE_PARAM _EXC_FLAGS_PARAM
                            _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_sin (BID_UINT128 * pres, BID_UINT128 * px
                            _RND_MODE_PARAM _EXC_FLAGS_PARAM
                            _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_cos (BID_UINT128 * pres, BID_UINT128 * px
                            _RND_MODE_PARAM _EXC_FLAGS_PARAM
                            _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_tan (BID_UINT128 * pres, BID_UINT128 * px
                            _RND_MODE_PARAM _EXC_FLAGS_PARAM
                            _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_asin (BID_UINT128 * pres, BID_UINT128 * px
                            _RND_MODE_PARAM _EXC_FLAGS_PARAM
                            _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_acos (BID_UINT128 * pres, BID_UINT128 * px
                            _RND_MODE_PARAM _EXC_FLAGS_PARAM
                            _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_atan (BID_UINT128 * pres, BID_UINT128 * px
                            _RND_MODE_PARAM _EXC_FLAGS_PARAM
                            _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_sinh (BID_UINT128 * pres, BID_UINT128 * px
                            _RND_MODE_PARAM _EXC_FLAGS_PARAM
                            _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_cosh (BID_UINT128 * pres, BID_UINT128 * px
                            _RND_MODE_PARAM _EXC_FLAGS_PARAM
                            _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_tanh (BID_UINT128 * pres, BID_UINT128 * px
                            _RND_MODE_PARAM _EXC_FLAGS_PARAM
                            _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_asinh (BID_UINT128 * pres, BID_UINT128 * px
                            _RND_MODE_PARAM _EXC_FLAGS_PARAM
                            _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_acosh (BID_UINT128 * pres, BID_UINT128 * px
                            _RND_MODE_PARAM _EXC_FLAGS_PARAM
                            _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_atanh (BID_UINT128 * pres, BID_UINT128 * px
                            _RND_MODE_PARAM _EXC_FLAGS_PARAM
                            _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_log1p (BID_UINT128 * pres, BID_UINT128 * px
                            _RND_MODE_PARAM _EXC_FLAGS_PARAM
                            _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_expm1 (BID_UINT128 * pres, BID_UINT128 * px
                            _RND_MODE_PARAM _EXC_FLAGS_PARAM
                            _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_log10 (BID_UINT128 * pres, BID_UINT128 * px
                            _RND_MODE_PARAM _EXC_FLAGS_PARAM
                            _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_log2 (BID_UINT128 * pres, BID_UINT128 * px
                            _RND_MODE_PARAM _EXC_FLAGS_PARAM
                            _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_exp2 (BID_UINT128 * pres, BID_UINT128 * px
                            _RND_MODE_PARAM _EXC_FLAGS_PARAM
                            _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_exp10 (BID_UINT128 * pres, BID_UINT128 * px
                            _RND_MODE_PARAM _EXC_FLAGS_PARAM
                            _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_erf (BID_UINT128 * pres, BID_UINT128 * px
                            _RND_MODE_PARAM _EXC_FLAGS_PARAM
                            _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_erfc (BID_UINT128 * pres, BID_UINT128 * px
                            _RND_MODE_PARAM _EXC_FLAGS_PARAM
                            _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_tgamma (BID_UINT128 * pres, BID_UINT128 * px
                            _RND_MODE_PARAM _EXC_FLAGS_PARAM
                            _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_lgamma (BID_UINT128 * pres, BID_UINT128 * px
                            _RND_MODE_PARAM _EXC_FLAGS_PARAM
                            _EXC_MASKS_PARAM _EXC_INFO_PARAM);

     BID_EXTERN_C void bid64_add (BID_UINT64 * pres, BID_UINT64 * px,
                            BID_UINT64 * py
                                 _RND_MODE_PARAM _EXC_FLAGS_PARAM
                            _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64dq_add (BID_UINT64 * pres, BID_UINT64 * px,
                              BID_UINT128 * py
                              _RND_MODE_PARAM _EXC_FLAGS_PARAM
                              _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64qd_add (BID_UINT64 * pres, BID_UINT128 * px,
                              BID_UINT64 * py
                              _RND_MODE_PARAM _EXC_FLAGS_PARAM
                              _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64qq_add (BID_UINT64 * pres, BID_UINT128 * px,
                              BID_UINT128 * py
                              _RND_MODE_PARAM _EXC_FLAGS_PARAM
                              _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_sub (BID_UINT64 * pres, BID_UINT64 * px,
                            BID_UINT64 *
                            py _RND_MODE_PARAM _EXC_FLAGS_PARAM
                            _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64dq_sub (BID_UINT64 * pres, BID_UINT64 * px,
                              BID_UINT128 * py
                              _RND_MODE_PARAM _EXC_FLAGS_PARAM
                              _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64qd_sub (BID_UINT64 * pres, BID_UINT128 * px,
                              BID_UINT64 * py
                              _RND_MODE_PARAM _EXC_FLAGS_PARAM
                              _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64qq_sub (BID_UINT64 * pres, BID_UINT128 * px,
                              BID_UINT128 * py
                              _RND_MODE_PARAM _EXC_FLAGS_PARAM
                              _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_mul (BID_UINT64 * pres, BID_UINT64 * px,
                            BID_UINT64 * py
                            _RND_MODE_PARAM _EXC_FLAGS_PARAM
                            _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64dq_mul (BID_UINT64 * pres, BID_UINT64 * px,
                              BID_UINT128 * py
                              _RND_MODE_PARAM _EXC_FLAGS_PARAM
                              _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64qd_mul (BID_UINT64 * pres, BID_UINT128 * px,
                              BID_UINT64 * py
                              _RND_MODE_PARAM _EXC_FLAGS_PARAM
                              _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64qq_mul (BID_UINT64 * pres, BID_UINT128 * px,
                              BID_UINT128 * py
                              _RND_MODE_PARAM _EXC_FLAGS_PARAM
                              _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_div (BID_UINT64 * pres, BID_UINT64 * px,
                            BID_UINT64 *
                            py _RND_MODE_PARAM _EXC_FLAGS_PARAM
                            _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64dq_div (BID_UINT64 * pres, BID_UINT64 * px,
                              BID_UINT128 * py
                              _RND_MODE_PARAM _EXC_FLAGS_PARAM
                              _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64qd_div (BID_UINT64 * pres, BID_UINT128 * px,
                              BID_UINT64 * py
                              _RND_MODE_PARAM _EXC_FLAGS_PARAM
                              _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64qq_div (BID_UINT64 * pres, BID_UINT128 * px,
                              BID_UINT128 * py
                              _RND_MODE_PARAM _EXC_FLAGS_PARAM
                              _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_fma (BID_UINT64 * pres, BID_UINT64 * px,
                            BID_UINT64 * py,
                            BID_UINT64 *
                            pz _RND_MODE_PARAM _EXC_FLAGS_PARAM
                            _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_sqrt (BID_UINT64 * pres,
                             BID_UINT64 *
                             px _RND_MODE_PARAM _EXC_FLAGS_PARAM
                             _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64q_sqrt (BID_UINT64 * pres, BID_UINT128 * px
                              _RND_MODE_PARAM _EXC_FLAGS_PARAM
                              _EXC_MASKS_PARAM _EXC_INFO_PARAM);

     BID_EXTERN_C void bid64_cbrt (BID_UINT64 * pres,
                             BID_UINT64 *
                             px _RND_MODE_PARAM _EXC_FLAGS_PARAM
                             _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_to_int8_rnint (char *pres,
                                       BID_UINT128 *
                                       px _EXC_FLAGS_PARAM
                                       _EXC_MASKS_PARAM
                                       _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_to_int8_xrnint (char *pres,
                                        BID_UINT128 *
                                        px _EXC_FLAGS_PARAM
                                        _EXC_MASKS_PARAM
                                        _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_to_int8_rninta (char *pres,
                                        BID_UINT128 *
                                        px _EXC_FLAGS_PARAM
                                        _EXC_MASKS_PARAM
                                        _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_to_int8_xrninta (char *pres,
                                         BID_UINT128 *
                                         px _EXC_FLAGS_PARAM
                                         _EXC_MASKS_PARAM
                                         _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_to_int8_int (char *pres,
                                     BID_UINT128 *
                                     px _EXC_FLAGS_PARAM
                                     _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_to_int8_xint (char *pres,
                                      BID_UINT128 *
                                      px _EXC_FLAGS_PARAM
                                      _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_to_int8_floor (char *pres,
                                       BID_UINT128 *
                                       px _EXC_FLAGS_PARAM
                                       _EXC_MASKS_PARAM
                                       _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_to_int8_xfloor (char *pres,
                                        BID_UINT128 *
                                        px _EXC_FLAGS_PARAM
                                        _EXC_MASKS_PARAM
                                        _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_to_int8_ceil (char *pres,
                                      BID_UINT128 *
                                      px _EXC_FLAGS_PARAM
                                      _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_to_int8_xceil (char *pres,
                                       BID_UINT128 *
                                       px _EXC_FLAGS_PARAM
                                       _EXC_MASKS_PARAM
                                       _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_to_int16_rnint (short *pres,
                                        BID_UINT128 *
                                        px _EXC_FLAGS_PARAM
                                        _EXC_MASKS_PARAM
                                        _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_to_int16_xrnint (short *pres,
                                         BID_UINT128 *
                                         px _EXC_FLAGS_PARAM
                                         _EXC_MASKS_PARAM
                                         _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_to_int16_rninta (short *pres,
                                         BID_UINT128 *
                                         px _EXC_FLAGS_PARAM
                                         _EXC_MASKS_PARAM
                                         _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_to_int16_xrninta (short *pres,
                                          BID_UINT128 *
                                          px _EXC_FLAGS_PARAM
                                          _EXC_MASKS_PARAM
                                          _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_to_int16_int (short *pres,
                                      BID_UINT128 *
                                      px _EXC_FLAGS_PARAM
                                      _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_to_int16_xint (short *pres,
                                       BID_UINT128 *
                                       px _EXC_FLAGS_PARAM
                                       _EXC_MASKS_PARAM
                                       _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_to_int16_floor (short *pres,
                                        BID_UINT128 *
                                        px _EXC_FLAGS_PARAM
                                        _EXC_MASKS_PARAM
                                        _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_to_int16_xfloor (short *pres,
                                         BID_UINT128 *
                                         px _EXC_FLAGS_PARAM
                                         _EXC_MASKS_PARAM
                                         _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_to_int16_ceil (short *pres,
                                       BID_UINT128 *
                                       px _EXC_FLAGS_PARAM
                                       _EXC_MASKS_PARAM
                                       _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_to_int16_xceil (short *pres,
                                        BID_UINT128 *
                                        px _EXC_FLAGS_PARAM
                                        _EXC_MASKS_PARAM
                                        _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_to_uint8_rnint (unsigned char *pres,
                                        BID_UINT128 *
                                        px _EXC_FLAGS_PARAM
                                        _EXC_MASKS_PARAM
                                        _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_to_uint8_xrnint (unsigned char *pres,
                                         BID_UINT128 *
                                         px _EXC_FLAGS_PARAM
                                         _EXC_MASKS_PARAM
                                         _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_to_uint8_rninta (unsigned char *pres,
                                         BID_UINT128 *
                                         px _EXC_FLAGS_PARAM
                                         _EXC_MASKS_PARAM
                                         _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_to_uint8_xrninta (unsigned char *pres,
                                          BID_UINT128 *
                                          px _EXC_FLAGS_PARAM
                                          _EXC_MASKS_PARAM
                                          _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_to_uint8_int (unsigned char *pres,
                                      BID_UINT128 *
                                      px _EXC_FLAGS_PARAM
                                      _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_to_uint8_xint (unsigned char *pres,
                                       BID_UINT128 *
                                       px _EXC_FLAGS_PARAM
                                       _EXC_MASKS_PARAM
                                       _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_to_uint8_floor (unsigned char *pres,
                                        BID_UINT128 *
                                        px _EXC_FLAGS_PARAM
                                        _EXC_MASKS_PARAM
                                        _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_to_uint8_xfloor (unsigned char *pres,
                                         BID_UINT128 *
                                         px _EXC_FLAGS_PARAM
                                         _EXC_MASKS_PARAM
                                         _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_to_uint8_ceil (unsigned char *pres,
                                       BID_UINT128 *
                                       px _EXC_FLAGS_PARAM
                                       _EXC_MASKS_PARAM
                                       _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_to_uint8_xceil (unsigned char *pres,
                                        BID_UINT128 *
                                        px _EXC_FLAGS_PARAM
                                        _EXC_MASKS_PARAM
                                        _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_to_uint16_rnint (unsigned short *pres,
                                         BID_UINT128 *
                                         px _EXC_FLAGS_PARAM
                                         _EXC_MASKS_PARAM
                                         _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_to_uint16_xrnint (unsigned short *pres,
                                          BID_UINT128 *
                                          px _EXC_FLAGS_PARAM
                                          _EXC_MASKS_PARAM
                                          _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_to_uint16_rninta (unsigned short *pres,
                                          BID_UINT128 *
                                          px _EXC_FLAGS_PARAM
                                          _EXC_MASKS_PARAM
                                          _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_to_uint16_xrninta (unsigned short *pres,
                                           BID_UINT128 *
                                           px _EXC_FLAGS_PARAM
                                           _EXC_MASKS_PARAM
                                           _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_to_uint16_int (unsigned short *pres,
                                       BID_UINT128 *
                                       px _EXC_FLAGS_PARAM
                                       _EXC_MASKS_PARAM
                                       _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_to_uint16_xint (unsigned short *pres,
                                        BID_UINT128 *
                                        px _EXC_FLAGS_PARAM
                                        _EXC_MASKS_PARAM
                                        _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_to_uint16_floor (unsigned short *pres,
                                         BID_UINT128 *
                                         px _EXC_FLAGS_PARAM
                                         _EXC_MASKS_PARAM
                                         _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_to_uint16_xfloor (unsigned short *pres,
                                          BID_UINT128 *
                                          px _EXC_FLAGS_PARAM
                                          _EXC_MASKS_PARAM
                                          _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_to_uint16_ceil (unsigned short *pres,
                                        BID_UINT128 *
                                        px _EXC_FLAGS_PARAM
                                        _EXC_MASKS_PARAM
                                        _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_to_uint16_xceil (unsigned short *pres,
                                         BID_UINT128 *
                                         px _EXC_FLAGS_PARAM
                                         _EXC_MASKS_PARAM
                                         _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_to_int32_rnint (int *pres,
                                        BID_UINT128 *
                                        px _EXC_FLAGS_PARAM
                                        _EXC_MASKS_PARAM
                                        _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_to_int32_xrnint (int *pres,
                                         BID_UINT128 *
                                         px _EXC_FLAGS_PARAM
                                         _EXC_MASKS_PARAM
                                         _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_to_int32_rninta (int *pres,
                                         BID_UINT128 *
                                         px _EXC_FLAGS_PARAM
                                         _EXC_MASKS_PARAM
                                         _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_to_int32_xrninta (int *pres,
                                          BID_UINT128 *
                                          px _EXC_FLAGS_PARAM
                                          _EXC_MASKS_PARAM
                                          _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_to_int32_int (int *pres,
                                      BID_UINT128 *
                                      px _EXC_FLAGS_PARAM
                                      _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_to_int32_xint (int *pres,
                                       BID_UINT128 *
                                       px _EXC_FLAGS_PARAM
                                       _EXC_MASKS_PARAM
                                       _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_to_int32_floor (int *pres,
                                        BID_UINT128 *
                                        px _EXC_FLAGS_PARAM
                                        _EXC_MASKS_PARAM
                                        _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_to_int32_xfloor (int *pres,
                                         BID_UINT128 *
                                         px _EXC_FLAGS_PARAM
                                         _EXC_MASKS_PARAM
                                         _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_to_int32_ceil (int *pres,
                                       BID_UINT128 *
                                       px _EXC_FLAGS_PARAM
                                       _EXC_MASKS_PARAM
                                       _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_to_int32_xceil (int *pres,
                                        BID_UINT128 *
                                        px _EXC_FLAGS_PARAM
                                        _EXC_MASKS_PARAM
                                        _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_to_uint32_rnint (unsigned int *pres,
                                         BID_UINT128 *
                                         px _EXC_FLAGS_PARAM
                                         _EXC_MASKS_PARAM
                                         _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_to_uint32_xrnint (unsigned int *pres,
                                          BID_UINT128 *
                                          px _EXC_FLAGS_PARAM
                                          _EXC_MASKS_PARAM
                                          _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_to_uint32_rninta (unsigned int *pres,
                                          BID_UINT128 *
                                          px _EXC_FLAGS_PARAM
                                          _EXC_MASKS_PARAM
                                          _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_to_uint32_xrninta (unsigned int *pres,
                                           BID_UINT128 *
                                           px _EXC_FLAGS_PARAM
                                           _EXC_MASKS_PARAM
                                           _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_to_uint32_int (unsigned int *pres,
                                       BID_UINT128 *
                                       px _EXC_FLAGS_PARAM
                                       _EXC_MASKS_PARAM
                                       _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_to_uint32_xint (unsigned int *pres,
                                        BID_UINT128 *
                                        px _EXC_FLAGS_PARAM
                                        _EXC_MASKS_PARAM
                                        _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_to_uint32_floor (unsigned int *pres,
                                         BID_UINT128 *
                                         px _EXC_FLAGS_PARAM
                                         _EXC_MASKS_PARAM
                                         _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_to_uint32_xfloor (unsigned int *pres,
                                          BID_UINT128 *
                                          px _EXC_FLAGS_PARAM
                                          _EXC_MASKS_PARAM
                                          _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_to_uint32_ceil (unsigned int *pres,
                                        BID_UINT128 *
                                        px _EXC_FLAGS_PARAM
                                        _EXC_MASKS_PARAM
                                        _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_to_uint32_xceil (unsigned int *pres,
                                         BID_UINT128 *
                                         px _EXC_FLAGS_PARAM
                                         _EXC_MASKS_PARAM
                                         _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_to_int64_rnint (BID_SINT64 * pres,
                                        BID_UINT128 *
                                        px _EXC_FLAGS_PARAM
                                        _EXC_MASKS_PARAM
                                        _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_to_int64_xrnint (BID_SINT64 * pres,
                                         BID_UINT128 *
                                         px _EXC_FLAGS_PARAM
                                         _EXC_MASKS_PARAM
                                         _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_to_int64_rninta (BID_SINT64 * pres,
                                         BID_UINT128 *
                                         px _EXC_FLAGS_PARAM
                                         _EXC_MASKS_PARAM
                                         _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_to_int64_xrninta (BID_SINT64 * pres,
                                          BID_UINT128 *
                                          px _EXC_FLAGS_PARAM
                                          _EXC_MASKS_PARAM
                                          _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_to_int64_int (BID_SINT64 * pres,
                                      BID_UINT128 *
                                      px _EXC_FLAGS_PARAM
                                      _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_to_int64_xint (BID_SINT64 * pres,
                                       BID_UINT128 *
                                       px _EXC_FLAGS_PARAM
                                       _EXC_MASKS_PARAM
                                       _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_to_int64_floor (BID_SINT64 * pres,
                                        BID_UINT128 *
                                        px _EXC_FLAGS_PARAM
                                        _EXC_MASKS_PARAM
                                        _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_to_int64_xfloor (BID_SINT64 * pres,
                                         BID_UINT128 *
                                         px _EXC_FLAGS_PARAM
                                         _EXC_MASKS_PARAM
                                         _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_to_int64_ceil (BID_SINT64 * pres,
                                       BID_UINT128 *
                                       px _EXC_FLAGS_PARAM
                                       _EXC_MASKS_PARAM
                                       _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_to_int64_xceil (BID_SINT64 * pres,
                                        BID_UINT128 *
                                        px _EXC_FLAGS_PARAM
                                        _EXC_MASKS_PARAM
                                        _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_to_uint64_rnint (BID_UINT64 * pres,
                                         BID_UINT128 *
                                         px _EXC_FLAGS_PARAM
                                         _EXC_MASKS_PARAM
                                         _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_to_uint64_xrnint (BID_UINT64 * pres,
                                          BID_UINT128 *
                                          px _EXC_FLAGS_PARAM
                                          _EXC_MASKS_PARAM
                                          _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_to_uint64_rninta (BID_UINT64 * pres,
                                          BID_UINT128 *
                                          px _EXC_FLAGS_PARAM
                                          _EXC_MASKS_PARAM
                                          _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_to_uint64_xrninta (BID_UINT64 * pres,
                                           BID_UINT128 *
                                           px _EXC_FLAGS_PARAM
                                           _EXC_MASKS_PARAM
                                           _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_to_uint64_int (BID_UINT64 * pres,
                                       BID_UINT128 *
                                       px _EXC_FLAGS_PARAM
                                       _EXC_MASKS_PARAM
                                       _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_to_uint64_xint (BID_UINT64 * pres,
                                        BID_UINT128 *
                                        px _EXC_FLAGS_PARAM
                                        _EXC_MASKS_PARAM
                                        _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_to_uint64_floor (BID_UINT64 * pres,
                                         BID_UINT128 *
                                         px _EXC_FLAGS_PARAM
                                         _EXC_MASKS_PARAM
                                         _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_to_uint64_xfloor (BID_UINT64 * pres,
                                          BID_UINT128 *
                                          px _EXC_FLAGS_PARAM
                                          _EXC_MASKS_PARAM
                                          _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_to_uint64_ceil (BID_UINT64 * pres,
                                        BID_UINT128 *
                                        px _EXC_FLAGS_PARAM
                                        _EXC_MASKS_PARAM
                                        _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_to_uint64_xceil (BID_UINT64 * pres,
                                         BID_UINT128 *
                                         px _EXC_FLAGS_PARAM
                                         _EXC_MASKS_PARAM
                                         _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_to_int32_rnint (int *pres,
                                       BID_UINT64 *
                                       px _EXC_FLAGS_PARAM
                                       _EXC_MASKS_PARAM
                                       _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_to_int32_xrnint (int *pres,
                                        BID_UINT64 *
                                        px _EXC_FLAGS_PARAM
                                        _EXC_MASKS_PARAM
                                        _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_to_int32_rninta (int *pres,
                                        BID_UINT64 *
                                        px _EXC_FLAGS_PARAM
                                        _EXC_MASKS_PARAM
                                        _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_to_int32_xrninta (int *pres,
                                         BID_UINT64 *
                                         px _EXC_FLAGS_PARAM
                                         _EXC_MASKS_PARAM
                                         _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_to_int32_int (int *pres,
                                     BID_UINT64 *
                                     px _EXC_FLAGS_PARAM
                                     _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_to_int32_xint (int *pres,
                                      BID_UINT64 *
                                      px _EXC_FLAGS_PARAM
                                      _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_to_int32_floor (int *pres,
                                       BID_UINT64 *
                                       px _EXC_FLAGS_PARAM
                                       _EXC_MASKS_PARAM
                                       _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_to_int32_xfloor (int *pres,
                                        BID_UINT64 *
                                        px _EXC_FLAGS_PARAM
                                        _EXC_MASKS_PARAM
                                        _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_to_int32_ceil (int *pres,
                                      BID_UINT64 *
                                      px _EXC_FLAGS_PARAM
                                      _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_to_int32_xceil (int *pres,
                                       BID_UINT64 *
                                       px _EXC_FLAGS_PARAM
                                       _EXC_MASKS_PARAM
                                       _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_to_int8_rnint (char *pres,
                                      BID_UINT64 *
                                      px _EXC_FLAGS_PARAM
                                      _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_to_int8_xrnint (char *pres,
                                       BID_UINT64 *
                                       px _EXC_FLAGS_PARAM
                                       _EXC_MASKS_PARAM
                                       _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_to_int8_rninta (char *pres,
                                       BID_UINT64 *
                                       px _EXC_FLAGS_PARAM
                                       _EXC_MASKS_PARAM
                                       _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_to_int8_xrninta (char *pres,
                                        BID_UINT64 *
                                        px _EXC_FLAGS_PARAM
                                        _EXC_MASKS_PARAM
                                        _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_to_int8_int (char *pres,
                                    BID_UINT64 *
                                    px _EXC_FLAGS_PARAM _EXC_MASKS_PARAM
                                    _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_to_int8_xint (char *pres,
                                     BID_UINT64 *
                                     px _EXC_FLAGS_PARAM
                                     _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_to_int8_floor (char *pres,
                                      BID_UINT64 *
                                      px _EXC_FLAGS_PARAM
                                      _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_to_int8_xfloor (char *pres,
                                       BID_UINT64 *
                                       px _EXC_FLAGS_PARAM
                                       _EXC_MASKS_PARAM
                                       _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_to_int8_ceil (char *pres,
                                     BID_UINT64 *
                                     px _EXC_FLAGS_PARAM
                                     _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_to_int8_xceil (char *pres,
                                      BID_UINT64 *
                                      px _EXC_FLAGS_PARAM
                                      _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_to_int16_rnint (short *pres,
                                       BID_UINT64 *
                                       px _EXC_FLAGS_PARAM
                                       _EXC_MASKS_PARAM
                                       _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_to_int16_xrnint (short *pres,
                                        BID_UINT64 *
                                        px _EXC_FLAGS_PARAM
                                        _EXC_MASKS_PARAM
                                        _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_to_int16_rninta (short *pres,
                                        BID_UINT64 *
                                        px _EXC_FLAGS_PARAM
                                        _EXC_MASKS_PARAM
                                        _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_to_int16_xrninta (short *pres,
                                         BID_UINT64 *
                                         px _EXC_FLAGS_PARAM
                                         _EXC_MASKS_PARAM
                                         _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_to_int16_int (short *pres,
                                     BID_UINT64 *
                                     px _EXC_FLAGS_PARAM
                                     _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_to_int16_xint (short *pres,
                                      BID_UINT64 *
                                      px _EXC_FLAGS_PARAM
                                      _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_to_int16_floor (short *pres,
                                       BID_UINT64 *
                                       px _EXC_FLAGS_PARAM
                                       _EXC_MASKS_PARAM
                                       _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_to_int16_xfloor (short *pres,
                                        BID_UINT64 *
                                        px _EXC_FLAGS_PARAM
                                        _EXC_MASKS_PARAM
                                        _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_to_int16_ceil (short *pres,
                                      BID_UINT64 *
                                      px _EXC_FLAGS_PARAM
                                      _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_to_int16_xceil (short *pres,
                                       BID_UINT64 *
                                       px _EXC_FLAGS_PARAM
                                       _EXC_MASKS_PARAM
                                       _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_to_uint8_rnint (unsigned char *pres,
                                       BID_UINT64 *
                                       px _EXC_FLAGS_PARAM
                                       _EXC_MASKS_PARAM
                                       _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_to_uint8_xrnint (unsigned char *pres,
                                        BID_UINT64 *
                                        px _EXC_FLAGS_PARAM
                                        _EXC_MASKS_PARAM
                                        _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_to_uint8_rninta (unsigned char *pres,
                                        BID_UINT64 *
                                        px _EXC_FLAGS_PARAM
                                        _EXC_MASKS_PARAM
                                        _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_to_uint8_xrninta (unsigned char *pres,
                                         BID_UINT64 *
                                         px _EXC_FLAGS_PARAM
                                         _EXC_MASKS_PARAM
                                         _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_to_uint8_int (unsigned char *pres,
                                     BID_UINT64 *
                                     px _EXC_FLAGS_PARAM
                                     _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_to_uint8_xint (unsigned char *pres,
                                      BID_UINT64 *
                                      px _EXC_FLAGS_PARAM
                                      _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_to_uint8_floor (unsigned char *pres,
                                       BID_UINT64 *
                                       px _EXC_FLAGS_PARAM
                                       _EXC_MASKS_PARAM
                                       _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_to_uint8_xfloor (unsigned char *pres,
                                        BID_UINT64 *
                                        px _EXC_FLAGS_PARAM
                                        _EXC_MASKS_PARAM
                                        _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_to_uint8_ceil (unsigned char *pres,
                                      BID_UINT64 *
                                      px _EXC_FLAGS_PARAM
                                      _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_to_uint8_xceil (unsigned char *pres,
                                       BID_UINT64 *
                                       px _EXC_FLAGS_PARAM
                                       _EXC_MASKS_PARAM
                                       _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_to_uint16_rnint (unsigned short *pres,
                                        BID_UINT64 *
                                        px _EXC_FLAGS_PARAM
                                        _EXC_MASKS_PARAM
                                        _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_to_uint16_xrnint (unsigned short *pres,
                                         BID_UINT64 *
                                         px _EXC_FLAGS_PARAM
                                         _EXC_MASKS_PARAM
                                         _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_to_uint16_rninta (unsigned short *pres,
                                         BID_UINT64 *
                                         px _EXC_FLAGS_PARAM
                                         _EXC_MASKS_PARAM
                                         _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_to_uint16_xrninta (unsigned short *pres,
                                          BID_UINT64 *
                                          px _EXC_FLAGS_PARAM
                                          _EXC_MASKS_PARAM
                                          _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_to_uint16_int (unsigned short *pres,
                                      BID_UINT64 *
                                      px _EXC_FLAGS_PARAM
                                      _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_to_uint16_xint (unsigned short *pres,
                                       BID_UINT64 *
                                       px _EXC_FLAGS_PARAM
                                       _EXC_MASKS_PARAM
                                       _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_to_uint16_floor (unsigned short *pres,
                                        BID_UINT64 *
                                        px _EXC_FLAGS_PARAM
                                        _EXC_MASKS_PARAM
                                        _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_to_uint16_xfloor (unsigned short *pres,
                                         BID_UINT64 *
                                         px _EXC_FLAGS_PARAM
                                         _EXC_MASKS_PARAM
                                         _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_to_uint16_ceil (unsigned short *pres,
                                       BID_UINT64 *
                                       px _EXC_FLAGS_PARAM
                                       _EXC_MASKS_PARAM
                                       _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_to_uint16_xceil (unsigned short *pres,
                                        BID_UINT64 *
                                        px _EXC_FLAGS_PARAM
                                        _EXC_MASKS_PARAM
                                        _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_to_uint32_rnint (unsigned int *pres,
                                        BID_UINT64 *
                                        px _EXC_FLAGS_PARAM
                                        _EXC_MASKS_PARAM
                                        _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_to_uint32_xrnint (unsigned int *pres,
                                         BID_UINT64 *
                                         px _EXC_FLAGS_PARAM
                                         _EXC_MASKS_PARAM
                                         _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_to_uint32_rninta (unsigned int *pres,
                                         BID_UINT64 *
                                         px _EXC_FLAGS_PARAM
                                         _EXC_MASKS_PARAM
                                         _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_to_uint32_xrninta (unsigned int *pres,
                                          BID_UINT64 *
                                          px _EXC_FLAGS_PARAM
                                          _EXC_MASKS_PARAM
                                          _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_to_uint32_int (unsigned int *pres,
                                      BID_UINT64 *
                                      px _EXC_FLAGS_PARAM
                                      _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_to_uint32_xint (unsigned int *pres,
                                       BID_UINT64 *
                                       px _EXC_FLAGS_PARAM
                                       _EXC_MASKS_PARAM
                                       _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_to_uint32_floor (unsigned int *pres,
                                        BID_UINT64 *
                                        px _EXC_FLAGS_PARAM
                                        _EXC_MASKS_PARAM
                                        _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_to_uint32_xfloor (unsigned int *pres,
                                         BID_UINT64 *
                                         px _EXC_FLAGS_PARAM
                                         _EXC_MASKS_PARAM
                                         _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_to_uint32_ceil (unsigned int *pres,
                                       BID_UINT64 *
                                       px _EXC_FLAGS_PARAM
                                       _EXC_MASKS_PARAM
                                       _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_to_uint32_xceil (unsigned int *pres,
                                        BID_UINT64 *
                                        px _EXC_FLAGS_PARAM
                                        _EXC_MASKS_PARAM
                                        _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_to_int64_rnint (BID_SINT64 * pres,
                                       BID_UINT64 *
                                       px _EXC_FLAGS_PARAM
                                       _EXC_MASKS_PARAM
                                       _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_to_int64_xrnint (BID_SINT64 * pres,
                                        BID_UINT64 *
                                        px _EXC_FLAGS_PARAM
                                        _EXC_MASKS_PARAM
                                        _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_to_int64_rninta (BID_SINT64 * pres,
                                        BID_UINT64 *
                                        px _EXC_FLAGS_PARAM
                                        _EXC_MASKS_PARAM
                                        _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_to_int64_xrninta (BID_SINT64 * pres,
                                         BID_UINT64 *
                                         px _EXC_FLAGS_PARAM
                                         _EXC_MASKS_PARAM
                                         _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_to_int64_int (BID_SINT64 * pres,
                                     BID_UINT64 *
                                     px _EXC_FLAGS_PARAM
                                     _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_to_int64_xint (BID_SINT64 * pres,
                                      BID_UINT64 *
                                      px _EXC_FLAGS_PARAM
                                      _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_to_int64_floor (BID_SINT64 * pres,
                                       BID_UINT64 *
                                       px _EXC_FLAGS_PARAM
                                       _EXC_MASKS_PARAM
                                       _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_to_int64_xfloor (BID_SINT64 * pres,
                                        BID_UINT64 *
                                        px _EXC_FLAGS_PARAM
                                        _EXC_MASKS_PARAM
                                        _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_to_int64_ceil (BID_SINT64 * pres,
                                      BID_UINT64 *
                                      px _EXC_FLAGS_PARAM
                                      _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_to_int64_xceil (BID_SINT64 * pres,
                                       BID_UINT64 *
                                       px _EXC_FLAGS_PARAM
                                       _EXC_MASKS_PARAM
                                       _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_to_uint64_rnint (BID_UINT64 * pres,
                                        BID_UINT64 *
                                        px _EXC_FLAGS_PARAM
                                        _EXC_MASKS_PARAM
                                        _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_to_uint64_xrnint (BID_UINT64 * pres,
                                         BID_UINT64 *
                                         px _EXC_FLAGS_PARAM
                                         _EXC_MASKS_PARAM
                                         _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_to_uint64_rninta (BID_UINT64 * pres,
                                         BID_UINT64 *
                                         px _EXC_FLAGS_PARAM
                                         _EXC_MASKS_PARAM
                                         _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_to_uint64_xrninta (BID_UINT64 * pres,
                                          BID_UINT64 *
                                          px _EXC_FLAGS_PARAM
                                          _EXC_MASKS_PARAM
                                          _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_to_uint64_int (BID_UINT64 * pres,
                                      BID_UINT64 *
                                      px _EXC_FLAGS_PARAM
                                      _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_to_uint64_xint (BID_UINT64 * pres,
                                       BID_UINT64 *
                                       px _EXC_FLAGS_PARAM
                                       _EXC_MASKS_PARAM
                                       _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_to_uint64_floor (BID_UINT64 * pres,
                                        BID_UINT64 *
                                        px _EXC_FLAGS_PARAM
                                        _EXC_MASKS_PARAM
                                        _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_to_uint64_xfloor (BID_UINT64 * pres,
                                         BID_UINT64 *
                                         px _EXC_FLAGS_PARAM
                                         _EXC_MASKS_PARAM
                                         _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_to_uint64_ceil (BID_UINT64 * pres,
                                       BID_UINT64 *
                                       px _EXC_FLAGS_PARAM
                                       _EXC_MASKS_PARAM
                                       _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_to_uint64_xceil (BID_UINT64 * pres,
                                        BID_UINT64 *
                                        px _EXC_FLAGS_PARAM
                                        _EXC_MASKS_PARAM
                                        _EXC_INFO_PARAM);

     BID_EXTERN_C void bid32_to_int32_rnint (int *pres,
                                       BID_UINT32 *
                                       px _EXC_FLAGS_PARAM
                                       _EXC_MASKS_PARAM
                                       _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_to_int32_xrnint (int *pres,
                                        BID_UINT32 *
                                        px _EXC_FLAGS_PARAM
                                        _EXC_MASKS_PARAM
                                        _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_to_int32_rninta (int *pres,
                                        BID_UINT32 *
                                        px _EXC_FLAGS_PARAM
                                        _EXC_MASKS_PARAM
                                        _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_to_int32_xrninta (int *pres,
                                         BID_UINT32 *
                                         px _EXC_FLAGS_PARAM
                                         _EXC_MASKS_PARAM
                                         _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_to_int32_int (int *pres,
                                     BID_UINT32 *
                                     px _EXC_FLAGS_PARAM
                                     _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_to_int32_xint (int *pres,
                                      BID_UINT32 *
                                      px _EXC_FLAGS_PARAM
                                      _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_to_int32_floor (int *pres,
                                       BID_UINT32 *
                                       px _EXC_FLAGS_PARAM
                                       _EXC_MASKS_PARAM
                                       _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_to_int32_xfloor (int *pres,
                                        BID_UINT32 *
                                        px _EXC_FLAGS_PARAM
                                        _EXC_MASKS_PARAM
                                        _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_to_int32_ceil (int *pres,
                                      BID_UINT32 *
                                      px _EXC_FLAGS_PARAM
                                      _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_to_int32_xceil (int *pres,
                                       BID_UINT32 *
                                       px _EXC_FLAGS_PARAM
                                       _EXC_MASKS_PARAM
                                       _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_to_int8_rnint (char *pres,
                                      BID_UINT32 *
                                      px _EXC_FLAGS_PARAM
                                      _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_to_int8_xrnint (char *pres,
                                       BID_UINT32 *
                                       px _EXC_FLAGS_PARAM
                                       _EXC_MASKS_PARAM
                                       _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_to_int8_rninta (char *pres,
                                       BID_UINT32 *
                                       px _EXC_FLAGS_PARAM
                                       _EXC_MASKS_PARAM
                                       _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_to_int8_xrninta (char *pres,
                                        BID_UINT32 *
                                        px _EXC_FLAGS_PARAM
                                        _EXC_MASKS_PARAM
                                        _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_to_int8_int (char *pres,
                                    BID_UINT32 *
                                    px _EXC_FLAGS_PARAM _EXC_MASKS_PARAM
                                    _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_to_int8_xint (char *pres,
                                     BID_UINT32 *
                                     px _EXC_FLAGS_PARAM
                                     _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_to_int8_floor (char *pres,
                                      BID_UINT32 *
                                      px _EXC_FLAGS_PARAM
                                      _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_to_int8_xfloor (char *pres,
                                       BID_UINT32 *
                                       px _EXC_FLAGS_PARAM
                                       _EXC_MASKS_PARAM
                                       _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_to_int8_ceil (char *pres,
                                     BID_UINT32 *
                                     px _EXC_FLAGS_PARAM
                                     _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_to_int8_xceil (char *pres,
                                      BID_UINT32 *
                                      px _EXC_FLAGS_PARAM
                                      _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_to_int16_rnint (short *pres,
                                       BID_UINT32 *
                                       px _EXC_FLAGS_PARAM
                                       _EXC_MASKS_PARAM
                                       _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_to_int16_xrnint (short *pres,
                                        BID_UINT32 *
                                        px _EXC_FLAGS_PARAM
                                        _EXC_MASKS_PARAM
                                        _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_to_int16_rninta (short *pres,
                                        BID_UINT32 *
                                        px _EXC_FLAGS_PARAM
                                        _EXC_MASKS_PARAM
                                        _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_to_int16_xrninta (short *pres,
                                         BID_UINT32 *
                                         px _EXC_FLAGS_PARAM
                                         _EXC_MASKS_PARAM
                                         _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_to_int16_int (short *pres,
                                     BID_UINT32 *
                                     px _EXC_FLAGS_PARAM
                                     _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_to_int16_xint (short *pres,
                                      BID_UINT32 *
                                      px _EXC_FLAGS_PARAM
                                      _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_to_int16_floor (short *pres,
                                       BID_UINT32 *
                                       px _EXC_FLAGS_PARAM
                                       _EXC_MASKS_PARAM
                                       _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_to_int16_xfloor (short *pres,
                                        BID_UINT32 *
                                        px _EXC_FLAGS_PARAM
                                        _EXC_MASKS_PARAM
                                        _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_to_int16_ceil (short *pres,
                                      BID_UINT32 *
                                      px _EXC_FLAGS_PARAM
                                      _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_to_int16_xceil (short *pres,
                                       BID_UINT32 *
                                       px _EXC_FLAGS_PARAM
                                       _EXC_MASKS_PARAM
                                       _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_to_uint8_rnint (unsigned char *pres,
                                       BID_UINT32 *
                                       px _EXC_FLAGS_PARAM
                                       _EXC_MASKS_PARAM
                                       _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_to_uint8_xrnint (unsigned char *pres,
                                        BID_UINT32 *
                                        px _EXC_FLAGS_PARAM
                                        _EXC_MASKS_PARAM
                                        _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_to_uint8_rninta (unsigned char *pres,
                                        BID_UINT32 *
                                        px _EXC_FLAGS_PARAM
                                        _EXC_MASKS_PARAM
                                        _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_to_uint8_xrninta (unsigned char *pres,
                                         BID_UINT32 *
                                         px _EXC_FLAGS_PARAM
                                         _EXC_MASKS_PARAM
                                         _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_to_uint8_int (unsigned char *pres,
                                     BID_UINT32 *
                                     px _EXC_FLAGS_PARAM
                                     _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_to_uint8_xint (unsigned char *pres,
                                      BID_UINT32 *
                                      px _EXC_FLAGS_PARAM
                                      _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_to_uint8_floor (unsigned char *pres,
                                       BID_UINT32 *
                                       px _EXC_FLAGS_PARAM
                                       _EXC_MASKS_PARAM
                                       _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_to_uint8_xfloor (unsigned char *pres,
                                        BID_UINT32 *
                                        px _EXC_FLAGS_PARAM
                                        _EXC_MASKS_PARAM
                                        _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_to_uint8_ceil (unsigned char *pres,
                                      BID_UINT32 *
                                      px _EXC_FLAGS_PARAM
                                      _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_to_uint8_xceil (unsigned char *pres,
                                       BID_UINT32 *
                                       px _EXC_FLAGS_PARAM
                                       _EXC_MASKS_PARAM
                                       _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_to_uint16_rnint (unsigned short *pres,
                                        BID_UINT32 *
                                        px _EXC_FLAGS_PARAM
                                        _EXC_MASKS_PARAM
                                        _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_to_uint16_xrnint (unsigned short *pres,
                                         BID_UINT32 *
                                         px _EXC_FLAGS_PARAM
                                         _EXC_MASKS_PARAM
                                         _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_to_uint16_rninta (unsigned short *pres,
                                         BID_UINT32 *
                                         px _EXC_FLAGS_PARAM
                                         _EXC_MASKS_PARAM
                                         _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_to_uint16_xrninta (unsigned short *pres,
                                          BID_UINT32 *
                                          px _EXC_FLAGS_PARAM
                                          _EXC_MASKS_PARAM
                                          _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_to_uint16_int (unsigned short *pres,
                                      BID_UINT32 *
                                      px _EXC_FLAGS_PARAM
                                      _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_to_uint16_xint (unsigned short *pres,
                                       BID_UINT32 *
                                       px _EXC_FLAGS_PARAM
                                       _EXC_MASKS_PARAM
                                       _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_to_uint16_floor (unsigned short *pres,
                                        BID_UINT32 *
                                        px _EXC_FLAGS_PARAM
                                        _EXC_MASKS_PARAM
                                        _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_to_uint16_xfloor (unsigned short *pres,
                                         BID_UINT32 *
                                         px _EXC_FLAGS_PARAM
                                         _EXC_MASKS_PARAM
                                         _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_to_uint16_ceil (unsigned short *pres,
                                       BID_UINT32 *
                                       px _EXC_FLAGS_PARAM
                                       _EXC_MASKS_PARAM
                                       _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_to_uint16_xceil (unsigned short *pres,
                                        BID_UINT32 *
                                        px _EXC_FLAGS_PARAM
                                        _EXC_MASKS_PARAM
                                        _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_to_uint32_rnint (unsigned int *pres,
                                        BID_UINT32 *
                                        px _EXC_FLAGS_PARAM
                                        _EXC_MASKS_PARAM
                                        _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_to_uint32_xrnint (unsigned int *pres,
                                         BID_UINT32 *
                                         px _EXC_FLAGS_PARAM
                                         _EXC_MASKS_PARAM
                                         _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_to_uint32_rninta (unsigned int *pres,
                                         BID_UINT32 *
                                         px _EXC_FLAGS_PARAM
                                         _EXC_MASKS_PARAM
                                         _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_to_uint32_xrninta (unsigned int *pres,
                                          BID_UINT32 *
                                          px _EXC_FLAGS_PARAM
                                          _EXC_MASKS_PARAM
                                          _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_to_uint32_int (unsigned int *pres,
                                      BID_UINT32 *
                                      px _EXC_FLAGS_PARAM
                                      _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_to_uint32_xint (unsigned int *pres,
                                       BID_UINT32 *
                                       px _EXC_FLAGS_PARAM
                                       _EXC_MASKS_PARAM
                                       _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_to_uint32_floor (unsigned int *pres,
                                        BID_UINT32 *
                                        px _EXC_FLAGS_PARAM
                                        _EXC_MASKS_PARAM
                                        _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_to_uint32_xfloor (unsigned int *pres,
                                         BID_UINT32 *
                                         px _EXC_FLAGS_PARAM
                                         _EXC_MASKS_PARAM
                                         _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_to_uint32_ceil (unsigned int *pres,
                                       BID_UINT32 *
                                       px _EXC_FLAGS_PARAM
                                       _EXC_MASKS_PARAM
                                       _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_to_uint32_xceil (unsigned int *pres,
                                        BID_UINT32 *
                                        px _EXC_FLAGS_PARAM
                                        _EXC_MASKS_PARAM
                                        _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_to_int64_rnint (BID_SINT64 * pres,
                                       BID_UINT32 *
                                       px _EXC_FLAGS_PARAM
                                       _EXC_MASKS_PARAM
                                       _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_to_int64_xrnint (BID_SINT64 * pres,
                                        BID_UINT32 *
                                        px _EXC_FLAGS_PARAM
                                        _EXC_MASKS_PARAM
                                        _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_to_int64_rninta (BID_SINT64 * pres,
                                        BID_UINT32 *
                                        px _EXC_FLAGS_PARAM
                                        _EXC_MASKS_PARAM
                                        _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_to_int64_xrninta (BID_SINT64 * pres,
                                         BID_UINT32 *
                                         px _EXC_FLAGS_PARAM
                                         _EXC_MASKS_PARAM
                                         _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_to_int64_int (BID_SINT64 * pres,
                                     BID_UINT32 *
                                     px _EXC_FLAGS_PARAM
                                     _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_to_int64_xint (BID_SINT64 * pres,
                                      BID_UINT32 *
                                      px _EXC_FLAGS_PARAM
                                      _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_to_int64_floor (BID_SINT64 * pres,
                                       BID_UINT32 *
                                       px _EXC_FLAGS_PARAM
                                       _EXC_MASKS_PARAM
                                       _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_to_int64_xfloor (BID_SINT64 * pres,
                                        BID_UINT32 *
                                        px _EXC_FLAGS_PARAM
                                        _EXC_MASKS_PARAM
                                        _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_to_int64_ceil (BID_SINT64 * pres,
                                      BID_UINT32 *
                                      px _EXC_FLAGS_PARAM
                                      _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_to_int64_xceil (BID_SINT64 * pres,
                                       BID_UINT32 *
                                       px _EXC_FLAGS_PARAM
                                       _EXC_MASKS_PARAM
                                       _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_to_uint64_rnint (BID_UINT64 * pres,
                                        BID_UINT32 *
                                        px _EXC_FLAGS_PARAM
                                        _EXC_MASKS_PARAM
                                        _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_to_uint64_xrnint (BID_UINT64 * pres,
                                         BID_UINT32 *
                                         px _EXC_FLAGS_PARAM
                                         _EXC_MASKS_PARAM
                                         _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_to_uint64_rninta (BID_UINT64 * pres,
                                         BID_UINT32 *
                                         px _EXC_FLAGS_PARAM
                                         _EXC_MASKS_PARAM
                                         _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_to_uint64_xrninta (BID_UINT64 * pres,
                                          BID_UINT32 *
                                          px _EXC_FLAGS_PARAM
                                          _EXC_MASKS_PARAM
                                          _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_to_uint64_int (BID_UINT64 * pres,
                                      BID_UINT32 *
                                      px _EXC_FLAGS_PARAM
                                      _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_to_uint64_xint (BID_UINT64 * pres,
                                       BID_UINT32 *
                                       px _EXC_FLAGS_PARAM
                                       _EXC_MASKS_PARAM
                                       _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_to_uint64_floor (BID_UINT64 * pres,
                                        BID_UINT32 *
                                        px _EXC_FLAGS_PARAM
                                        _EXC_MASKS_PARAM
                                        _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_to_uint64_xfloor (BID_UINT64 * pres,
                                         BID_UINT32 *
                                         px _EXC_FLAGS_PARAM
                                         _EXC_MASKS_PARAM
                                         _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_to_uint64_ceil (BID_UINT64 * pres,
                                       BID_UINT32 *
                                       px _EXC_FLAGS_PARAM
                                       _EXC_MASKS_PARAM
                                       _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_to_uint64_xceil (BID_UINT64 * pres,
                                        BID_UINT32 *
                                        px _EXC_FLAGS_PARAM
                                        _EXC_MASKS_PARAM
                                        _EXC_INFO_PARAM);

     BID_EXTERN_C void bid64_quiet_equal (int *pres, BID_UINT64 * px, BID_UINT64 * py
                                    _EXC_FLAGS_PARAM _EXC_MASKS_PARAM
                                    _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_quiet_greater (int *pres, BID_UINT64 * px,
                                      BID_UINT64 *
                                      py _EXC_FLAGS_PARAM
                                      _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_quiet_greater_equal (int *pres, BID_UINT64 * px,
                                            BID_UINT64 *
                                            py _EXC_FLAGS_PARAM
                                            _EXC_MASKS_PARAM
                                            _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_quiet_greater_unordered (int *pres, BID_UINT64 * px,
                                                BID_UINT64 *
                                                py _EXC_FLAGS_PARAM
                                                _EXC_MASKS_PARAM
                                                _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_quiet_less (int *pres, BID_UINT64 * px,
                                   BID_UINT64 *
                                   py _EXC_FLAGS_PARAM _EXC_MASKS_PARAM
                                   _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_quiet_less_equal (int *pres, BID_UINT64 * px,
                                         BID_UINT64 *
                                         py _EXC_FLAGS_PARAM
                                         _EXC_MASKS_PARAM
                                         _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_quiet_less_unordered (int *pres, BID_UINT64 * px,
                                             BID_UINT64 *
                                             py _EXC_FLAGS_PARAM
                                             _EXC_MASKS_PARAM
                                             _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_quiet_not_equal (int *pres, BID_UINT64 * px,
                                        BID_UINT64 *
                                        py _EXC_FLAGS_PARAM
                                        _EXC_MASKS_PARAM
                                        _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_quiet_not_greater (int *pres, BID_UINT64 * px,
                                          BID_UINT64 *
                                          py _EXC_FLAGS_PARAM
                                          _EXC_MASKS_PARAM
                                          _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_quiet_not_less (int *pres, BID_UINT64 * px,
                                       BID_UINT64 *
                                       py _EXC_FLAGS_PARAM
                                       _EXC_MASKS_PARAM
                                       _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_quiet_ordered (int *pres, BID_UINT64 * px,
                                      BID_UINT64 *
                                      py _EXC_FLAGS_PARAM
                                      _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_quiet_unordered (int *pres, BID_UINT64 * px,
                                        BID_UINT64 *
                                        py _EXC_FLAGS_PARAM
                                        _EXC_MASKS_PARAM
                                        _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_signaling_greater (int *pres, BID_UINT64 * px,
                                          BID_UINT64 *
                                          py _EXC_FLAGS_PARAM
                                          _EXC_MASKS_PARAM
                                          _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_signaling_greater_equal (int *pres, BID_UINT64 * px,
                                                BID_UINT64 *
                                                py _EXC_FLAGS_PARAM
                                                _EXC_MASKS_PARAM
                                                _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_signaling_greater_unordered (int *pres,
                                                    BID_UINT64 * px,
                                                    BID_UINT64 *
                                                    py _EXC_FLAGS_PARAM
                                                    _EXC_MASKS_PARAM
                                                    _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_signaling_less (int *pres, BID_UINT64 * px,
                                       BID_UINT64 *
                                       py _EXC_FLAGS_PARAM
                                       _EXC_MASKS_PARAM
                                       _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_signaling_less_equal (int *pres, BID_UINT64 * px,
                                             BID_UINT64 *
                                             py _EXC_FLAGS_PARAM
                                             _EXC_MASKS_PARAM
                                             _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_signaling_less_unordered (int *pres, BID_UINT64 * px,
                                                 BID_UINT64 *
                                                 py _EXC_FLAGS_PARAM
                                                 _EXC_MASKS_PARAM
                                                 _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_signaling_not_greater (int *pres, BID_UINT64 * px,
                                              BID_UINT64 *
                                              py _EXC_FLAGS_PARAM
                                              _EXC_MASKS_PARAM
                                              _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_signaling_not_less (int *pres, BID_UINT64 * px,
                                           BID_UINT64 *
                                           py _EXC_FLAGS_PARAM
                                           _EXC_MASKS_PARAM
                                           _EXC_INFO_PARAM);

     BID_EXTERN_C void bid128_quiet_equal (int *pres, BID_UINT128 * px,
                                     BID_UINT128 *
                                     py _EXC_FLAGS_PARAM
                                     _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_quiet_greater (int *pres, BID_UINT128 * px,
                                       BID_UINT128 *
                                       py _EXC_FLAGS_PARAM
                                       _EXC_MASKS_PARAM
                                       _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_quiet_greater_equal (int *pres, BID_UINT128 * px,
                                             BID_UINT128 *
                                             py _EXC_FLAGS_PARAM
                                             _EXC_MASKS_PARAM
                                             _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_quiet_greater_unordered (int *pres,
                                                 BID_UINT128 * px,
                                                 BID_UINT128 *
                                                 py _EXC_FLAGS_PARAM
                                                 _EXC_MASKS_PARAM
                                                 _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_quiet_less (int *pres, BID_UINT128 * px,
                                    BID_UINT128 *
                                    py _EXC_FLAGS_PARAM _EXC_MASKS_PARAM
                                    _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_quiet_less_equal (int *pres, BID_UINT128 * px,
                                          BID_UINT128 *
                                          py _EXC_FLAGS_PARAM
                                          _EXC_MASKS_PARAM
                                          _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_quiet_less_unordered (int *pres, BID_UINT128 * px,
                                              BID_UINT128 *
                                              py _EXC_FLAGS_PARAM
                                              _EXC_MASKS_PARAM
                                              _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_quiet_not_equal (int *pres, BID_UINT128 * px,
                                         BID_UINT128 *
                                         py _EXC_FLAGS_PARAM
                                         _EXC_MASKS_PARAM
                                         _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_quiet_not_greater (int *pres, BID_UINT128 * px,
                                           BID_UINT128 *
                                           py _EXC_FLAGS_PARAM
                                           _EXC_MASKS_PARAM
                                           _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_quiet_not_less (int *pres, BID_UINT128 * px,
                                        BID_UINT128 *
                                        py _EXC_FLAGS_PARAM
                                        _EXC_MASKS_PARAM
                                        _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_quiet_ordered (int *pres, BID_UINT128 * px,
                                       BID_UINT128 *
                                       py _EXC_FLAGS_PARAM
                                       _EXC_MASKS_PARAM
                                       _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_quiet_unordered (int *pres, BID_UINT128 * px,
                                         BID_UINT128 *
                                         py _EXC_FLAGS_PARAM
                                         _EXC_MASKS_PARAM
                                         _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_signaling_greater (int *pres, BID_UINT128 * px,
                                           BID_UINT128 *
                                           py _EXC_FLAGS_PARAM
                                           _EXC_MASKS_PARAM
                                           _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_signaling_greater_equal (int *pres,
                                                 BID_UINT128 * px,
                                                 BID_UINT128 *
                                                 py _EXC_FLAGS_PARAM
                                                 _EXC_MASKS_PARAM
                                                 _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_signaling_greater_unordered (int *pres,
                                                     BID_UINT128 * px,
                                                     BID_UINT128 *
                                                     py _EXC_FLAGS_PARAM
                                                     _EXC_MASKS_PARAM
                                                     _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_signaling_less (int *pres, BID_UINT128 * px,
                                        BID_UINT128 *
                                        py _EXC_FLAGS_PARAM
                                        _EXC_MASKS_PARAM
                                        _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_signaling_less_equal (int *pres, BID_UINT128 * px,
                                              BID_UINT128 *
                                              py _EXC_FLAGS_PARAM
                                              _EXC_MASKS_PARAM
                                              _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_signaling_less_unordered (int *pres,
                                                  BID_UINT128 * px,
                                                  BID_UINT128 *
                                                  py _EXC_FLAGS_PARAM
                                                  _EXC_MASKS_PARAM
                                                  _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_signaling_not_greater (int *pres, BID_UINT128 * px,
                                               BID_UINT128 *
                                               py _EXC_FLAGS_PARAM
                                               _EXC_MASKS_PARAM
                                               _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_signaling_not_less (int *pres, BID_UINT128 * px,
                                            BID_UINT128 *
                                            py _EXC_FLAGS_PARAM
                                            _EXC_MASKS_PARAM
                                            _EXC_INFO_PARAM);

     BID_EXTERN_C void bid32_round_integral_exact (BID_UINT32 * pres, BID_UINT32 * px
                                             _RND_MODE_PARAM
                                             _EXC_FLAGS_PARAM
                                             _EXC_MASKS_PARAM
                                             _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_round_integral_nearest_even (BID_UINT32 * pres,
                                                    BID_UINT32 *
                                                    px _EXC_FLAGS_PARAM
                                                    _EXC_MASKS_PARAM
                                                    _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_round_integral_negative (BID_UINT32 * pres,
                                                BID_UINT32 *
                                                px _EXC_FLAGS_PARAM
                                                _EXC_MASKS_PARAM
                                                _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_round_integral_positive (BID_UINT32 * pres,
                                                BID_UINT32 *
                                                px _EXC_FLAGS_PARAM
                                                _EXC_MASKS_PARAM
                                                _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_round_integral_zero (BID_UINT32 * pres,
                                            BID_UINT32 *
                                            px _EXC_FLAGS_PARAM
                                            _EXC_MASKS_PARAM
                                            _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_round_integral_nearest_away (BID_UINT32 * pres,
                                                    BID_UINT32 *
                                                    px _EXC_FLAGS_PARAM
                                                    _EXC_MASKS_PARAM
                                                    _EXC_INFO_PARAM);

     BID_EXTERN_C void bid64_round_integral_exact (BID_UINT64 * pres, BID_UINT64 * px
                                             _RND_MODE_PARAM
                                             _EXC_FLAGS_PARAM
                                             _EXC_MASKS_PARAM
                                             _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_round_integral_nearest_even (BID_UINT64 * pres,
                                                    BID_UINT64 *
                                                    px _EXC_FLAGS_PARAM
                                                    _EXC_MASKS_PARAM
                                                    _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_round_integral_negative (BID_UINT64 * pres,
                                                BID_UINT64 *
                                                px _EXC_FLAGS_PARAM
                                                _EXC_MASKS_PARAM
                                                _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_round_integral_positive (BID_UINT64 * pres,
                                                BID_UINT64 *
                                                px _EXC_FLAGS_PARAM
                                                _EXC_MASKS_PARAM
                                                _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_round_integral_zero (BID_UINT64 * pres,
                                            BID_UINT64 *
                                            px _EXC_FLAGS_PARAM
                                            _EXC_MASKS_PARAM
                                            _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_round_integral_nearest_away (BID_UINT64 * pres,
                                                    BID_UINT64 *
                                                    px _EXC_FLAGS_PARAM
                                                    _EXC_MASKS_PARAM
                                                    _EXC_INFO_PARAM);

     BID_EXTERN_C void bid128_round_integral_exact (BID_UINT128 * pres,
                                              BID_UINT128 *
                                              px _RND_MODE_PARAM
                                              _EXC_FLAGS_PARAM
                                              _EXC_MASKS_PARAM
                                              _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_round_integral_nearest_even (BID_UINT128 * pres,
                                                     BID_UINT128 *
                                                     px _EXC_FLAGS_PARAM
                                                     _EXC_MASKS_PARAM
                                                     _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_round_integral_negative (BID_UINT128 * pres,
                                                 BID_UINT128 *
                                                 px _EXC_FLAGS_PARAM
                                                 _EXC_MASKS_PARAM
                                                 _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_round_integral_positive (BID_UINT128 * pres,
                                                 BID_UINT128 *
                                                 px _EXC_FLAGS_PARAM
                                                 _EXC_MASKS_PARAM
                                                 _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_round_integral_zero (BID_UINT128 * pres,
                                             BID_UINT128 *
                                             px _EXC_FLAGS_PARAM
                                             _EXC_MASKS_PARAM
                                             _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_round_integral_nearest_away (BID_UINT128 * pres,
                                                     BID_UINT128 *
                                                     px _EXC_FLAGS_PARAM
                                                     _EXC_MASKS_PARAM
                                                     _EXC_INFO_PARAM);

     BID_EXTERN_C void bid32_nextup (BID_UINT32 * pres, BID_UINT32 * px
                               _EXC_FLAGS_PARAM _EXC_MASKS_PARAM
                               _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_nextdown (BID_UINT32 * pres,
                                 BID_UINT32 *
                                 px _EXC_FLAGS_PARAM _EXC_MASKS_PARAM
                                 _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_nextafter (BID_UINT32 * pres, BID_UINT32 * px,
                                  BID_UINT32 *
                                  py _EXC_FLAGS_PARAM _EXC_MASKS_PARAM
                                  _EXC_INFO_PARAM);

     BID_EXTERN_C void bid64_nextup (BID_UINT64 * pres, BID_UINT64 * px
                               _EXC_FLAGS_PARAM _EXC_MASKS_PARAM
                               _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_nextdown (BID_UINT64 * pres,
                                 BID_UINT64 *
                                 px _EXC_FLAGS_PARAM _EXC_MASKS_PARAM
                                 _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_nextafter (BID_UINT64 * pres, BID_UINT64 * px,
                                  BID_UINT64 *
                                  py _EXC_FLAGS_PARAM _EXC_MASKS_PARAM
                                  _EXC_INFO_PARAM);

     BID_EXTERN_C void bid128_nextup (BID_UINT128 * pres, BID_UINT128 * px
                                _EXC_FLAGS_PARAM _EXC_MASKS_PARAM
                                _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_nextdown (BID_UINT128 * pres,
                                  BID_UINT128 *
                                  px _EXC_FLAGS_PARAM _EXC_MASKS_PARAM
                                  _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_nextafter (BID_UINT128 * pres, BID_UINT128 * px,
                                   BID_UINT128 *
                                   py _EXC_FLAGS_PARAM _EXC_MASKS_PARAM
                                   _EXC_INFO_PARAM);

     BID_EXTERN_C void bid32_minnum (BID_UINT32 * pres, BID_UINT32 * px, BID_UINT32 * py
         _EXC_FLAGS_PARAM _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_minnum_mag (BID_UINT32 * pres, BID_UINT32 * px,
         BID_UINT32 * py _EXC_FLAGS_PARAM _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_maxnum (BID_UINT32 * pres, BID_UINT32 * px, BID_UINT32 * py
         _EXC_FLAGS_PARAM _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_maxnum_mag (BID_UINT32 * pres, BID_UINT32 * px,
         BID_UINT32 * py _EXC_FLAGS_PARAM _EXC_MASKS_PARAM _EXC_INFO_PARAM);

     BID_EXTERN_C void bid64_minnum (BID_UINT64 * pres, BID_UINT64 * px, BID_UINT64 * py
                               _EXC_FLAGS_PARAM _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_minnum_mag (BID_UINT64 * pres, BID_UINT64 * px,
                                   BID_UINT64 * py _EXC_FLAGS_PARAM _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_maxnum (BID_UINT64 * pres, BID_UINT64 * px, BID_UINT64 * py
                               _EXC_FLAGS_PARAM _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_maxnum_mag (BID_UINT64 * pres, BID_UINT64 * px,
                                   BID_UINT64 * py _EXC_FLAGS_PARAM _EXC_MASKS_PARAM _EXC_INFO_PARAM);

     BID_EXTERN_C void bid128_minnum (BID_UINT128 * pres, BID_UINT128 * px,
                                BID_UINT128 * py _EXC_FLAGS_PARAM _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_minnum_mag (BID_UINT128 * pres, BID_UINT128 * px,
                                    BID_UINT128 * py _EXC_FLAGS_PARAM _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_maxnum (BID_UINT128 * pres, BID_UINT128 * px,
                                BID_UINT128 * py _EXC_FLAGS_PARAM _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_maxnum_mag (BID_UINT128 * pres, BID_UINT128 * px,
                                    BID_UINT128 * py _EXC_FLAGS_PARAM _EXC_MASKS_PARAM _EXC_INFO_PARAM);

     BID_EXTERN_C void bid32_from_int32 (BID_UINT32 * pres, int *px
         _RND_MODE_PARAM _EXC_FLAGS_PARAM _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_from_uint32 (BID_UINT32 * pres, unsigned int *px
         _RND_MODE_PARAM _EXC_FLAGS_PARAM _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_from_int64 (BID_UINT32 * pres, BID_SINT64 * px
                                   _RND_MODE_PARAM _EXC_FLAGS_PARAM
                                   _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_from_uint64 (BID_UINT32 * pres,
                                    BID_UINT64 *
                                    px _RND_MODE_PARAM _EXC_FLAGS_PARAM
                                    _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_from_int32 (BID_UINT64 * pres, int *px
                                   _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_from_uint32 (BID_UINT64 * pres, unsigned int *px
                                    _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_from_int64 (BID_UINT64 * pres, BID_SINT64 * px
                                   _RND_MODE_PARAM _EXC_FLAGS_PARAM
                                   _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_from_uint64 (BID_UINT64 * pres,
                                    BID_UINT64 *
                                    px _RND_MODE_PARAM _EXC_FLAGS_PARAM
                                    _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_from_int32 (BID_UINT128 * pres,
                                    int *px _EXC_MASKS_PARAM
                                    _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_from_uint32 (BID_UINT128 * pres,
                                     unsigned int *px _EXC_MASKS_PARAM
                                     _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_from_int64 (BID_UINT128 * pres,
                                    BID_SINT64 *
                                    px _EXC_MASKS_PARAM
                                    _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_from_uint64 (BID_UINT128 * pres,
                                     BID_UINT64 *
                                     px _EXC_MASKS_PARAM
                                     _EXC_INFO_PARAM);

     BID_EXTERN_C void bid32_isSigned (int *pres, BID_UINT32 * px
                                 _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_isNormal (int *pres, BID_UINT32 * px
                                 _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_isSubnormal (int *pres, BID_UINT32 * px
                                    _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_isFinite (int *pres, BID_UINT32 * px
                                 _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_isZero (int *pres, BID_UINT32 * px
                               _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_isInf (int *pres, BID_UINT32 * px
                              _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_isSignaling (int *pres, BID_UINT32 * px
                                    _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_isCanonical (int *pres, BID_UINT32 * px
                                    _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_isNaN (int *pres, BID_UINT32 * px
                              _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_copy (BID_UINT32 * pres, BID_UINT32 * px
                             _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_negate (BID_UINT32 * pres, BID_UINT32 * px
                               _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_abs (BID_UINT32 * pres, BID_UINT32 * px
                            _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_copySign (BID_UINT32 * pres, BID_UINT32 * px, BID_UINT32 * py
                                 _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_class (int *pres, BID_UINT32 * px
                              _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_sameQuantum (int *pres, BID_UINT32 * px, BID_UINT32 * py
                                    _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_totalOrder (int *pres, BID_UINT32 * px, BID_UINT32 * py
                                   _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_totalOrderMag (int *pres, BID_UINT32 * px,
                                      BID_UINT32 *
                                      py _EXC_MASKS_PARAM
                                      _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_radix (int *pres,
                              BID_UINT32 *
                              px _EXC_MASKS_PARAM _EXC_INFO_PARAM);

     BID_EXTERN_C void bid64_isSigned (int *pres, BID_UINT64 * px
                                 _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_isNormal (int *pres, BID_UINT64 * px
                                 _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_isSubnormal (int *pres, BID_UINT64 * px
                                    _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_isFinite (int *pres, BID_UINT64 * px
                                 _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_isZero (int *pres, BID_UINT64 * px
                               _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_isInf (int *pres, BID_UINT64 * px
                              _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_isSignaling (int *pres, BID_UINT64 * px
                                    _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_isCanonical (int *pres, BID_UINT64 * px
                                    _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_isNaN (int *pres, BID_UINT64 * px
                              _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_copy (BID_UINT64 * pres, BID_UINT64 * px
                             _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_negate (BID_UINT64 * pres, BID_UINT64 * px
                               _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_abs (BID_UINT64 * pres, BID_UINT64 * px
                            _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_copySign (BID_UINT64 * pres, BID_UINT64 * px, BID_UINT64 * py
                                 _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_class (int *pres, BID_UINT64 * px
                              _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_sameQuantum (int *pres, BID_UINT64 * px, BID_UINT64 * py
                                    _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_totalOrder (int *pres, BID_UINT64 * px, BID_UINT64 * py
                                   _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_totalOrderMag (int *pres, BID_UINT64 * px,
                                      BID_UINT64 *
                                      py _EXC_MASKS_PARAM
                                      _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_radix (int *pres,
                              BID_UINT64 *
                              px _EXC_MASKS_PARAM _EXC_INFO_PARAM);

     BID_EXTERN_C void bid128_isSigned (int *pres, BID_UINT128 * px
                                  _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_isNormal (int *pres, BID_UINT128 * px
                                  _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_isSubnormal (int *pres, BID_UINT128 * px
                                     _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_isFinite (int *pres, BID_UINT128 * px
                                  _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_isZero (int *pres, BID_UINT128 * px
                                _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_isInf (int *pres, BID_UINT128 * px
                               _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_isSignaling (int *pres, BID_UINT128 * px
                                     _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_isCanonical (int *pres, BID_UINT128 * px
                                     _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_isNaN (int *pres, BID_UINT128 * px
                               _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_copy (BID_UINT128 * pres, BID_UINT128 * px
                              _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_negate (BID_UINT128 * pres, BID_UINT128 * px
                                _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_abs (BID_UINT128 * pres, BID_UINT128 * px
                             _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_copySign (BID_UINT128 * pres, BID_UINT128 * px,
                                  BID_UINT128 *
                                  py _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_class (int *pres,
                               BID_UINT128 *
                               px _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_sameQuantum (int *pres, BID_UINT128 * px,
                                     BID_UINT128 *
                                     py _EXC_MASKS_PARAM
                                     _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_totalOrder (int *pres, BID_UINT128 * px,
                                    BID_UINT128 *
                                    py _EXC_MASKS_PARAM
                                    _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_totalOrderMag (int *pres, BID_UINT128 * px,
                                       BID_UINT128 *
                                       py _EXC_MASKS_PARAM
                                       _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_radix (int *pres,
                               BID_UINT128 *
                               px _EXC_MASKS_PARAM _EXC_INFO_PARAM);

     BID_EXTERN_C void bid32_rem (BID_UINT32 * pres, BID_UINT32 * px, BID_UINT32 * py
                            _EXC_FLAGS_PARAM
                            _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_ilogb (int * pres, BID_UINT32 * px
                             _EXC_FLAGS_PARAM _EXC_MASKS_PARAM
                             _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_scalbn (BID_UINT32 * pres, BID_UINT32 * px,
                              int *pn _RND_MODE_PARAM _EXC_FLAGS_PARAM
                              _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_ldexp (BID_UINT32 * pres, BID_UINT32 * px,
                              int *pn _RND_MODE_PARAM _EXC_FLAGS_PARAM
                              _EXC_MASKS_PARAM _EXC_INFO_PARAM);

     BID_EXTERN_C void bid64_rem (BID_UINT64 * pres, BID_UINT64 * px, BID_UINT64 * py
                            _EXC_FLAGS_PARAM
                            _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_ilogb (int * pres, BID_UINT64 * px
                             _EXC_FLAGS_PARAM _EXC_MASKS_PARAM
                             _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_scalbn (BID_UINT64 * pres, BID_UINT64 * px,
                              int *pn _RND_MODE_PARAM _EXC_FLAGS_PARAM
                              _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_ldexp (BID_UINT64 * pres, BID_UINT64 * px,
                              int *pn _RND_MODE_PARAM _EXC_FLAGS_PARAM
                              _EXC_MASKS_PARAM _EXC_INFO_PARAM);

     BID_EXTERN_C void bid128_rem (BID_UINT128 * pres, BID_UINT128 * px, BID_UINT128 * py
                             _EXC_FLAGS_PARAM
                             _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_ilogb (int * pres, BID_UINT128 * px
                              _EXC_FLAGS_PARAM
                              _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_scalbn (BID_UINT128 * pres, BID_UINT128 * px,
                               int *pn _RND_MODE_PARAM _EXC_FLAGS_PARAM
                               _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_ldexp (BID_UINT128 * pres, BID_UINT128 * px,
                               int *pn _RND_MODE_PARAM _EXC_FLAGS_PARAM
                               _EXC_MASKS_PARAM _EXC_INFO_PARAM);

     BID_EXTERN_C void bid32_to_bid64 (BID_UINT64 * pres,
                                 BID_UINT32 *
                                 px _EXC_FLAGS_PARAM _EXC_MASKS_PARAM
                                 _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_to_bid128 (BID_UINT128 * pres,
                                  BID_UINT32 *
                                  px _EXC_FLAGS_PARAM _EXC_MASKS_PARAM
                                  _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_to_bid128 (BID_UINT128 * pres,
                                  BID_UINT64 *
                                  px _EXC_FLAGS_PARAM _EXC_MASKS_PARAM
                                  _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_to_bid32 (BID_UINT32 * pres,
                                 BID_UINT64 *
                                 px _RND_MODE_PARAM _EXC_FLAGS_PARAM
                                 _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_to_bid32 (BID_UINT32 * pres,
                                  BID_UINT128 *
                                  px _RND_MODE_PARAM _EXC_FLAGS_PARAM
                                  _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_to_bid64 (BID_UINT64 * pres,
                                  BID_UINT128 *
                                  px _RND_MODE_PARAM _EXC_FLAGS_PARAM
                                  _EXC_MASKS_PARAM _EXC_INFO_PARAM);

     BID_EXTERN_C void bid32_from_string (BID_UINT32 * pres, char *ps
                                    _RND_MODE_PARAM _EXC_FLAGS_PARAM
                                    _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_to_string (char *ps, BID_UINT32 * px
                                  _EXC_FLAGS_PARAM
                                  _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_from_string (BID_UINT64 * pres, char *ps
                                    _RND_MODE_PARAM _EXC_FLAGS_PARAM
                                    _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_to_string (char *ps, BID_UINT64 * px
                                  _EXC_FLAGS_PARAM
                                  _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_from_string (BID_UINT128 * pres, char *ps
                                     _RND_MODE_PARAM _EXC_FLAGS_PARAM
                                     _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_to_string (char *str, BID_UINT128 * px
                                   _EXC_FLAGS_PARAM
                                   _EXC_MASKS_PARAM _EXC_INFO_PARAM);

     BID_EXTERN_C void bid32_quantize (BID_UINT32 * pres, BID_UINT32 * px,
                                 BID_UINT32 *
                                 py _RND_MODE_PARAM _EXC_FLAGS_PARAM
                                 _EXC_MASKS_PARAM _EXC_INFO_PARAM);

     BID_EXTERN_C void bid64_quantize (BID_UINT64 * pres, BID_UINT64 * px,
                                 BID_UINT64 *
                                 py _RND_MODE_PARAM _EXC_FLAGS_PARAM
                                 _EXC_MASKS_PARAM _EXC_INFO_PARAM);

     BID_EXTERN_C void bid128_quantize (BID_UINT128 * pres, BID_UINT128 * px,
                                  BID_UINT128 *
                                  py _RND_MODE_PARAM _EXC_FLAGS_PARAM
                                  _EXC_MASKS_PARAM _EXC_INFO_PARAM);

     BID_EXTERN_C void bid128_to_binary32 (float *pres, BID_UINT128 * px
                                     _RND_MODE_PARAM _EXC_FLAGS_PARAM
                                     _EXC_MASKS_PARAM _EXC_INFO_PARAM);

     BID_EXTERN_C void bid128_to_binary64 (double *pres, BID_UINT128 * px
                                     _RND_MODE_PARAM _EXC_FLAGS_PARAM
                                     _EXC_MASKS_PARAM _EXC_INFO_PARAM);

     BID_EXTERN_C void bid128_to_binary80 (BINARY80 * pres, BID_UINT128 * px
                                     _RND_MODE_PARAM _EXC_FLAGS_PARAM
                                     _EXC_MASKS_PARAM _EXC_INFO_PARAM);

     BID_EXTERN_C void bid128_to_binary128 (BINARY128 * pres, BID_UINT128 * px
                                      _RND_MODE_PARAM _EXC_FLAGS_PARAM
                                      _EXC_MASKS_PARAM _EXC_INFO_PARAM);

     BID_EXTERN_C void binary128_to_bid32 (BID_UINT32 * pres, BINARY128 * px
                                     _RND_MODE_PARAM _EXC_FLAGS_PARAM
                                     _EXC_MASKS_PARAM _EXC_INFO_PARAM);

     BID_EXTERN_C void binary128_to_bid64 (BID_UINT64 * pres, BINARY128 * px
                                     _RND_MODE_PARAM _EXC_FLAGS_PARAM
                                     _EXC_MASKS_PARAM _EXC_INFO_PARAM);

     BID_EXTERN_C void binary128_to_bid128 (BID_UINT128 * pres, BINARY128 * px
                                      _RND_MODE_PARAM _EXC_FLAGS_PARAM
                                      _EXC_MASKS_PARAM _EXC_INFO_PARAM);

     BID_EXTERN_C void bid64_to_binary32 (float *pres, BID_UINT64 * px
                                    _RND_MODE_PARAM _EXC_FLAGS_PARAM
                                    _EXC_MASKS_PARAM _EXC_INFO_PARAM);

     BID_EXTERN_C void bid64_to_binary64 (double *pres, BID_UINT64 * px
                                    _RND_MODE_PARAM _EXC_FLAGS_PARAM
                                    _EXC_MASKS_PARAM _EXC_INFO_PARAM);

     BID_EXTERN_C void bid64_to_binary80 (BINARY80 * pres, BID_UINT64 * px
                                    _RND_MODE_PARAM _EXC_FLAGS_PARAM
                                    _EXC_MASKS_PARAM _EXC_INFO_PARAM);

     BID_EXTERN_C void bid64_to_binary128 (BINARY128 * pres, BID_UINT64 * px
                                     _RND_MODE_PARAM _EXC_FLAGS_PARAM
                                     _EXC_MASKS_PARAM _EXC_INFO_PARAM);

     BID_EXTERN_C void binary64_to_bid32 (BID_UINT32 * pres, double *px
                                    _RND_MODE_PARAM _EXC_FLAGS_PARAM
                                    _EXC_MASKS_PARAM _EXC_INFO_PARAM);

     BID_EXTERN_C void binary64_to_bid64 (BID_UINT64 * pres, double *px
                                    _RND_MODE_PARAM _EXC_FLAGS_PARAM
                                    _EXC_MASKS_PARAM _EXC_INFO_PARAM);

     BID_EXTERN_C void binary64_to_bid128 (BID_UINT128 * pres, double *px
                                     _RND_MODE_PARAM _EXC_FLAGS_PARAM
                                     _EXC_MASKS_PARAM _EXC_INFO_PARAM);

     BID_EXTERN_C void bid32_to_binary32 (float *pres, BID_UINT32 * px
                                    _RND_MODE_PARAM _EXC_FLAGS_PARAM
                                    _EXC_MASKS_PARAM _EXC_INFO_PARAM);

     BID_EXTERN_C void bid32_to_binary64 (double *pres, BID_UINT32 * px
                                    _RND_MODE_PARAM _EXC_FLAGS_PARAM
                                    _EXC_MASKS_PARAM _EXC_INFO_PARAM);

     BID_EXTERN_C void bid32_to_binary80 (BINARY80 * pres, BID_UINT32 * px
                                    _RND_MODE_PARAM _EXC_FLAGS_PARAM
                                    _EXC_MASKS_PARAM _EXC_INFO_PARAM);

     BID_EXTERN_C void bid32_to_binary128 (BINARY128 * pres, BID_UINT32 * px
                                     _RND_MODE_PARAM _EXC_FLAGS_PARAM
                                     _EXC_MASKS_PARAM _EXC_INFO_PARAM);

     BID_EXTERN_C void binary32_to_bid32 (BID_UINT32 * pres, float *px
                                    _RND_MODE_PARAM _EXC_FLAGS_PARAM
                                    _EXC_MASKS_PARAM _EXC_INFO_PARAM);

     BID_EXTERN_C void binary32_to_bid64 (BID_UINT64 * pres, float *px
                                    _RND_MODE_PARAM _EXC_FLAGS_PARAM
                                    _EXC_MASKS_PARAM _EXC_INFO_PARAM);

     BID_EXTERN_C void binary32_to_bid128 (BID_UINT128 * pres, float *px
                                     _RND_MODE_PARAM _EXC_FLAGS_PARAM
                                     _EXC_MASKS_PARAM _EXC_INFO_PARAM);

     BID_EXTERN_C void binary80_to_bid32 (BID_UINT32 * pres, BINARY80 * px
                                    _RND_MODE_PARAM _EXC_FLAGS_PARAM
                                    _EXC_MASKS_PARAM _EXC_INFO_PARAM);

     BID_EXTERN_C void binary80_to_bid64 (BID_UINT64 * pres, BINARY80 * px
                                    _RND_MODE_PARAM _EXC_FLAGS_PARAM
                                    _EXC_MASKS_PARAM _EXC_INFO_PARAM);

     BID_EXTERN_C void binary80_to_bid128 (BID_UINT128 * pres, BINARY80 * px
                                     _RND_MODE_PARAM _EXC_FLAGS_PARAM
                                     _EXC_MASKS_PARAM _EXC_INFO_PARAM);

     BID_EXTERN_C void bid_is754 (int *retval);

     BID_EXTERN_C void bid_is754R (int *retval);

     BID_EXTERN_C void bid_signalException (_IDEC_flags *
                                  pflagsmask _EXC_FLAGS_PARAM);

     BID_EXTERN_C void bid_lowerFlags (_IDEC_flags * pflagsmask _EXC_FLAGS_PARAM);

     BID_EXTERN_C void bid_testFlags (_IDEC_flags * praised,
                            _IDEC_flags * pflagsmask _EXC_FLAGS_PARAM);

     BID_EXTERN_C void bid_testSavedFlags (_IDEC_flags * praised,
                                 _IDEC_flags * psavedflags,
                                 _IDEC_flags * pflagsmask);

     BID_EXTERN_C void bid_restoreFlags (_IDEC_flags * pflagsvalues,
                               _IDEC_flags *
                               pflagsmask _EXC_FLAGS_PARAM);

     BID_EXTERN_C void bid_saveFlags (_IDEC_flags * pflagsvalues,
                            _IDEC_flags * pflagsmask _EXC_FLAGS_PARAM);

     BID_EXTERN_C void bid_getDecimalRoundingDirection (_IDEC_round *
                                       rounding_mode _RND_MODE_PARAM);

     BID_EXTERN_C void bid_setDecimalRoundingDirection (_IDEC_round *
                                       rounding_mode _RND_MODE_PARAM);

     BID_EXTERN_C void bid32_add (BID_UINT32 * pres, BID_UINT32 * px,
                            BID_UINT32 * py
                                _RND_MODE_PARAM _EXC_FLAGS_PARAM
                            _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_sub (BID_UINT32 * pres, BID_UINT32 * px,
                            BID_UINT32 * py
                            _RND_MODE_PARAM _EXC_FLAGS_PARAM
                            _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_mul (BID_UINT32 * pres, BID_UINT32 * px,
                            BID_UINT32 * py
                            _RND_MODE_PARAM _EXC_FLAGS_PARAM
                            _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_div (BID_UINT32 * pres, BID_UINT32 * px,
                            BID_UINT32 * py
                            _RND_MODE_PARAM _EXC_FLAGS_PARAM
                            _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_fma (BID_UINT32 * pres, BID_UINT32 * px,
                            BID_UINT32 * py, BID_UINT32 * pz
                            _RND_MODE_PARAM _EXC_FLAGS_PARAM
                            _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_sqrt (BID_UINT32 * pres,
                             BID_UINT32 * px
                                  _RND_MODE_PARAM _EXC_FLAGS_PARAM
                             _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_cbrt (BID_UINT32 * pres,
                             BID_UINT32 * px
                                  _RND_MODE_PARAM _EXC_FLAGS_PARAM
                             _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_quiet_equal (int *pres, BID_UINT32 * px, BID_UINT32 * py
                                    _EXC_FLAGS_PARAM _EXC_MASKS_PARAM
                                    _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_quiet_greater (int *pres, BID_UINT32 * px,
                                      BID_UINT32 *
                                      py _EXC_FLAGS_PARAM
                                      _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_quiet_greater_equal (int *pres, BID_UINT32 * px,
                                            BID_UINT32 *
                                            py _EXC_FLAGS_PARAM
                                            _EXC_MASKS_PARAM
                                            _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_quiet_greater_unordered (int *pres, BID_UINT32 * px,
                                                BID_UINT32 *
                                                py _EXC_FLAGS_PARAM
                                                _EXC_MASKS_PARAM
                                                _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_quiet_less (int *pres, BID_UINT32 * px,
                                   BID_UINT32 *
                                   py _EXC_FLAGS_PARAM _EXC_MASKS_PARAM
                                   _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_quiet_less_equal (int *pres, BID_UINT32 * px,
                                         BID_UINT32 *
                                         py _EXC_FLAGS_PARAM
                                         _EXC_MASKS_PARAM
                                         _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_quiet_less_unordered (int *pres, BID_UINT32 * px,
                                             BID_UINT32 *
                                             py _EXC_FLAGS_PARAM
                                             _EXC_MASKS_PARAM
                                             _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_quiet_not_equal (int *pres, BID_UINT32 * px,
                                        BID_UINT32 *
                                        py _EXC_FLAGS_PARAM
                                        _EXC_MASKS_PARAM
                                        _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_quiet_not_greater (int *pres, BID_UINT32 * px,
                                          BID_UINT32 *
                                          py _EXC_FLAGS_PARAM
                                          _EXC_MASKS_PARAM
                                          _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_quiet_not_less (int *pres, BID_UINT32 * px,
                                       BID_UINT32 *
                                       py _EXC_FLAGS_PARAM
                                       _EXC_MASKS_PARAM
                                       _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_quiet_ordered (int *pres, BID_UINT32 * px,
                                      BID_UINT32 *
                                      py _EXC_FLAGS_PARAM
                                      _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_quiet_unordered (int *pres, BID_UINT32 * px,
                                        BID_UINT32 *
                                        py _EXC_FLAGS_PARAM
                                        _EXC_MASKS_PARAM
                                        _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_signaling_greater (int *pres, BID_UINT32 * px,
                                          BID_UINT32 *
                                          py _EXC_FLAGS_PARAM
                                          _EXC_MASKS_PARAM
                                          _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_signaling_greater_equal (int *pres, BID_UINT32 * px,
                                                BID_UINT32 *
                                                py _EXC_FLAGS_PARAM
                                                _EXC_MASKS_PARAM
                                                _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_signaling_greater_unordered (int *pres,
                                                    BID_UINT32 * px,
                                                    BID_UINT32 *
                                                    py _EXC_FLAGS_PARAM
                                                    _EXC_MASKS_PARAM
                                                    _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_signaling_less (int *pres, BID_UINT32 * px,
                                       BID_UINT32 *
                                       py _EXC_FLAGS_PARAM
                                       _EXC_MASKS_PARAM
                                       _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_signaling_less_equal (int *pres, BID_UINT32 * px,
                                             BID_UINT32 *
                                             py _EXC_FLAGS_PARAM
                                             _EXC_MASKS_PARAM
                                             _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_signaling_less_unordered (int *pres, BID_UINT32 * px,
                                                 BID_UINT32 *
                                                 py _EXC_FLAGS_PARAM
                                                 _EXC_MASKS_PARAM
                                                 _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_signaling_not_greater (int *pres, BID_UINT32 * px,
                                              BID_UINT32 *
                                              py _EXC_FLAGS_PARAM
                                              _EXC_MASKS_PARAM
                                              _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_signaling_not_less (int *pres, BID_UINT32 * px,
                                           BID_UINT32 *
                                           py _EXC_FLAGS_PARAM
                                           _EXC_MASKS_PARAM
                                           _EXC_INFO_PARAM);

     BID_EXTERN_C void bid32_frexp (BID_UINT32 *pres, BID_UINT32 *px, int *exp);
     BID_EXTERN_C void bid64_frexp (BID_UINT64 *pres, BID_UINT64 *px, int *exp);
     BID_EXTERN_C void bid128_frexp (BID_UINT128 *pres, BID_UINT128 *px, int *exp);
     BID_EXTERN_C void bid32_logb (BID_UINT32 *pres, BID_UINT32 *px
                     _EXC_FLAGS_PARAM _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_logb (BID_UINT64 *pres, BID_UINT64 *px
                     _EXC_FLAGS_PARAM _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_logb (BID_UINT128 *pres, BID_UINT128 *px
                     _EXC_FLAGS_PARAM _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_scalbln (BID_UINT32 *pres, BID_UINT32 *px, long int *pn
        _RND_MODE_PARAM _EXC_FLAGS_PARAM _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_scalbln (BID_UINT64 *pres, BID_UINT64 *px, long int *pn
        _RND_MODE_PARAM _EXC_FLAGS_PARAM _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_scalbln (BID_UINT128 *pres, BID_UINT128 *px, long int *pn
        _RND_MODE_PARAM _EXC_FLAGS_PARAM _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_nearbyint (BID_UINT32 *pres, BID_UINT32 *px
        _RND_MODE_PARAM _EXC_FLAGS_PARAM _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_nearbyint (BID_UINT64 *pres, BID_UINT64 *px
        _RND_MODE_PARAM _EXC_FLAGS_PARAM _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_nearbyint (BID_UINT128 *pres, BID_UINT128 *px
        _RND_MODE_PARAM _EXC_FLAGS_PARAM _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_lrint (long int *pres, BID_UINT32 *px
        _RND_MODE_PARAM _EXC_FLAGS_PARAM _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_lrint (long int *pres, BID_UINT64 *px
        _RND_MODE_PARAM _EXC_FLAGS_PARAM _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_lrint (long int *pres, BID_UINT128 *px
        _RND_MODE_PARAM _EXC_FLAGS_PARAM _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_llrint (long long int *pres, BID_UINT32 *px
        _RND_MODE_PARAM _EXC_FLAGS_PARAM _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_llrint (long long int *pres, BID_UINT64 *px
        _RND_MODE_PARAM _EXC_FLAGS_PARAM _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_llrint (long long int *pres, BID_UINT128 *px
        _RND_MODE_PARAM _EXC_FLAGS_PARAM _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_lround (long int *pres, BID_UINT32 *px
        _EXC_FLAGS_PARAM _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_lround (long int *pres, BID_UINT64 *px
        _EXC_FLAGS_PARAM _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_lround (long int *pres, BID_UINT128 *px
        _EXC_FLAGS_PARAM _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_llround (long long int *pres, BID_UINT32 *px
        _EXC_FLAGS_PARAM _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_llround (long long int *pres, BID_UINT64 *px
        _EXC_FLAGS_PARAM _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_llround (long long int *pres, BID_UINT128 *px
        _EXC_FLAGS_PARAM _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_nan (BID_UINT32 *pres, const char *tagp);
     BID_EXTERN_C void bid64_nan (BID_UINT64 *pres, const char *tagp);
     BID_EXTERN_C void bid128_nan (BID_UINT128 *pres, const char *tagp);
     BID_EXTERN_C void bid32_nexttoward (BID_UINT32 *pres, BID_UINT32 *px, BID_UINT128 *py
         _EXC_FLAGS_PARAM _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_nexttoward (BID_UINT64 *pres, BID_UINT64 *px, BID_UINT128 *py
         _EXC_FLAGS_PARAM _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_nexttoward (BID_UINT128 *pres, BID_UINT128 *px, BID_UINT128 *py
         _EXC_FLAGS_PARAM _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_fdim (BID_UINT32 *pres, BID_UINT32 *px, BID_UINT32 *py
         _RND_MODE_PARAM _EXC_FLAGS_PARAM _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_fdim (BID_UINT64 *pres, BID_UINT64 *px, BID_UINT64 *py
         _RND_MODE_PARAM _EXC_FLAGS_PARAM _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_fdim (BID_UINT128 *pres, BID_UINT128 *px, BID_UINT128 *py
         _RND_MODE_PARAM _EXC_FLAGS_PARAM _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid32_quantexp (int *pres, BID_UINT32 *px
         _EXC_FLAGS_PARAM _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_quantexp (int *pres, BID_UINT64 *px
         _EXC_FLAGS_PARAM _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_quantexp (int *pres, BID_UINT128 *px
         _EXC_FLAGS_PARAM _EXC_MASKS_PARAM _EXC_INFO_PARAM);

     BID_EXTERN_C void bid32_inf (BID_UINT32 *pres);
     BID_EXTERN_C void bid64_inf (BID_UINT64 *pres);
     BID_EXTERN_C void bid128_inf (BID_UINT128 *pres);

#else

     BID_EXTERN_C BID_UINT32 bid_to_dpd32 (BID_UINT32 px);
     BID_EXTERN_C BID_UINT64 bid_to_dpd64 (BID_UINT64 px);
     BID_EXTERN_C BID_UINT128 bid_to_dpd128 (BID_UINT128 px);
     BID_EXTERN_C BID_UINT32 bid_dpd_to_bid32 (BID_UINT32 px);
     BID_EXTERN_C BID_UINT64 bid_dpd_to_bid64 (BID_UINT64 px);
     BID_EXTERN_C BID_UINT128 bid_dpd_to_bid128 (BID_UINT128 px);

     BID_EXTERN_C BID_UINT128 bid128dd_add (BID_UINT64 x, BID_UINT64 y
                                  _RND_MODE_PARAM _EXC_FLAGS_PARAM
                                  _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT128 bid128dq_add (BID_UINT64 x, BID_UINT128 y
                                  _RND_MODE_PARAM _EXC_FLAGS_PARAM
                                  _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT128 bid128qd_add (BID_UINT128 x, BID_UINT64 y
                                  _RND_MODE_PARAM _EXC_FLAGS_PARAM
                                  _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT128 bid128_add (BID_UINT128 x, BID_UINT128 y
                                _RND_MODE_PARAM _EXC_FLAGS_PARAM
                                _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT128 bid128dd_sub (BID_UINT64 x, BID_UINT64 y
                                  _RND_MODE_PARAM _EXC_FLAGS_PARAM
                                  _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT128 bid128dq_sub (BID_UINT64 x, BID_UINT128 y
                                  _RND_MODE_PARAM _EXC_FLAGS_PARAM
                                  _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT128 bid128qd_sub (BID_UINT128 x, BID_UINT64 y
                                  _RND_MODE_PARAM _EXC_FLAGS_PARAM
                                  _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT128 bid128_sub (BID_UINT128 x,
                                BID_UINT128 y _RND_MODE_PARAM
                                _EXC_FLAGS_PARAM _EXC_MASKS_PARAM
                                _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT128 bid128dd_mul (BID_UINT64 x, BID_UINT64 y
                                  _RND_MODE_PARAM _EXC_FLAGS_PARAM
                                  _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT128 bid128dq_mul (BID_UINT64 x, BID_UINT128 y
                                  _RND_MODE_PARAM _EXC_FLAGS_PARAM
                                  _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT128 bid128qd_mul (BID_UINT128 x, BID_UINT64 y
                                  _RND_MODE_PARAM _EXC_FLAGS_PARAM
                                  _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT128 bid128_mul (BID_UINT128 x, BID_UINT128 y
                                _RND_MODE_PARAM _EXC_FLAGS_PARAM
                                _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT128 bid128_div (BID_UINT128 x,
                                BID_UINT128 y _RND_MODE_PARAM
                                _EXC_FLAGS_PARAM _EXC_MASKS_PARAM
                                _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT128 bid128dd_div (BID_UINT64 x, BID_UINT64 y
                                  _RND_MODE_PARAM _EXC_FLAGS_PARAM
                                  _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT128 bid128dq_div (BID_UINT64 x, BID_UINT128 y
                                  _RND_MODE_PARAM _EXC_FLAGS_PARAM
                                  _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT128 bid128qd_div (BID_UINT128 x, BID_UINT64 y
                                  _RND_MODE_PARAM _EXC_FLAGS_PARAM
                                  _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT128 bid128_fma (BID_UINT128 x, BID_UINT128 y, BID_UINT128 z
                                _RND_MODE_PARAM _EXC_FLAGS_PARAM
                                _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT128 bid128ddd_fma (BID_UINT64 x, BID_UINT64 y, BID_UINT64 z
                                   _RND_MODE_PARAM _EXC_FLAGS_PARAM
                                   _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT128 bid128ddq_fma (BID_UINT64 x, BID_UINT64 y, BID_UINT128 z
                                   _RND_MODE_PARAM _EXC_FLAGS_PARAM
                                   _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT128 bid128dqd_fma (BID_UINT64 x, BID_UINT128 y, BID_UINT64 z
                                   _RND_MODE_PARAM _EXC_FLAGS_PARAM
                                   _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT128 bid128dqq_fma (BID_UINT64 x, BID_UINT128 y,
                                   BID_UINT128 z
                                   _RND_MODE_PARAM _EXC_FLAGS_PARAM
                                   _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT128 bid128qdd_fma (BID_UINT128 x, BID_UINT64 y, BID_UINT64 z
                                   _RND_MODE_PARAM _EXC_FLAGS_PARAM
                                   _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT128 bid128qdq_fma (BID_UINT128 x, BID_UINT64 y,
                                   BID_UINT128 z
                                   _RND_MODE_PARAM _EXC_FLAGS_PARAM
                                   _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT128 bid128qqd_fma (BID_UINT128 x, BID_UINT128 y,
                                   BID_UINT64 z
                                   _RND_MODE_PARAM _EXC_FLAGS_PARAM
                                   _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     // Note: bid128qqq_fma is represented by bid128_fma
     // Note: bid64ddd_fma is represented by bid64_fma
     BID_EXTERN_C BID_UINT64 bid64ddq_fma (BID_UINT64 x, BID_UINT64 y,
                                 BID_UINT128 z
                                 _RND_MODE_PARAM _EXC_FLAGS_PARAM
                                 _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT64 bid64dqd_fma (BID_UINT64 x, BID_UINT128 y,
                                 BID_UINT64 z
                                 _RND_MODE_PARAM _EXC_FLAGS_PARAM
                                 _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT64 bid64dqq_fma (BID_UINT64 x, BID_UINT128 y,
                                 BID_UINT128 z
                                 _RND_MODE_PARAM _EXC_FLAGS_PARAM
                                 _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT64 bid64qdd_fma (BID_UINT128 x, BID_UINT64 y,
                                 BID_UINT64 z
                                 _RND_MODE_PARAM _EXC_FLAGS_PARAM
                                 _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT64 bid64qdq_fma (BID_UINT128 x, BID_UINT64 y,
                                 BID_UINT128 z
                                 _RND_MODE_PARAM _EXC_FLAGS_PARAM
                                 _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT64 bid64qqd_fma (BID_UINT128 x, BID_UINT128 y,
                                 BID_UINT64 z
                                 _RND_MODE_PARAM _EXC_FLAGS_PARAM
                                 _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT64 bid64qqq_fma (BID_UINT128 x, BID_UINT128 y,
                                 BID_UINT128 z
                                 _RND_MODE_PARAM _EXC_FLAGS_PARAM
                                 _EXC_MASKS_PARAM _EXC_INFO_PARAM);

     BID_EXTERN_C BID_UINT128 bid128_sqrt (BID_UINT128 x _RND_MODE_PARAM
                                 _EXC_FLAGS_PARAM _EXC_MASKS_PARAM
                                 _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT128 bid128d_sqrt (BID_UINT64 x
                                  _RND_MODE_PARAM _EXC_FLAGS_PARAM
                                  _EXC_MASKS_PARAM _EXC_INFO_PARAM);

     BID_EXTERN_C BID_UINT128 bid128_cbrt (BID_UINT128 x _RND_MODE_PARAM
                                 _EXC_FLAGS_PARAM _EXC_MASKS_PARAM
                                 _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT32 bid32_exp (BID_UINT32 x
                              _RND_MODE_PARAM _EXC_FLAGS_PARAM
                              _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT32 bid32_log (BID_UINT32 x
                              _RND_MODE_PARAM _EXC_FLAGS_PARAM
                              _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT32 bid32_pow (BID_UINT32 x, BID_UINT32 y
                              _RND_MODE_PARAM _EXC_FLAGS_PARAM
                              _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT32 bid32_atan2 (BID_UINT32 x, BID_UINT32 y
                              _RND_MODE_PARAM _EXC_FLAGS_PARAM
                              _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT32 bid32_fmod (BID_UINT32 x, BID_UINT32 y
                               _EXC_FLAGS_PARAM
                              _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT32 bid32_modf (BID_UINT32 x, BID_UINT32 * y
                              _EXC_FLAGS_PARAM
                              _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT32 bid32_hypot (BID_UINT32 x, BID_UINT32 y
                              _RND_MODE_PARAM _EXC_FLAGS_PARAM
                              _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT32 bid32_sin (BID_UINT32 x
                              _RND_MODE_PARAM _EXC_FLAGS_PARAM
                              _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT32 bid32_cos (BID_UINT32 x
                              _RND_MODE_PARAM _EXC_FLAGS_PARAM
                              _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT32 bid32_tan (BID_UINT32 x
                              _RND_MODE_PARAM _EXC_FLAGS_PARAM
                              _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT32 bid32_asin (BID_UINT32 x
                              _RND_MODE_PARAM _EXC_FLAGS_PARAM
                              _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT32 bid32_acos (BID_UINT32 x
                              _RND_MODE_PARAM _EXC_FLAGS_PARAM
                              _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT32 bid32_atan (BID_UINT32 x
                              _RND_MODE_PARAM _EXC_FLAGS_PARAM
                              _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT32 bid32_sinh (BID_UINT32 x
                              _RND_MODE_PARAM _EXC_FLAGS_PARAM
                              _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT32 bid32_cosh (BID_UINT32 x
                              _RND_MODE_PARAM _EXC_FLAGS_PARAM
                              _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT32 bid32_tanh (BID_UINT32 x
                              _RND_MODE_PARAM _EXC_FLAGS_PARAM
                              _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT32 bid32_asinh (BID_UINT32 x
                              _RND_MODE_PARAM _EXC_FLAGS_PARAM
                              _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT32 bid32_acosh (BID_UINT32 x
                              _RND_MODE_PARAM _EXC_FLAGS_PARAM
                              _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT32 bid32_atanh (BID_UINT32 x
                              _RND_MODE_PARAM _EXC_FLAGS_PARAM
                              _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT32 bid32_log1p (BID_UINT32 x
                              _RND_MODE_PARAM _EXC_FLAGS_PARAM
                              _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT32 bid32_expm1 (BID_UINT32 x
                              _RND_MODE_PARAM _EXC_FLAGS_PARAM
                              _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT32 bid32_log10 (BID_UINT32 x
                              _RND_MODE_PARAM _EXC_FLAGS_PARAM
                              _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT32 bid32_log2 (BID_UINT32 x
                              _RND_MODE_PARAM _EXC_FLAGS_PARAM
                              _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT32 bid32_exp2 (BID_UINT32 x
                              _RND_MODE_PARAM _EXC_FLAGS_PARAM
                              _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT32 bid32_exp10 (BID_UINT32 x
                              _RND_MODE_PARAM _EXC_FLAGS_PARAM
                              _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT32 bid32_erf (BID_UINT32 x
                              _RND_MODE_PARAM _EXC_FLAGS_PARAM
                              _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT32 bid32_erfc (BID_UINT32 x
                              _RND_MODE_PARAM _EXC_FLAGS_PARAM
                              _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT32 bid32_tgamma (BID_UINT32 x
                              _RND_MODE_PARAM _EXC_FLAGS_PARAM
                              _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT32 bid32_lgamma (BID_UINT32 x
                              _RND_MODE_PARAM _EXC_FLAGS_PARAM
                              _EXC_MASKS_PARAM _EXC_INFO_PARAM);

     BID_EXTERN_C BID_UINT64 bid64_exp (BID_UINT64 x
                              _RND_MODE_PARAM _EXC_FLAGS_PARAM
                              _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT64 bid64_log (BID_UINT64 x
                              _RND_MODE_PARAM _EXC_FLAGS_PARAM
                              _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT64 bid64_pow (BID_UINT64 x, BID_UINT64 y
                              _RND_MODE_PARAM _EXC_FLAGS_PARAM
                              _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT64 bid64_atan2 (BID_UINT64 x, BID_UINT64 y
                              _RND_MODE_PARAM _EXC_FLAGS_PARAM
                              _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT64 bid64_fmod (BID_UINT64 x, BID_UINT64 y
                               _EXC_FLAGS_PARAM
                              _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT64 bid64_modf (BID_UINT64 x, BID_UINT64 * y
                              _EXC_FLAGS_PARAM
                              _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT64 bid64_hypot (BID_UINT64 x, BID_UINT64 y
                              _RND_MODE_PARAM _EXC_FLAGS_PARAM
                              _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT64 bid64_sin (BID_UINT64 x
                              _RND_MODE_PARAM _EXC_FLAGS_PARAM
                              _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT64 bid64_cos (BID_UINT64 x
                              _RND_MODE_PARAM _EXC_FLAGS_PARAM
                              _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT64 bid64_tan (BID_UINT64 x
                              _RND_MODE_PARAM _EXC_FLAGS_PARAM
                              _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT64 bid64_asin (BID_UINT64 x
                              _RND_MODE_PARAM _EXC_FLAGS_PARAM
                              _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT64 bid64_acos (BID_UINT64 x
                              _RND_MODE_PARAM _EXC_FLAGS_PARAM
                              _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT64 bid64_atan (BID_UINT64 x
                              _RND_MODE_PARAM _EXC_FLAGS_PARAM
                              _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT64 bid64_sinh (BID_UINT64 x
                              _RND_MODE_PARAM _EXC_FLAGS_PARAM
                              _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT64 bid64_cosh (BID_UINT64 x
                              _RND_MODE_PARAM _EXC_FLAGS_PARAM
                              _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT64 bid64_tanh (BID_UINT64 x
                              _RND_MODE_PARAM _EXC_FLAGS_PARAM
                              _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT64 bid64_asinh (BID_UINT64 x
                              _RND_MODE_PARAM _EXC_FLAGS_PARAM
                              _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT64 bid64_acosh (BID_UINT64 x
                              _RND_MODE_PARAM _EXC_FLAGS_PARAM
                              _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT64 bid64_atanh (BID_UINT64 x
                              _RND_MODE_PARAM _EXC_FLAGS_PARAM
                              _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT64 bid64_log1p (BID_UINT64 x
                              _RND_MODE_PARAM _EXC_FLAGS_PARAM
                              _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT64 bid64_expm1 (BID_UINT64 x
                              _RND_MODE_PARAM _EXC_FLAGS_PARAM
                              _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT64 bid64_log10 (BID_UINT64 x
                              _RND_MODE_PARAM _EXC_FLAGS_PARAM
                              _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT64 bid64_log2 (BID_UINT64 x
                              _RND_MODE_PARAM _EXC_FLAGS_PARAM
                              _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT64 bid64_exp2 (BID_UINT64 x
                              _RND_MODE_PARAM _EXC_FLAGS_PARAM
                              _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT64 bid64_exp10 (BID_UINT64 x
                              _RND_MODE_PARAM _EXC_FLAGS_PARAM
                              _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT64 bid64_erf (BID_UINT64 x
                              _RND_MODE_PARAM _EXC_FLAGS_PARAM
                              _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT64 bid64_erfc (BID_UINT64 x
                              _RND_MODE_PARAM _EXC_FLAGS_PARAM
                              _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT64 bid64_tgamma (BID_UINT64 x
                              _RND_MODE_PARAM _EXC_FLAGS_PARAM
                              _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT64 bid64_lgamma (BID_UINT64 x
                              _RND_MODE_PARAM _EXC_FLAGS_PARAM
                              _EXC_MASKS_PARAM _EXC_INFO_PARAM);

     BID_EXTERN_C BID_UINT128 bid128_exp (BID_UINT128 x
                              _RND_MODE_PARAM _EXC_FLAGS_PARAM
                              _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT128 bid128_log (BID_UINT128 x
                              _RND_MODE_PARAM _EXC_FLAGS_PARAM
                              _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT128 bid128_pow (BID_UINT128 x, BID_UINT128 y
                              _RND_MODE_PARAM _EXC_FLAGS_PARAM
                              _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT128 bid128_atan2 (BID_UINT128 x, BID_UINT128 y
                              _RND_MODE_PARAM _EXC_FLAGS_PARAM
                              _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT128 bid128_fmod (BID_UINT128 x, BID_UINT128 y
                               _EXC_FLAGS_PARAM
                              _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT128 bid128_modf (BID_UINT128 x, BID_UINT128 * y
                              _EXC_FLAGS_PARAM
                              _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT128 bid128_hypot (BID_UINT128 x, BID_UINT128 y
                              _RND_MODE_PARAM _EXC_FLAGS_PARAM
                              _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT128 bid128_sin (BID_UINT128 x
                              _RND_MODE_PARAM _EXC_FLAGS_PARAM
                              _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT128 bid128_cos (BID_UINT128 x
                              _RND_MODE_PARAM _EXC_FLAGS_PARAM
                              _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT128 bid128_tan (BID_UINT128 x
                              _RND_MODE_PARAM _EXC_FLAGS_PARAM
                              _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT128 bid128_asin (BID_UINT128 x
                              _RND_MODE_PARAM _EXC_FLAGS_PARAM
                              _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT128 bid128_acos (BID_UINT128 x
                              _RND_MODE_PARAM _EXC_FLAGS_PARAM
                              _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT128 bid128_atan (BID_UINT128 x
                              _RND_MODE_PARAM _EXC_FLAGS_PARAM
                              _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT128 bid128_sinh (BID_UINT128 x
                              _RND_MODE_PARAM _EXC_FLAGS_PARAM
                              _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT128 bid128_cosh (BID_UINT128 x
                              _RND_MODE_PARAM _EXC_FLAGS_PARAM
                              _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT128 bid128_tanh (BID_UINT128 x
                              _RND_MODE_PARAM _EXC_FLAGS_PARAM
                              _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT128 bid128_asinh (BID_UINT128 x
                              _RND_MODE_PARAM _EXC_FLAGS_PARAM
                              _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT128 bid128_acosh (BID_UINT128 x
                              _RND_MODE_PARAM _EXC_FLAGS_PARAM
                              _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT128 bid128_atanh (BID_UINT128 x
                              _RND_MODE_PARAM _EXC_FLAGS_PARAM
                              _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT128 bid128_log1p (BID_UINT128 x
                              _RND_MODE_PARAM _EXC_FLAGS_PARAM
                              _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT128 bid128_expm1 (BID_UINT128 x
                              _RND_MODE_PARAM _EXC_FLAGS_PARAM
                              _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT128 bid128_log10 (BID_UINT128 x
                              _RND_MODE_PARAM _EXC_FLAGS_PARAM
                              _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT128 bid128_log2 (BID_UINT128 x
                              _RND_MODE_PARAM _EXC_FLAGS_PARAM
                              _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT128 bid128_exp2 (BID_UINT128 x
                              _RND_MODE_PARAM _EXC_FLAGS_PARAM
                              _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT128 bid128_exp10 (BID_UINT128 x
                              _RND_MODE_PARAM _EXC_FLAGS_PARAM
                              _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT128 bid128_erf (BID_UINT128 x
                              _RND_MODE_PARAM _EXC_FLAGS_PARAM
                              _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT128 bid128_erfc (BID_UINT128 x
                              _RND_MODE_PARAM _EXC_FLAGS_PARAM
                              _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT128 bid128_tgamma (BID_UINT128 x
                              _RND_MODE_PARAM _EXC_FLAGS_PARAM
                              _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT128 bid128_lgamma (BID_UINT128 x
                              _RND_MODE_PARAM _EXC_FLAGS_PARAM
                              _EXC_MASKS_PARAM _EXC_INFO_PARAM);

     BID_EXTERN_C BID_UINT64 bid64_add (BID_UINT64 x, BID_UINT64 y
                              _RND_MODE_PARAM _EXC_FLAGS_PARAM
                              _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT64 bid64dq_add (BID_UINT64 x, BID_UINT128 y
                                _RND_MODE_PARAM _EXC_FLAGS_PARAM
                                _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT64 bid64qd_add (BID_UINT128 x, BID_UINT64 y
                                _RND_MODE_PARAM _EXC_FLAGS_PARAM
                                _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT64 bid64qq_add (BID_UINT128 x, BID_UINT128 y
                                _RND_MODE_PARAM _EXC_FLAGS_PARAM
                                _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT64 bid64_sub (BID_UINT64 x,
                              BID_UINT64 y _RND_MODE_PARAM
                              _EXC_FLAGS_PARAM _EXC_MASKS_PARAM
                              _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT64 bid64dq_sub (BID_UINT64 x, BID_UINT128 y
                                _RND_MODE_PARAM _EXC_FLAGS_PARAM
                                _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT64 bid64qd_sub (BID_UINT128 x, BID_UINT64 y
                                _RND_MODE_PARAM _EXC_FLAGS_PARAM
                                _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT64 bid64qq_sub (BID_UINT128 x, BID_UINT128 y
                                _RND_MODE_PARAM _EXC_FLAGS_PARAM
                                _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT64 bid64_mul (BID_UINT64 x, BID_UINT64 y
                              _RND_MODE_PARAM _EXC_FLAGS_PARAM
                              _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT64 bid64dq_mul (BID_UINT64 x, BID_UINT128 y
                                _RND_MODE_PARAM _EXC_FLAGS_PARAM
                                _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT64 bid64qd_mul (BID_UINT128 x, BID_UINT64 y
                                _RND_MODE_PARAM _EXC_FLAGS_PARAM
                                _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT64 bid64qq_mul (BID_UINT128 x, BID_UINT128 y
                                _RND_MODE_PARAM _EXC_FLAGS_PARAM
                                _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT64 bid64_div (BID_UINT64 x,
                              BID_UINT64 y _RND_MODE_PARAM
                              _EXC_FLAGS_PARAM _EXC_MASKS_PARAM
                              _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT64 bid64dq_div (BID_UINT64 x, BID_UINT128 y
                                _RND_MODE_PARAM _EXC_FLAGS_PARAM
                                _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT64 bid64qd_div (BID_UINT128 x, BID_UINT64 y
                                _RND_MODE_PARAM _EXC_FLAGS_PARAM
                                _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT64 bid64qq_div (BID_UINT128 x, BID_UINT128 y
                                _RND_MODE_PARAM _EXC_FLAGS_PARAM
                                _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT64 bid64_fma (BID_UINT64 x, BID_UINT64 y,
                              BID_UINT64 z _RND_MODE_PARAM
                              _EXC_FLAGS_PARAM _EXC_MASKS_PARAM
                              _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT64 bid64_sqrt (BID_UINT64 x _RND_MODE_PARAM
                               _EXC_FLAGS_PARAM _EXC_MASKS_PARAM
                               _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT64 bid64q_sqrt (BID_UINT128 x
                                _RND_MODE_PARAM _EXC_FLAGS_PARAM
                                _EXC_MASKS_PARAM _EXC_INFO_PARAM);

     BID_EXTERN_C BID_UINT64 bid64_cbrt (BID_UINT64 x _RND_MODE_PARAM
                               _EXC_FLAGS_PARAM _EXC_MASKS_PARAM
                               _EXC_INFO_PARAM);
     BID_EXTERN_C char bid128_to_int8_rnint (BID_UINT128 x
                                       _EXC_FLAGS_PARAM
                                       _EXC_MASKS_PARAM
                                       _EXC_INFO_PARAM);
     BID_EXTERN_C char bid128_to_int8_xrnint (BID_UINT128 x
                                        _EXC_FLAGS_PARAM
                                        _EXC_MASKS_PARAM
                                        _EXC_INFO_PARAM);
     BID_EXTERN_C char bid128_to_int8_rninta (BID_UINT128 x
                                        _EXC_FLAGS_PARAM
                                        _EXC_MASKS_PARAM
                                        _EXC_INFO_PARAM);
     BID_EXTERN_C char bid128_to_int8_xrninta (BID_UINT128 x
                                         _EXC_FLAGS_PARAM
                                         _EXC_MASKS_PARAM
                                         _EXC_INFO_PARAM);
     BID_EXTERN_C char bid128_to_int8_int (BID_UINT128 x _EXC_FLAGS_PARAM
                                     _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C char bid128_to_int8_xint (BID_UINT128 x _EXC_FLAGS_PARAM
                                      _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C char bid128_to_int8_floor (BID_UINT128 x
                                       _EXC_FLAGS_PARAM
                                       _EXC_MASKS_PARAM
                                       _EXC_INFO_PARAM);
     BID_EXTERN_C char bid128_to_int8_xfloor (BID_UINT128 x
                                        _EXC_FLAGS_PARAM
                                        _EXC_MASKS_PARAM
                                        _EXC_INFO_PARAM);
     BID_EXTERN_C char bid128_to_int8_ceil (BID_UINT128 x _EXC_FLAGS_PARAM
                                      _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C char bid128_to_int8_xceil (BID_UINT128 x
                                       _EXC_FLAGS_PARAM
                                       _EXC_MASKS_PARAM
                                       _EXC_INFO_PARAM);
     BID_EXTERN_C short bid128_to_int16_rnint (BID_UINT128 x
                                         _EXC_FLAGS_PARAM
                                         _EXC_MASKS_PARAM
                                         _EXC_INFO_PARAM);
     BID_EXTERN_C short bid128_to_int16_xrnint (BID_UINT128 x
                                          _EXC_FLAGS_PARAM
                                          _EXC_MASKS_PARAM
                                          _EXC_INFO_PARAM);
     BID_EXTERN_C short bid128_to_int16_rninta (BID_UINT128 x
                                          _EXC_FLAGS_PARAM
                                          _EXC_MASKS_PARAM
                                          _EXC_INFO_PARAM);
     BID_EXTERN_C short bid128_to_int16_xrninta (BID_UINT128 x
                                           _EXC_FLAGS_PARAM
                                           _EXC_MASKS_PARAM
                                           _EXC_INFO_PARAM);
     BID_EXTERN_C short bid128_to_int16_int (BID_UINT128 x _EXC_FLAGS_PARAM
                                       _EXC_MASKS_PARAM
                                       _EXC_INFO_PARAM);
     BID_EXTERN_C short bid128_to_int16_xint (BID_UINT128 x _EXC_FLAGS_PARAM
                                        _EXC_MASKS_PARAM
                                        _EXC_INFO_PARAM);
     BID_EXTERN_C short bid128_to_int16_floor (BID_UINT128 x
                                         _EXC_FLAGS_PARAM
                                         _EXC_MASKS_PARAM
                                         _EXC_INFO_PARAM);
     BID_EXTERN_C short bid128_to_int16_xfloor (BID_UINT128 x
                                          _EXC_FLAGS_PARAM
                                          _EXC_MASKS_PARAM
                                          _EXC_INFO_PARAM);
     BID_EXTERN_C short bid128_to_int16_ceil (BID_UINT128 x _EXC_FLAGS_PARAM
                                        _EXC_MASKS_PARAM
                                        _EXC_INFO_PARAM);
     BID_EXTERN_C short bid128_to_int16_xceil (BID_UINT128 x
                                         _EXC_FLAGS_PARAM
                                         _EXC_MASKS_PARAM
                                         _EXC_INFO_PARAM);
     BID_EXTERN_C unsigned char bid128_to_uint8_rnint (BID_UINT128 x
                                                 _EXC_FLAGS_PARAM
                                                 _EXC_MASKS_PARAM
                                                 _EXC_INFO_PARAM);
     BID_EXTERN_C unsigned char bid128_to_uint8_xrnint (BID_UINT128 x
                                                  _EXC_FLAGS_PARAM
                                                  _EXC_MASKS_PARAM
                                                  _EXC_INFO_PARAM);
     BID_EXTERN_C unsigned char bid128_to_uint8_rninta (BID_UINT128 x
                                                  _EXC_FLAGS_PARAM
                                                  _EXC_MASKS_PARAM
                                                  _EXC_INFO_PARAM);
     BID_EXTERN_C unsigned char bid128_to_uint8_xrninta (BID_UINT128 x
                                                   _EXC_FLAGS_PARAM
                                                   _EXC_MASKS_PARAM
                                                   _EXC_INFO_PARAM);
     BID_EXTERN_C unsigned char bid128_to_uint8_int (BID_UINT128 x
                                               _EXC_FLAGS_PARAM
                                               _EXC_MASKS_PARAM
                                               _EXC_INFO_PARAM);
     BID_EXTERN_C unsigned char bid128_to_uint8_xint (BID_UINT128 x
                                                _EXC_FLAGS_PARAM
                                                _EXC_MASKS_PARAM
                                                _EXC_INFO_PARAM);
     BID_EXTERN_C unsigned char bid128_to_uint8_floor (BID_UINT128 x
                                                 _EXC_FLAGS_PARAM
                                                 _EXC_MASKS_PARAM
                                                 _EXC_INFO_PARAM);
     BID_EXTERN_C unsigned char bid128_to_uint8_xfloor (BID_UINT128 x
                                                  _EXC_FLAGS_PARAM
                                                  _EXC_MASKS_PARAM
                                                  _EXC_INFO_PARAM);
     BID_EXTERN_C unsigned char bid128_to_uint8_ceil (BID_UINT128 x
                                                _EXC_FLAGS_PARAM
                                                _EXC_MASKS_PARAM
                                                _EXC_INFO_PARAM);
     BID_EXTERN_C unsigned char bid128_to_uint8_xceil (BID_UINT128 x
                                                 _EXC_FLAGS_PARAM
                                                 _EXC_MASKS_PARAM
                                                 _EXC_INFO_PARAM);
     BID_EXTERN_C unsigned short bid128_to_uint16_rnint (BID_UINT128 x
                                                   _EXC_FLAGS_PARAM
                                                   _EXC_MASKS_PARAM
                                                   _EXC_INFO_PARAM);
     BID_EXTERN_C unsigned short bid128_to_uint16_xrnint (BID_UINT128 x
                                                    _EXC_FLAGS_PARAM
                                                    _EXC_MASKS_PARAM
                                                    _EXC_INFO_PARAM);
     BID_EXTERN_C unsigned short bid128_to_uint16_rninta (BID_UINT128 x
                                                    _EXC_FLAGS_PARAM
                                                    _EXC_MASKS_PARAM
                                                    _EXC_INFO_PARAM);
     BID_EXTERN_C unsigned short bid128_to_uint16_xrninta (BID_UINT128 x
                                                     _EXC_FLAGS_PARAM
                                                     _EXC_MASKS_PARAM
                                                     _EXC_INFO_PARAM);
     BID_EXTERN_C unsigned short bid128_to_uint16_int (BID_UINT128 x
                                                 _EXC_FLAGS_PARAM
                                                 _EXC_MASKS_PARAM
                                                 _EXC_INFO_PARAM);
     BID_EXTERN_C unsigned short bid128_to_uint16_xint (BID_UINT128 x
                                                  _EXC_FLAGS_PARAM
                                                  _EXC_MASKS_PARAM
                                                  _EXC_INFO_PARAM);
     BID_EXTERN_C unsigned short bid128_to_uint16_floor (BID_UINT128 x
                                                   _EXC_FLAGS_PARAM
                                                   _EXC_MASKS_PARAM
                                                   _EXC_INFO_PARAM);
     BID_EXTERN_C unsigned short bid128_to_uint16_xfloor (BID_UINT128 x
                                                    _EXC_FLAGS_PARAM
                                                    _EXC_MASKS_PARAM
                                                    _EXC_INFO_PARAM);
     BID_EXTERN_C unsigned short bid128_to_uint16_ceil (BID_UINT128 x
                                                  _EXC_FLAGS_PARAM
                                                  _EXC_MASKS_PARAM
                                                  _EXC_INFO_PARAM);
     BID_EXTERN_C unsigned short bid128_to_uint16_xceil (BID_UINT128 x
                                                   _EXC_FLAGS_PARAM
                                                   _EXC_MASKS_PARAM
                                                   _EXC_INFO_PARAM);
     BID_EXTERN_C int bid128_to_int32_rnint (BID_UINT128 x _EXC_FLAGS_PARAM
                                       _EXC_MASKS_PARAM
                                       _EXC_INFO_PARAM);
     BID_EXTERN_C int bid128_to_int32_xrnint (BID_UINT128 x _EXC_FLAGS_PARAM
                                        _EXC_MASKS_PARAM
                                        _EXC_INFO_PARAM);
     BID_EXTERN_C int bid128_to_int32_rninta (BID_UINT128 x _EXC_FLAGS_PARAM
                                        _EXC_MASKS_PARAM
                                        _EXC_INFO_PARAM);
     BID_EXTERN_C int bid128_to_int32_xrninta (BID_UINT128 x _EXC_FLAGS_PARAM
                                         _EXC_MASKS_PARAM
                                         _EXC_INFO_PARAM);
     BID_EXTERN_C int bid128_to_int32_int (BID_UINT128 x _EXC_FLAGS_PARAM
                                     _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C int bid128_to_int32_xint (BID_UINT128 x _EXC_FLAGS_PARAM
                                      _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C int bid128_to_int32_floor (BID_UINT128 x _EXC_FLAGS_PARAM
                                       _EXC_MASKS_PARAM
                                       _EXC_INFO_PARAM);
     BID_EXTERN_C int bid128_to_int32_xfloor (BID_UINT128 x _EXC_FLAGS_PARAM
                                        _EXC_MASKS_PARAM
                                        _EXC_INFO_PARAM);
     BID_EXTERN_C int bid128_to_int32_ceil (BID_UINT128 x _EXC_FLAGS_PARAM
                                      _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C int bid128_to_int32_xceil (BID_UINT128 x _EXC_FLAGS_PARAM
                                       _EXC_MASKS_PARAM
                                       _EXC_INFO_PARAM);
     BID_EXTERN_C unsigned int bid128_to_uint32_rnint (BID_UINT128 x
                                                 _EXC_FLAGS_PARAM
                                                 _EXC_MASKS_PARAM
                                                 _EXC_INFO_PARAM);
     BID_EXTERN_C unsigned int bid128_to_uint32_xrnint (BID_UINT128 x
                                                  _EXC_FLAGS_PARAM
                                                  _EXC_MASKS_PARAM
                                                  _EXC_INFO_PARAM);
     BID_EXTERN_C unsigned int bid128_to_uint32_rninta (BID_UINT128 x
                                                  _EXC_FLAGS_PARAM
                                                  _EXC_MASKS_PARAM
                                                  _EXC_INFO_PARAM);
     BID_EXTERN_C unsigned int bid128_to_uint32_xrninta (BID_UINT128 x
                                                   _EXC_FLAGS_PARAM
                                                   _EXC_MASKS_PARAM
                                                   _EXC_INFO_PARAM);
     BID_EXTERN_C unsigned int bid128_to_uint32_int (BID_UINT128 x
                                               _EXC_FLAGS_PARAM
                                               _EXC_MASKS_PARAM
                                               _EXC_INFO_PARAM);
     BID_EXTERN_C unsigned int bid128_to_uint32_xint (BID_UINT128 x
                                                _EXC_FLAGS_PARAM
                                                _EXC_MASKS_PARAM
                                                _EXC_INFO_PARAM);
     BID_EXTERN_C unsigned int bid128_to_uint32_floor (BID_UINT128 x
                                                 _EXC_FLAGS_PARAM
                                                 _EXC_MASKS_PARAM
                                                 _EXC_INFO_PARAM);
     BID_EXTERN_C unsigned int bid128_to_uint32_xfloor (BID_UINT128 x
                                                  _EXC_FLAGS_PARAM
                                                  _EXC_MASKS_PARAM
                                                  _EXC_INFO_PARAM);
     BID_EXTERN_C unsigned int bid128_to_uint32_ceil (BID_UINT128 x
                                                _EXC_FLAGS_PARAM
                                                _EXC_MASKS_PARAM
                                                _EXC_INFO_PARAM);
     BID_EXTERN_C unsigned int bid128_to_uint32_xceil (BID_UINT128 x
                                                 _EXC_FLAGS_PARAM
                                                 _EXC_MASKS_PARAM
                                                 _EXC_INFO_PARAM);
     BID_EXTERN_C BID_SINT64 bid128_to_int64_rnint (BID_UINT128 x _EXC_FLAGS_PARAM
                                          _EXC_MASKS_PARAM
                                          _EXC_INFO_PARAM);
     BID_EXTERN_C BID_SINT64 bid128_to_int64_xrnint (BID_UINT128 x _EXC_FLAGS_PARAM
                                           _EXC_MASKS_PARAM
                                           _EXC_INFO_PARAM);
     BID_EXTERN_C BID_SINT64 bid128_to_int64_rninta (BID_UINT128 x _EXC_FLAGS_PARAM
                                           _EXC_MASKS_PARAM
                                           _EXC_INFO_PARAM);
     BID_EXTERN_C BID_SINT64 bid128_to_int64_xrninta (BID_UINT128 x _EXC_FLAGS_PARAM
                                            _EXC_MASKS_PARAM
                                            _EXC_INFO_PARAM);
     BID_EXTERN_C BID_SINT64 bid128_to_int64_int (BID_UINT128 x _EXC_FLAGS_PARAM
                                        _EXC_MASKS_PARAM
                                        _EXC_INFO_PARAM);
     BID_EXTERN_C BID_SINT64 bid128_to_int64_xint (BID_UINT128 x _EXC_FLAGS_PARAM
                                         _EXC_MASKS_PARAM
                                         _EXC_INFO_PARAM);
     BID_EXTERN_C BID_SINT64 bid128_to_int64_floor (BID_UINT128 x _EXC_FLAGS_PARAM
                                          _EXC_MASKS_PARAM
                                          _EXC_INFO_PARAM);
     BID_EXTERN_C BID_SINT64 bid128_to_int64_xfloor (BID_UINT128 x _EXC_FLAGS_PARAM
                                           _EXC_MASKS_PARAM
                                           _EXC_INFO_PARAM);
     BID_EXTERN_C BID_SINT64 bid128_to_int64_ceil (BID_UINT128 x _EXC_FLAGS_PARAM
                                         _EXC_MASKS_PARAM
                                         _EXC_INFO_PARAM);
     BID_EXTERN_C BID_SINT64 bid128_to_int64_xceil (BID_UINT128 x _EXC_FLAGS_PARAM
                                          _EXC_MASKS_PARAM
                                          _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT64 bid128_to_uint64_rnint (BID_UINT128 x _EXC_FLAGS_PARAM
                                           _EXC_MASKS_PARAM
                                           _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT64 bid128_to_uint64_xrnint (BID_UINT128 x _EXC_FLAGS_PARAM
                                            _EXC_MASKS_PARAM
                                            _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT64 bid128_to_uint64_rninta (BID_UINT128 x _EXC_FLAGS_PARAM
                                            _EXC_MASKS_PARAM
                                            _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT64 bid128_to_uint64_xrninta (BID_UINT128 x _EXC_FLAGS_PARAM
                                             _EXC_MASKS_PARAM
                                             _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT64 bid128_to_uint64_int (BID_UINT128 x _EXC_FLAGS_PARAM
                                         _EXC_MASKS_PARAM
                                         _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT64 bid128_to_uint64_xint (BID_UINT128 x _EXC_FLAGS_PARAM
                                          _EXC_MASKS_PARAM
                                          _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT64 bid128_to_uint64_floor (BID_UINT128 x _EXC_FLAGS_PARAM
                                           _EXC_MASKS_PARAM
                                           _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT64 bid128_to_uint64_xfloor (BID_UINT128 x _EXC_FLAGS_PARAM
                                            _EXC_MASKS_PARAM
                                            _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT64 bid128_to_uint64_ceil (BID_UINT128 x _EXC_FLAGS_PARAM
                                          _EXC_MASKS_PARAM
                                          _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT64 bid128_to_uint64_xceil (BID_UINT128 x _EXC_FLAGS_PARAM
                                           _EXC_MASKS_PARAM
                                           _EXC_INFO_PARAM);
     BID_EXTERN_C int bid64_to_int32_rnint (BID_UINT64 x _EXC_FLAGS_PARAM
                                      _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C int bid64_to_int32_xrnint (BID_UINT64 x _EXC_FLAGS_PARAM
                                       _EXC_MASKS_PARAM
                                       _EXC_INFO_PARAM);
     BID_EXTERN_C int bid64_to_int32_rninta (BID_UINT64 x _EXC_FLAGS_PARAM
                                       _EXC_MASKS_PARAM
                                       _EXC_INFO_PARAM);
     BID_EXTERN_C int bid64_to_int32_xrninta (BID_UINT64 x _EXC_FLAGS_PARAM
                                        _EXC_MASKS_PARAM
                                        _EXC_INFO_PARAM);
     BID_EXTERN_C int bid64_to_int32_int (BID_UINT64 x _EXC_FLAGS_PARAM
                                    _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C int bid64_to_int32_xint (BID_UINT64 x _EXC_FLAGS_PARAM
                                     _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C int bid64_to_int32_floor (BID_UINT64 x _EXC_FLAGS_PARAM
                                      _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C int bid64_to_int32_xfloor (BID_UINT64 x _EXC_FLAGS_PARAM
                                       _EXC_MASKS_PARAM
                                       _EXC_INFO_PARAM);
     BID_EXTERN_C int bid64_to_int32_ceil (BID_UINT64 x _EXC_FLAGS_PARAM
                                     _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C int bid64_to_int32_xceil (BID_UINT64 x _EXC_FLAGS_PARAM
                                      _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C char bid64_to_int8_rnint (BID_UINT64 x _EXC_FLAGS_PARAM
                                      _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C char bid64_to_int8_xrnint (BID_UINT64 x _EXC_FLAGS_PARAM
                                       _EXC_MASKS_PARAM
                                       _EXC_INFO_PARAM);
     BID_EXTERN_C char bid64_to_int8_rninta (BID_UINT64 x _EXC_FLAGS_PARAM
                                       _EXC_MASKS_PARAM
                                       _EXC_INFO_PARAM);
     BID_EXTERN_C char bid64_to_int8_xrninta (BID_UINT64 x _EXC_FLAGS_PARAM
                                        _EXC_MASKS_PARAM
                                        _EXC_INFO_PARAM);
     BID_EXTERN_C char bid64_to_int8_int (BID_UINT64 x _EXC_FLAGS_PARAM
                                    _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C char bid64_to_int8_xint (BID_UINT64 x _EXC_FLAGS_PARAM
                                     _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C char bid64_to_int8_floor (BID_UINT64 x _EXC_FLAGS_PARAM
                                      _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C char bid64_to_int8_xfloor (BID_UINT64 x _EXC_FLAGS_PARAM
                                       _EXC_MASKS_PARAM
                                       _EXC_INFO_PARAM);
     BID_EXTERN_C char bid64_to_int8_ceil (BID_UINT64 x _EXC_FLAGS_PARAM
                                     _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C char bid64_to_int8_xceil (BID_UINT64 x _EXC_FLAGS_PARAM
                                      _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C short bid64_to_int16_rnint (BID_UINT64 x _EXC_FLAGS_PARAM
                                        _EXC_MASKS_PARAM
                                        _EXC_INFO_PARAM);
     BID_EXTERN_C short bid64_to_int16_xrnint (BID_UINT64 x _EXC_FLAGS_PARAM
                                         _EXC_MASKS_PARAM
                                         _EXC_INFO_PARAM);
     BID_EXTERN_C short bid64_to_int16_rninta (BID_UINT64 x _EXC_FLAGS_PARAM
                                         _EXC_MASKS_PARAM
                                         _EXC_INFO_PARAM);
     BID_EXTERN_C short bid64_to_int16_xrninta (BID_UINT64 x _EXC_FLAGS_PARAM
                                          _EXC_MASKS_PARAM
                                          _EXC_INFO_PARAM);
     BID_EXTERN_C short bid64_to_int16_int (BID_UINT64 x _EXC_FLAGS_PARAM
                                      _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C short bid64_to_int16_xint (BID_UINT64 x _EXC_FLAGS_PARAM
                                       _EXC_MASKS_PARAM
                                       _EXC_INFO_PARAM);
     BID_EXTERN_C short bid64_to_int16_floor (BID_UINT64 x _EXC_FLAGS_PARAM
                                        _EXC_MASKS_PARAM
                                        _EXC_INFO_PARAM);
     BID_EXTERN_C short bid64_to_int16_xfloor (BID_UINT64 x _EXC_FLAGS_PARAM
                                         _EXC_MASKS_PARAM
                                         _EXC_INFO_PARAM);
     BID_EXTERN_C short bid64_to_int16_ceil (BID_UINT64 x _EXC_FLAGS_PARAM
                                       _EXC_MASKS_PARAM
                                       _EXC_INFO_PARAM);
     BID_EXTERN_C short bid64_to_int16_xceil (BID_UINT64 x _EXC_FLAGS_PARAM
                                        _EXC_MASKS_PARAM
                                        _EXC_INFO_PARAM);
     BID_EXTERN_C unsigned char bid64_to_uint8_rnint (BID_UINT64 x
                                                _EXC_FLAGS_PARAM
                                                _EXC_MASKS_PARAM
                                                _EXC_INFO_PARAM);
     BID_EXTERN_C unsigned char bid64_to_uint8_xrnint (BID_UINT64 x
                                                 _EXC_FLAGS_PARAM
                                                 _EXC_MASKS_PARAM
                                                 _EXC_INFO_PARAM);
     BID_EXTERN_C unsigned char bid64_to_uint8_rninta (BID_UINT64 x
                                                 _EXC_FLAGS_PARAM
                                                 _EXC_MASKS_PARAM
                                                 _EXC_INFO_PARAM);
     BID_EXTERN_C unsigned char bid64_to_uint8_xrninta (BID_UINT64 x
                                                  _EXC_FLAGS_PARAM
                                                  _EXC_MASKS_PARAM
                                                  _EXC_INFO_PARAM);
     BID_EXTERN_C unsigned char bid64_to_uint8_int (BID_UINT64 x _EXC_FLAGS_PARAM
                                              _EXC_MASKS_PARAM
                                              _EXC_INFO_PARAM);
     BID_EXTERN_C unsigned char bid64_to_uint8_xint (BID_UINT64 x _EXC_FLAGS_PARAM
                                               _EXC_MASKS_PARAM
                                               _EXC_INFO_PARAM);
     BID_EXTERN_C unsigned char bid64_to_uint8_floor (BID_UINT64 x
                                                _EXC_FLAGS_PARAM
                                                _EXC_MASKS_PARAM
                                                _EXC_INFO_PARAM);
     BID_EXTERN_C unsigned char bid64_to_uint8_xfloor (BID_UINT64 x
                                                 _EXC_FLAGS_PARAM
                                                 _EXC_MASKS_PARAM
                                                 _EXC_INFO_PARAM);
     BID_EXTERN_C unsigned char bid64_to_uint8_ceil (BID_UINT64 x _EXC_FLAGS_PARAM
                                               _EXC_MASKS_PARAM
                                               _EXC_INFO_PARAM);
     BID_EXTERN_C unsigned char bid64_to_uint8_xceil (BID_UINT64 x
                                                _EXC_FLAGS_PARAM
                                                _EXC_MASKS_PARAM
                                                _EXC_INFO_PARAM);
     BID_EXTERN_C unsigned short bid64_to_uint16_rnint (BID_UINT64 x
                                                  _EXC_FLAGS_PARAM
                                                  _EXC_MASKS_PARAM
                                                  _EXC_INFO_PARAM);
     BID_EXTERN_C unsigned short bid64_to_uint16_xrnint (BID_UINT64 x
                                                   _EXC_FLAGS_PARAM
                                                   _EXC_MASKS_PARAM
                                                   _EXC_INFO_PARAM);
     BID_EXTERN_C unsigned short bid64_to_uint16_rninta (BID_UINT64 x
                                                   _EXC_FLAGS_PARAM
                                                   _EXC_MASKS_PARAM
                                                   _EXC_INFO_PARAM);
     BID_EXTERN_C unsigned short bid64_to_uint16_xrninta (BID_UINT64 x
                                                    _EXC_FLAGS_PARAM
                                                    _EXC_MASKS_PARAM
                                                    _EXC_INFO_PARAM);
     BID_EXTERN_C unsigned short bid64_to_uint16_int (BID_UINT64 x
                                                _EXC_FLAGS_PARAM
                                                _EXC_MASKS_PARAM
                                                _EXC_INFO_PARAM);
     BID_EXTERN_C unsigned short bid64_to_uint16_xint (BID_UINT64 x
                                                 _EXC_FLAGS_PARAM
                                                 _EXC_MASKS_PARAM
                                                 _EXC_INFO_PARAM);
     BID_EXTERN_C unsigned short bid64_to_uint16_floor (BID_UINT64 x
                                                  _EXC_FLAGS_PARAM
                                                  _EXC_MASKS_PARAM
                                                  _EXC_INFO_PARAM);
     BID_EXTERN_C unsigned short bid64_to_uint16_xfloor (BID_UINT64 x
                                                   _EXC_FLAGS_PARAM
                                                   _EXC_MASKS_PARAM
                                                   _EXC_INFO_PARAM);
     BID_EXTERN_C unsigned short bid64_to_uint16_ceil (BID_UINT64 x
                                                 _EXC_FLAGS_PARAM
                                                 _EXC_MASKS_PARAM
                                                 _EXC_INFO_PARAM);
     BID_EXTERN_C unsigned short bid64_to_uint16_xceil (BID_UINT64 x
                                                  _EXC_FLAGS_PARAM
                                                  _EXC_MASKS_PARAM
                                                  _EXC_INFO_PARAM);
     BID_EXTERN_C unsigned int bid64_to_uint32_rnint (BID_UINT64 x
                                                _EXC_FLAGS_PARAM
                                                _EXC_MASKS_PARAM
                                                _EXC_INFO_PARAM);
     BID_EXTERN_C unsigned int bid64_to_uint32_xrnint (BID_UINT64 x
                                                 _EXC_FLAGS_PARAM
                                                 _EXC_MASKS_PARAM
                                                 _EXC_INFO_PARAM);
     BID_EXTERN_C unsigned int bid64_to_uint32_rninta (BID_UINT64 x
                                                 _EXC_FLAGS_PARAM
                                                 _EXC_MASKS_PARAM
                                                 _EXC_INFO_PARAM);
     BID_EXTERN_C unsigned int bid64_to_uint32_xrninta (BID_UINT64 x
                                                  _EXC_FLAGS_PARAM
                                                  _EXC_MASKS_PARAM
                                                  _EXC_INFO_PARAM);
     BID_EXTERN_C unsigned int bid64_to_uint32_int (BID_UINT64 x _EXC_FLAGS_PARAM
                                              _EXC_MASKS_PARAM
                                              _EXC_INFO_PARAM);
     BID_EXTERN_C unsigned int bid64_to_uint32_xint (BID_UINT64 x _EXC_FLAGS_PARAM
                                               _EXC_MASKS_PARAM
                                               _EXC_INFO_PARAM);
     BID_EXTERN_C unsigned int bid64_to_uint32_floor (BID_UINT64 x
                                                _EXC_FLAGS_PARAM
                                                _EXC_MASKS_PARAM
                                                _EXC_INFO_PARAM);
     BID_EXTERN_C unsigned int bid64_to_uint32_xfloor (BID_UINT64 x
                                                 _EXC_FLAGS_PARAM
                                                 _EXC_MASKS_PARAM
                                                 _EXC_INFO_PARAM);
     BID_EXTERN_C unsigned int bid64_to_uint32_ceil (BID_UINT64 x _EXC_FLAGS_PARAM
                                               _EXC_MASKS_PARAM
                                               _EXC_INFO_PARAM);
     BID_EXTERN_C unsigned int bid64_to_uint32_xceil (BID_UINT64 x
                                                _EXC_FLAGS_PARAM
                                                _EXC_MASKS_PARAM
                                                _EXC_INFO_PARAM);
     BID_EXTERN_C BID_SINT64 bid64_to_int64_rnint (BID_UINT64 x _EXC_FLAGS_PARAM
                                         _EXC_MASKS_PARAM
                                         _EXC_INFO_PARAM);
     BID_EXTERN_C BID_SINT64 bid64_to_int64_xrnint (BID_UINT64 x _EXC_FLAGS_PARAM
                                          _EXC_MASKS_PARAM
                                          _EXC_INFO_PARAM);
     BID_EXTERN_C BID_SINT64 bid64_to_int64_rninta (BID_UINT64 x _EXC_FLAGS_PARAM
                                          _EXC_MASKS_PARAM
                                          _EXC_INFO_PARAM);
     BID_EXTERN_C BID_SINT64 bid64_to_int64_xrninta (BID_UINT64 x _EXC_FLAGS_PARAM
                                           _EXC_MASKS_PARAM
                                           _EXC_INFO_PARAM);
     BID_EXTERN_C BID_SINT64 bid64_to_int64_int (BID_UINT64 x _EXC_FLAGS_PARAM
                                       _EXC_MASKS_PARAM
                                       _EXC_INFO_PARAM);
     BID_EXTERN_C BID_SINT64 bid64_to_int64_xint (BID_UINT64 x _EXC_FLAGS_PARAM
                                        _EXC_MASKS_PARAM
                                        _EXC_INFO_PARAM);
     BID_EXTERN_C BID_SINT64 bid64_to_int64_floor (BID_UINT64 x _EXC_FLAGS_PARAM
                                         _EXC_MASKS_PARAM
                                         _EXC_INFO_PARAM);
     BID_EXTERN_C BID_SINT64 bid64_to_int64_xfloor (BID_UINT64 x _EXC_FLAGS_PARAM
                                          _EXC_MASKS_PARAM
                                          _EXC_INFO_PARAM);
     BID_EXTERN_C BID_SINT64 bid64_to_int64_ceil (BID_UINT64 x _EXC_FLAGS_PARAM
                                        _EXC_MASKS_PARAM
                                        _EXC_INFO_PARAM);
     BID_EXTERN_C BID_SINT64 bid64_to_int64_xceil (BID_UINT64 x _EXC_FLAGS_PARAM
                                         _EXC_MASKS_PARAM
                                         _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT64 bid64_to_uint64_rnint (BID_UINT64 x _EXC_FLAGS_PARAM
                                          _EXC_MASKS_PARAM
                                          _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT64 bid64_to_uint64_xrnint (BID_UINT64 x _EXC_FLAGS_PARAM
                                           _EXC_MASKS_PARAM
                                           _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT64 bid64_to_uint64_rninta (BID_UINT64 x _EXC_FLAGS_PARAM
                                           _EXC_MASKS_PARAM
                                           _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT64 bid64_to_uint64_xrninta (BID_UINT64 x _EXC_FLAGS_PARAM
                                            _EXC_MASKS_PARAM
                                            _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT64 bid64_to_uint64_int (BID_UINT64 x _EXC_FLAGS_PARAM
                                        _EXC_MASKS_PARAM
                                        _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT64 bid64_to_uint64_xint (BID_UINT64 x _EXC_FLAGS_PARAM
                                         _EXC_MASKS_PARAM
                                         _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT64 bid64_to_uint64_floor (BID_UINT64 x _EXC_FLAGS_PARAM
                                          _EXC_MASKS_PARAM
                                          _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT64 bid64_to_uint64_xfloor (BID_UINT64 x _EXC_FLAGS_PARAM
                                           _EXC_MASKS_PARAM
                                           _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT64 bid64_to_uint64_ceil (BID_UINT64 x _EXC_FLAGS_PARAM
                                         _EXC_MASKS_PARAM
                                         _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT64 bid64_to_uint64_xceil (BID_UINT64 x _EXC_FLAGS_PARAM
                                          _EXC_MASKS_PARAM
                                          _EXC_INFO_PARAM);

     BID_EXTERN_C char bid32_to_int8_rnint (BID_UINT32 x
                                       _EXC_FLAGS_PARAM
                                       _EXC_MASKS_PARAM
                                       _EXC_INFO_PARAM);
     BID_EXTERN_C char bid32_to_int8_xrnint (BID_UINT32 x
                                        _EXC_FLAGS_PARAM
                                        _EXC_MASKS_PARAM
                                        _EXC_INFO_PARAM);
     BID_EXTERN_C char bid32_to_int8_rninta (BID_UINT32 x
                                        _EXC_FLAGS_PARAM
                                        _EXC_MASKS_PARAM
                                        _EXC_INFO_PARAM);
     BID_EXTERN_C char bid32_to_int8_xrninta (BID_UINT32 x
                                         _EXC_FLAGS_PARAM
                                         _EXC_MASKS_PARAM
                                         _EXC_INFO_PARAM);
     BID_EXTERN_C char bid32_to_int8_int (BID_UINT32 x _EXC_FLAGS_PARAM
                                     _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C char bid32_to_int8_xint (BID_UINT32 x _EXC_FLAGS_PARAM
                                      _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C char bid32_to_int8_floor (BID_UINT32 x
                                       _EXC_FLAGS_PARAM
                                       _EXC_MASKS_PARAM
                                       _EXC_INFO_PARAM);
     BID_EXTERN_C char bid32_to_int8_xfloor (BID_UINT32 x
                                        _EXC_FLAGS_PARAM
                                        _EXC_MASKS_PARAM
                                        _EXC_INFO_PARAM);
     BID_EXTERN_C char bid32_to_int8_ceil (BID_UINT32 x _EXC_FLAGS_PARAM
                                      _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C char bid32_to_int8_xceil (BID_UINT32 x
                                       _EXC_FLAGS_PARAM
                                       _EXC_MASKS_PARAM
                                       _EXC_INFO_PARAM);
     BID_EXTERN_C short bid32_to_int16_rnint (BID_UINT32 x
                                         _EXC_FLAGS_PARAM
                                         _EXC_MASKS_PARAM
                                         _EXC_INFO_PARAM);
     BID_EXTERN_C short bid32_to_int16_xrnint (BID_UINT32 x
                                          _EXC_FLAGS_PARAM
                                          _EXC_MASKS_PARAM
                                          _EXC_INFO_PARAM);
     BID_EXTERN_C short bid32_to_int16_rninta (BID_UINT32 x
                                          _EXC_FLAGS_PARAM
                                          _EXC_MASKS_PARAM
                                          _EXC_INFO_PARAM);
     BID_EXTERN_C short bid32_to_int16_xrninta (BID_UINT32 x
                                           _EXC_FLAGS_PARAM
                                           _EXC_MASKS_PARAM
                                           _EXC_INFO_PARAM);
     BID_EXTERN_C short bid32_to_int16_int (BID_UINT32 x _EXC_FLAGS_PARAM
                                       _EXC_MASKS_PARAM
                                       _EXC_INFO_PARAM);
     BID_EXTERN_C short bid32_to_int16_xint (BID_UINT32 x _EXC_FLAGS_PARAM
                                        _EXC_MASKS_PARAM
                                        _EXC_INFO_PARAM);
     BID_EXTERN_C short bid32_to_int16_floor (BID_UINT32 x
                                         _EXC_FLAGS_PARAM
                                         _EXC_MASKS_PARAM
                                         _EXC_INFO_PARAM);
     BID_EXTERN_C short bid32_to_int16_xfloor (BID_UINT32 x
                                          _EXC_FLAGS_PARAM
                                          _EXC_MASKS_PARAM
                                          _EXC_INFO_PARAM);
     BID_EXTERN_C short bid32_to_int16_ceil (BID_UINT32 x _EXC_FLAGS_PARAM
                                        _EXC_MASKS_PARAM
                                        _EXC_INFO_PARAM);
     BID_EXTERN_C short bid32_to_int16_xceil (BID_UINT32 x
                                         _EXC_FLAGS_PARAM
                                         _EXC_MASKS_PARAM
                                         _EXC_INFO_PARAM);
     BID_EXTERN_C unsigned char bid32_to_uint8_rnint (BID_UINT32 x
                                                 _EXC_FLAGS_PARAM
                                                 _EXC_MASKS_PARAM
                                                 _EXC_INFO_PARAM);
     BID_EXTERN_C unsigned char bid32_to_uint8_xrnint (BID_UINT32 x
                                                  _EXC_FLAGS_PARAM
                                                  _EXC_MASKS_PARAM
                                                  _EXC_INFO_PARAM);
     BID_EXTERN_C unsigned char bid32_to_uint8_rninta (BID_UINT32 x
                                                  _EXC_FLAGS_PARAM
                                                  _EXC_MASKS_PARAM
                                                  _EXC_INFO_PARAM);
     BID_EXTERN_C unsigned char bid32_to_uint8_xrninta (BID_UINT32 x
                                                   _EXC_FLAGS_PARAM
                                                   _EXC_MASKS_PARAM
                                                   _EXC_INFO_PARAM);
     BID_EXTERN_C unsigned char bid32_to_uint8_int (BID_UINT32 x
                                               _EXC_FLAGS_PARAM
                                               _EXC_MASKS_PARAM
                                               _EXC_INFO_PARAM);
     BID_EXTERN_C unsigned char bid32_to_uint8_xint (BID_UINT32 x
                                                _EXC_FLAGS_PARAM
                                                _EXC_MASKS_PARAM
                                                _EXC_INFO_PARAM);
     BID_EXTERN_C unsigned char bid32_to_uint8_floor (BID_UINT32 x
                                                 _EXC_FLAGS_PARAM
                                                 _EXC_MASKS_PARAM
                                                 _EXC_INFO_PARAM);
     BID_EXTERN_C unsigned char bid32_to_uint8_xfloor (BID_UINT32 x
                                                  _EXC_FLAGS_PARAM
                                                  _EXC_MASKS_PARAM
                                                  _EXC_INFO_PARAM);
     BID_EXTERN_C unsigned char bid32_to_uint8_ceil (BID_UINT32 x
                                                _EXC_FLAGS_PARAM
                                                _EXC_MASKS_PARAM
                                                _EXC_INFO_PARAM);
     BID_EXTERN_C unsigned char bid32_to_uint8_xceil (BID_UINT32 x
                                                 _EXC_FLAGS_PARAM
                                                 _EXC_MASKS_PARAM
                                                 _EXC_INFO_PARAM);
     BID_EXTERN_C unsigned short bid32_to_uint16_rnint (BID_UINT32 x
                                                   _EXC_FLAGS_PARAM
                                                   _EXC_MASKS_PARAM
                                                   _EXC_INFO_PARAM);
     BID_EXTERN_C unsigned short bid32_to_uint16_xrnint (BID_UINT32 x
                                                    _EXC_FLAGS_PARAM
                                                    _EXC_MASKS_PARAM
                                                    _EXC_INFO_PARAM);
     BID_EXTERN_C unsigned short bid32_to_uint16_rninta (BID_UINT32 x
                                                    _EXC_FLAGS_PARAM
                                                    _EXC_MASKS_PARAM
                                                    _EXC_INFO_PARAM);
     BID_EXTERN_C unsigned short bid32_to_uint16_xrninta (BID_UINT32 x
                                                     _EXC_FLAGS_PARAM
                                                     _EXC_MASKS_PARAM
                                                     _EXC_INFO_PARAM);
     BID_EXTERN_C unsigned short bid32_to_uint16_int (BID_UINT32 x
                                                 _EXC_FLAGS_PARAM
                                                 _EXC_MASKS_PARAM
                                                 _EXC_INFO_PARAM);
     BID_EXTERN_C unsigned short bid32_to_uint16_xint (BID_UINT32 x
                                                  _EXC_FLAGS_PARAM
                                                  _EXC_MASKS_PARAM
                                                  _EXC_INFO_PARAM);
     BID_EXTERN_C unsigned short bid32_to_uint16_floor (BID_UINT32 x
                                                   _EXC_FLAGS_PARAM
                                                   _EXC_MASKS_PARAM
                                                   _EXC_INFO_PARAM);
     BID_EXTERN_C unsigned short bid32_to_uint16_xfloor (BID_UINT32 x
                                                    _EXC_FLAGS_PARAM
                                                    _EXC_MASKS_PARAM
                                                    _EXC_INFO_PARAM);
     BID_EXTERN_C unsigned short bid32_to_uint16_ceil (BID_UINT32 x
                                                  _EXC_FLAGS_PARAM
                                                  _EXC_MASKS_PARAM
                                                  _EXC_INFO_PARAM);
     BID_EXTERN_C unsigned short bid32_to_uint16_xceil (BID_UINT32 x
                                                   _EXC_FLAGS_PARAM
                                                   _EXC_MASKS_PARAM
                                                   _EXC_INFO_PARAM);
     BID_EXTERN_C int bid32_to_int32_rnint (BID_UINT32 x _EXC_FLAGS_PARAM
                                       _EXC_MASKS_PARAM
                                       _EXC_INFO_PARAM);
     BID_EXTERN_C int bid32_to_int32_xrnint (BID_UINT32 x _EXC_FLAGS_PARAM
                                        _EXC_MASKS_PARAM
                                        _EXC_INFO_PARAM);
     BID_EXTERN_C int bid32_to_int32_rninta (BID_UINT32 x _EXC_FLAGS_PARAM
                                        _EXC_MASKS_PARAM
                                        _EXC_INFO_PARAM);
     BID_EXTERN_C int bid32_to_int32_xrninta (BID_UINT32 x _EXC_FLAGS_PARAM
                                         _EXC_MASKS_PARAM
                                         _EXC_INFO_PARAM);
     BID_EXTERN_C int bid32_to_int32_int (BID_UINT32 x _EXC_FLAGS_PARAM
                                     _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C int bid32_to_int32_xint (BID_UINT32 x _EXC_FLAGS_PARAM
                                      _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C int bid32_to_int32_floor (BID_UINT32 x _EXC_FLAGS_PARAM
                                       _EXC_MASKS_PARAM
                                       _EXC_INFO_PARAM);
     BID_EXTERN_C int bid32_to_int32_xfloor (BID_UINT32 x _EXC_FLAGS_PARAM
                                        _EXC_MASKS_PARAM
                                        _EXC_INFO_PARAM);
     BID_EXTERN_C int bid32_to_int32_ceil (BID_UINT32 x _EXC_FLAGS_PARAM
                                      _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C int bid32_to_int32_xceil (BID_UINT32 x _EXC_FLAGS_PARAM
                                       _EXC_MASKS_PARAM
                                       _EXC_INFO_PARAM);
     BID_EXTERN_C unsigned int bid32_to_uint32_rnint (BID_UINT32 x
                                                 _EXC_FLAGS_PARAM
                                                 _EXC_MASKS_PARAM
                                                 _EXC_INFO_PARAM);
     BID_EXTERN_C unsigned int bid32_to_uint32_xrnint (BID_UINT32 x
                                                  _EXC_FLAGS_PARAM
                                                  _EXC_MASKS_PARAM
                                                  _EXC_INFO_PARAM);
     BID_EXTERN_C unsigned int bid32_to_uint32_rninta (BID_UINT32 x
                                                  _EXC_FLAGS_PARAM
                                                  _EXC_MASKS_PARAM
                                                  _EXC_INFO_PARAM);
     BID_EXTERN_C unsigned int bid32_to_uint32_xrninta (BID_UINT32 x
                                                   _EXC_FLAGS_PARAM
                                                   _EXC_MASKS_PARAM
                                                   _EXC_INFO_PARAM);
     BID_EXTERN_C unsigned int bid32_to_uint32_int (BID_UINT32 x
                                               _EXC_FLAGS_PARAM
                                               _EXC_MASKS_PARAM
                                               _EXC_INFO_PARAM);
     BID_EXTERN_C unsigned int bid32_to_uint32_xint (BID_UINT32 x
                                                _EXC_FLAGS_PARAM
                                                _EXC_MASKS_PARAM
                                                _EXC_INFO_PARAM);
     BID_EXTERN_C unsigned int bid32_to_uint32_floor (BID_UINT32 x
                                                 _EXC_FLAGS_PARAM
                                                 _EXC_MASKS_PARAM
                                                 _EXC_INFO_PARAM);
     BID_EXTERN_C unsigned int bid32_to_uint32_xfloor (BID_UINT32 x
                                                  _EXC_FLAGS_PARAM
                                                  _EXC_MASKS_PARAM
                                                  _EXC_INFO_PARAM);
     BID_EXTERN_C unsigned int bid32_to_uint32_ceil (BID_UINT32 x
                                                _EXC_FLAGS_PARAM
                                                _EXC_MASKS_PARAM
                                                _EXC_INFO_PARAM);
     BID_EXTERN_C unsigned int bid32_to_uint32_xceil (BID_UINT32 x
                                                 _EXC_FLAGS_PARAM
                                                 _EXC_MASKS_PARAM
                                                 _EXC_INFO_PARAM);
     BID_EXTERN_C BID_SINT64 bid32_to_int64_rnint (BID_UINT32 x _EXC_FLAGS_PARAM
                                          _EXC_MASKS_PARAM
                                          _EXC_INFO_PARAM);
     BID_EXTERN_C BID_SINT64 bid32_to_int64_xrnint (BID_UINT32 x _EXC_FLAGS_PARAM
                                           _EXC_MASKS_PARAM
                                           _EXC_INFO_PARAM);
     BID_EXTERN_C BID_SINT64 bid32_to_int64_rninta (BID_UINT32 x _EXC_FLAGS_PARAM
                                           _EXC_MASKS_PARAM
                                           _EXC_INFO_PARAM);
     BID_EXTERN_C BID_SINT64 bid32_to_int64_xrninta (BID_UINT32 x _EXC_FLAGS_PARAM
                                            _EXC_MASKS_PARAM
                                            _EXC_INFO_PARAM);
     BID_EXTERN_C BID_SINT64 bid32_to_int64_int (BID_UINT32 x _EXC_FLAGS_PARAM
                                        _EXC_MASKS_PARAM
                                        _EXC_INFO_PARAM);
     BID_EXTERN_C BID_SINT64 bid32_to_int64_xint (BID_UINT32 x _EXC_FLAGS_PARAM
                                         _EXC_MASKS_PARAM
                                         _EXC_INFO_PARAM);
     BID_EXTERN_C BID_SINT64 bid32_to_int64_floor (BID_UINT32 x _EXC_FLAGS_PARAM
                                          _EXC_MASKS_PARAM
                                          _EXC_INFO_PARAM);
     BID_EXTERN_C BID_SINT64 bid32_to_int64_xfloor (BID_UINT32 x _EXC_FLAGS_PARAM
                                           _EXC_MASKS_PARAM
                                           _EXC_INFO_PARAM);
     BID_EXTERN_C BID_SINT64 bid32_to_int64_ceil (BID_UINT32 x _EXC_FLAGS_PARAM
                                         _EXC_MASKS_PARAM
                                         _EXC_INFO_PARAM);
     BID_EXTERN_C BID_SINT64 bid32_to_int64_xceil (BID_UINT32 x _EXC_FLAGS_PARAM
                                          _EXC_MASKS_PARAM
                                          _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT64 bid32_to_uint64_rnint (BID_UINT32 x _EXC_FLAGS_PARAM
                                           _EXC_MASKS_PARAM
                                           _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT64 bid32_to_uint64_xrnint (BID_UINT32 x _EXC_FLAGS_PARAM
                                            _EXC_MASKS_PARAM
                                            _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT64 bid32_to_uint64_rninta (BID_UINT32 x _EXC_FLAGS_PARAM
                                            _EXC_MASKS_PARAM
                                            _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT64 bid32_to_uint64_xrninta (BID_UINT32 x _EXC_FLAGS_PARAM
                                             _EXC_MASKS_PARAM
                                             _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT64 bid32_to_uint64_int (BID_UINT32 x _EXC_FLAGS_PARAM
                                         _EXC_MASKS_PARAM
                                         _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT64 bid32_to_uint64_xint (BID_UINT32 x _EXC_FLAGS_PARAM
                                          _EXC_MASKS_PARAM
                                          _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT64 bid32_to_uint64_floor (BID_UINT32 x _EXC_FLAGS_PARAM
                                           _EXC_MASKS_PARAM
                                           _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT64 bid32_to_uint64_xfloor (BID_UINT32 x _EXC_FLAGS_PARAM
                                            _EXC_MASKS_PARAM
                                            _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT64 bid32_to_uint64_ceil (BID_UINT32 x _EXC_FLAGS_PARAM
                                          _EXC_MASKS_PARAM
                                          _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT64 bid32_to_uint64_xceil (BID_UINT32 x _EXC_FLAGS_PARAM
                                           _EXC_MASKS_PARAM
                                           _EXC_INFO_PARAM);

     BID_EXTERN_C int bid64_quiet_equal (BID_UINT64 x, BID_UINT64 y
                                   _EXC_FLAGS_PARAM _EXC_MASKS_PARAM
                                   _EXC_INFO_PARAM);
     BID_EXTERN_C int bid64_quiet_greater (BID_UINT64 x,
                                     BID_UINT64 y _EXC_FLAGS_PARAM
                                     _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C int bid64_quiet_greater_equal (BID_UINT64 x,
                                           BID_UINT64 y _EXC_FLAGS_PARAM
                                           _EXC_MASKS_PARAM
                                           _EXC_INFO_PARAM);
     BID_EXTERN_C int bid64_quiet_greater_unordered (BID_UINT64 x,
                                               BID_UINT64 y _EXC_FLAGS_PARAM
                                               _EXC_MASKS_PARAM
                                               _EXC_INFO_PARAM);
     BID_EXTERN_C int bid64_quiet_less (BID_UINT64 x,
                                  BID_UINT64 y _EXC_FLAGS_PARAM
                                  _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C int bid64_quiet_less_equal (BID_UINT64 x,
                                        BID_UINT64 y _EXC_FLAGS_PARAM
                                        _EXC_MASKS_PARAM
                                        _EXC_INFO_PARAM);
     BID_EXTERN_C int bid64_quiet_less_unordered (BID_UINT64 x,
                                            BID_UINT64 y _EXC_FLAGS_PARAM
                                            _EXC_MASKS_PARAM
                                            _EXC_INFO_PARAM);
     BID_EXTERN_C int bid64_quiet_not_equal (BID_UINT64 x,
                                       BID_UINT64 y _EXC_FLAGS_PARAM
                                       _EXC_MASKS_PARAM
                                       _EXC_INFO_PARAM);
     BID_EXTERN_C int bid64_quiet_not_greater (BID_UINT64 x,
                                         BID_UINT64 y _EXC_FLAGS_PARAM
                                         _EXC_MASKS_PARAM
                                         _EXC_INFO_PARAM);
     BID_EXTERN_C int bid64_quiet_not_less (BID_UINT64 x,
                                      BID_UINT64 y _EXC_FLAGS_PARAM
                                      _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C int bid64_quiet_ordered (BID_UINT64 x,
                                     BID_UINT64 y _EXC_FLAGS_PARAM
                                     _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C int bid64_quiet_unordered (BID_UINT64 x,
                                       BID_UINT64 y _EXC_FLAGS_PARAM
                                       _EXC_MASKS_PARAM
                                       _EXC_INFO_PARAM);
     BID_EXTERN_C int bid64_signaling_greater (BID_UINT64 x,
                                         BID_UINT64 y _EXC_FLAGS_PARAM
                                         _EXC_MASKS_PARAM
                                         _EXC_INFO_PARAM);
     BID_EXTERN_C int bid64_signaling_greater_equal (BID_UINT64 x,
                                               BID_UINT64 y _EXC_FLAGS_PARAM
                                               _EXC_MASKS_PARAM
                                               _EXC_INFO_PARAM);
     BID_EXTERN_C int bid64_signaling_greater_unordered (BID_UINT64 x,
                                                   BID_UINT64 y
                                                   _EXC_FLAGS_PARAM
                                                   _EXC_MASKS_PARAM
                                                   _EXC_INFO_PARAM);
     BID_EXTERN_C int bid64_signaling_less (BID_UINT64 x,
                                      BID_UINT64 y _EXC_FLAGS_PARAM
                                      _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C int bid64_signaling_less_equal (BID_UINT64 x,
                                            BID_UINT64 y _EXC_FLAGS_PARAM
                                            _EXC_MASKS_PARAM
                                            _EXC_INFO_PARAM);
     BID_EXTERN_C int bid64_signaling_less_unordered (BID_UINT64 x,
                                                BID_UINT64 y
                                                _EXC_FLAGS_PARAM
                                                _EXC_MASKS_PARAM
                                                _EXC_INFO_PARAM);
     BID_EXTERN_C int bid64_signaling_not_greater (BID_UINT64 x,
                                             BID_UINT64 y _EXC_FLAGS_PARAM
                                             _EXC_MASKS_PARAM
                                             _EXC_INFO_PARAM);
     BID_EXTERN_C int bid64_signaling_not_less (BID_UINT64 x,
                                          BID_UINT64 y _EXC_FLAGS_PARAM
                                          _EXC_MASKS_PARAM
                                          _EXC_INFO_PARAM);

     BID_EXTERN_C int bid128_quiet_equal (BID_UINT128 x, BID_UINT128 y
                                    _EXC_FLAGS_PARAM _EXC_MASKS_PARAM
                                    _EXC_INFO_PARAM);
     BID_EXTERN_C int bid128_quiet_greater (BID_UINT128 x,
                                      BID_UINT128 y _EXC_FLAGS_PARAM
                                      _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C int bid128_quiet_greater_equal (BID_UINT128 x,
                                            BID_UINT128 y _EXC_FLAGS_PARAM
                                            _EXC_MASKS_PARAM
                                            _EXC_INFO_PARAM);
     BID_EXTERN_C int bid128_quiet_greater_unordered (BID_UINT128 x,
                                                BID_UINT128 y
                                                _EXC_FLAGS_PARAM
                                                _EXC_MASKS_PARAM
                                                _EXC_INFO_PARAM);
     BID_EXTERN_C int bid128_quiet_less (BID_UINT128 x,
                                   BID_UINT128 y _EXC_FLAGS_PARAM
                                   _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C int bid128_quiet_less_equal (BID_UINT128 x,
                                         BID_UINT128 y _EXC_FLAGS_PARAM
                                         _EXC_MASKS_PARAM
                                         _EXC_INFO_PARAM);
     BID_EXTERN_C int bid128_quiet_less_unordered (BID_UINT128 x,
                                             BID_UINT128 y _EXC_FLAGS_PARAM
                                             _EXC_MASKS_PARAM
                                             _EXC_INFO_PARAM);
     BID_EXTERN_C int bid128_quiet_not_equal (BID_UINT128 x,
                                        BID_UINT128 y _EXC_FLAGS_PARAM
                                        _EXC_MASKS_PARAM
                                        _EXC_INFO_PARAM);
     BID_EXTERN_C int bid128_quiet_not_greater (BID_UINT128 x,
                                          BID_UINT128 y _EXC_FLAGS_PARAM
                                          _EXC_MASKS_PARAM
                                          _EXC_INFO_PARAM);
     BID_EXTERN_C int bid128_quiet_not_less (BID_UINT128 x,
                                       BID_UINT128 y _EXC_FLAGS_PARAM
                                       _EXC_MASKS_PARAM
                                       _EXC_INFO_PARAM);
     BID_EXTERN_C int bid128_quiet_ordered (BID_UINT128 x,
                                      BID_UINT128 y _EXC_FLAGS_PARAM
                                      _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C int bid128_quiet_unordered (BID_UINT128 x,
                                        BID_UINT128 y _EXC_FLAGS_PARAM
                                        _EXC_MASKS_PARAM
                                        _EXC_INFO_PARAM);
     BID_EXTERN_C int bid128_signaling_greater (BID_UINT128 x,
                                          BID_UINT128 y _EXC_FLAGS_PARAM
                                          _EXC_MASKS_PARAM
                                          _EXC_INFO_PARAM);
     BID_EXTERN_C int bid128_signaling_greater_equal (BID_UINT128 x,
                                                BID_UINT128 y
                                                _EXC_FLAGS_PARAM
                                                _EXC_MASKS_PARAM
                                                _EXC_INFO_PARAM);
     BID_EXTERN_C int bid128_signaling_greater_unordered (BID_UINT128 x,
                                                    BID_UINT128 y
                                                    _EXC_FLAGS_PARAM
                                                    _EXC_MASKS_PARAM
                                                    _EXC_INFO_PARAM);
     BID_EXTERN_C int bid128_signaling_less (BID_UINT128 x,
                                       BID_UINT128 y _EXC_FLAGS_PARAM
                                       _EXC_MASKS_PARAM
                                       _EXC_INFO_PARAM);
     BID_EXTERN_C int bid128_signaling_less_equal (BID_UINT128 x,
                                             BID_UINT128 y _EXC_FLAGS_PARAM
                                             _EXC_MASKS_PARAM
                                             _EXC_INFO_PARAM);
     BID_EXTERN_C int bid128_signaling_less_unordered (BID_UINT128 x,
                                                 BID_UINT128 y
                                                 _EXC_FLAGS_PARAM
                                                 _EXC_MASKS_PARAM
                                                 _EXC_INFO_PARAM);
     BID_EXTERN_C int bid128_signaling_not_greater (BID_UINT128 x,
                                              BID_UINT128 y _EXC_FLAGS_PARAM
                                              _EXC_MASKS_PARAM
                                              _EXC_INFO_PARAM);
     BID_EXTERN_C int bid128_signaling_not_less (BID_UINT128 x,
                                           BID_UINT128 y _EXC_FLAGS_PARAM
                                           _EXC_MASKS_PARAM
                                           _EXC_INFO_PARAM);

     BID_EXTERN_C int bid32_quiet_equal (BID_UINT32 x, BID_UINT32 y
                                   _EXC_FLAGS_PARAM _EXC_MASKS_PARAM
                                   _EXC_INFO_PARAM);
     BID_EXTERN_C int bid32_quiet_greater (BID_UINT32 x,
                                     BID_UINT32 y _EXC_FLAGS_PARAM
                                     _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C int bid32_quiet_greater_equal (BID_UINT32 x,
                                           BID_UINT32 y _EXC_FLAGS_PARAM
                                           _EXC_MASKS_PARAM
                                           _EXC_INFO_PARAM);
     BID_EXTERN_C int bid32_quiet_greater_unordered (BID_UINT32 x,
                                               BID_UINT32 y _EXC_FLAGS_PARAM
                                               _EXC_MASKS_PARAM
                                               _EXC_INFO_PARAM);
     BID_EXTERN_C int bid32_quiet_less (BID_UINT32 x,
                                  BID_UINT32 y _EXC_FLAGS_PARAM
                                  _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C int bid32_quiet_less_equal (BID_UINT32 x,
                                        BID_UINT32 y _EXC_FLAGS_PARAM
                                        _EXC_MASKS_PARAM
                                        _EXC_INFO_PARAM);
     BID_EXTERN_C int bid32_quiet_less_unordered (BID_UINT32 x,
                                            BID_UINT32 y _EXC_FLAGS_PARAM
                                            _EXC_MASKS_PARAM
                                            _EXC_INFO_PARAM);
     BID_EXTERN_C int bid32_quiet_not_equal (BID_UINT32 x,
                                       BID_UINT32 y _EXC_FLAGS_PARAM
                                       _EXC_MASKS_PARAM
                                       _EXC_INFO_PARAM);
     BID_EXTERN_C int bid32_quiet_not_greater (BID_UINT32 x,
                                         BID_UINT32 y _EXC_FLAGS_PARAM
                                         _EXC_MASKS_PARAM
                                         _EXC_INFO_PARAM);
     BID_EXTERN_C int bid32_quiet_not_less (BID_UINT32 x,
                                      BID_UINT32 y _EXC_FLAGS_PARAM
                                      _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C int bid32_quiet_ordered (BID_UINT32 x,
                                     BID_UINT32 y _EXC_FLAGS_PARAM
                                     _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C int bid32_quiet_unordered (BID_UINT32 x,
                                       BID_UINT32 y _EXC_FLAGS_PARAM
                                       _EXC_MASKS_PARAM
                                       _EXC_INFO_PARAM);
     BID_EXTERN_C int bid32_signaling_greater (BID_UINT32 x,
                                         BID_UINT32 y _EXC_FLAGS_PARAM
                                         _EXC_MASKS_PARAM
                                         _EXC_INFO_PARAM);
     BID_EXTERN_C int bid32_signaling_greater_equal (BID_UINT32 x,
                                               BID_UINT32 y _EXC_FLAGS_PARAM
                                               _EXC_MASKS_PARAM
                                               _EXC_INFO_PARAM);
     BID_EXTERN_C int bid32_signaling_greater_unordered (BID_UINT32 x,
                                                   BID_UINT32 y
                                                   _EXC_FLAGS_PARAM
                                                   _EXC_MASKS_PARAM
                                                   _EXC_INFO_PARAM);
     BID_EXTERN_C int bid32_signaling_less (BID_UINT32 x,
                                      BID_UINT32 y _EXC_FLAGS_PARAM
                                      _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C int bid32_signaling_less_equal (BID_UINT32 x,
                                            BID_UINT32 y _EXC_FLAGS_PARAM
                                            _EXC_MASKS_PARAM
                                            _EXC_INFO_PARAM);
     BID_EXTERN_C int bid32_signaling_less_unordered (BID_UINT32 x,
                                                BID_UINT32 y
                                                _EXC_FLAGS_PARAM
                                                _EXC_MASKS_PARAM
                                                _EXC_INFO_PARAM);
     BID_EXTERN_C int bid32_signaling_not_greater (BID_UINT32 x,
                                             BID_UINT32 y _EXC_FLAGS_PARAM
                                             _EXC_MASKS_PARAM
                                             _EXC_INFO_PARAM);
     BID_EXTERN_C int bid32_signaling_not_less (BID_UINT32 x,
                                          BID_UINT32 y _EXC_FLAGS_PARAM
                                          _EXC_MASKS_PARAM
                                          _EXC_INFO_PARAM);

     BID_EXTERN_C BID_UINT32 bid32_round_integral_exact (BID_UINT32 x
                                               _RND_MODE_PARAM
                                               _EXC_FLAGS_PARAM
                                               _EXC_MASKS_PARAM
                                               _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT32 bid32_round_integral_nearest_even (BID_UINT32 x
                                                      _EXC_FLAGS_PARAM
                                                      _EXC_MASKS_PARAM
                                                      _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT32 bid32_round_integral_negative (BID_UINT32 x
                                                  _EXC_FLAGS_PARAM
                                                  _EXC_MASKS_PARAM
                                                  _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT32 bid32_round_integral_positive (BID_UINT32 x
                                                  _EXC_FLAGS_PARAM
                                                  _EXC_MASKS_PARAM
                                                  _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT32 bid32_round_integral_zero (BID_UINT32 x _EXC_FLAGS_PARAM
                                              _EXC_MASKS_PARAM
                                              _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT32 bid32_round_integral_nearest_away (BID_UINT32 x
                                                      _EXC_FLAGS_PARAM
                                                      _EXC_MASKS_PARAM
                                                      _EXC_INFO_PARAM);

     BID_EXTERN_C BID_UINT64 bid64_round_integral_exact (BID_UINT64 x
                                               _RND_MODE_PARAM
                                               _EXC_FLAGS_PARAM
                                               _EXC_MASKS_PARAM
                                               _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT64 bid64_round_integral_nearest_even (BID_UINT64 x
                                                      _EXC_FLAGS_PARAM
                                                      _EXC_MASKS_PARAM
                                                      _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT64 bid64_round_integral_negative (BID_UINT64 x
                                                  _EXC_FLAGS_PARAM
                                                  _EXC_MASKS_PARAM
                                                  _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT64 bid64_round_integral_positive (BID_UINT64 x
                                                  _EXC_FLAGS_PARAM
                                                  _EXC_MASKS_PARAM
                                                  _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT64 bid64_round_integral_zero (BID_UINT64 x _EXC_FLAGS_PARAM
                                              _EXC_MASKS_PARAM
                                              _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT64 bid64_round_integral_nearest_away (BID_UINT64 x
                                                      _EXC_FLAGS_PARAM
                                                      _EXC_MASKS_PARAM
                                                      _EXC_INFO_PARAM);

     BID_EXTERN_C BID_UINT128 bid128_round_integral_exact (BID_UINT128 x
                                                 _RND_MODE_PARAM
                                                 _EXC_FLAGS_PARAM
                                                 _EXC_MASKS_PARAM
                                                 _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT128 bid128_round_integral_nearest_even (BID_UINT128 x
                                                        _EXC_FLAGS_PARAM
                                                        _EXC_MASKS_PARAM
                                                        _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT128 bid128_round_integral_negative (BID_UINT128 x
                                                    _EXC_FLAGS_PARAM
                                                    _EXC_MASKS_PARAM
                                                    _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT128 bid128_round_integral_positive (BID_UINT128 x
                                                    _EXC_FLAGS_PARAM
                                                    _EXC_MASKS_PARAM
                                                    _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT128 bid128_round_integral_zero (BID_UINT128 x
                                                _EXC_FLAGS_PARAM
                                                _EXC_MASKS_PARAM
                                                _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT128 bid128_round_integral_nearest_away (BID_UINT128 x
                                                        _EXC_FLAGS_PARAM
                                                        _EXC_MASKS_PARAM
                                                        _EXC_INFO_PARAM);

     BID_EXTERN_C BID_UINT32 bid32_nextup (BID_UINT32 x
                                 _EXC_FLAGS_PARAM _EXC_MASKS_PARAM
                                 _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT32 bid32_nextdown (BID_UINT32 x _EXC_FLAGS_PARAM
                                   _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT32 bid32_nextafter (BID_UINT32 x,
                                    BID_UINT32 y _EXC_FLAGS_PARAM
                                    _EXC_MASKS_PARAM _EXC_INFO_PARAM);

     BID_EXTERN_C BID_UINT64 bid64_nextup (BID_UINT64 x
                                 _EXC_FLAGS_PARAM _EXC_MASKS_PARAM
                                 _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT64 bid64_nextdown (BID_UINT64 x _EXC_FLAGS_PARAM
                                   _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT64 bid64_nextafter (BID_UINT64 x,
                                    BID_UINT64 y _EXC_FLAGS_PARAM
                                    _EXC_MASKS_PARAM _EXC_INFO_PARAM);

     BID_EXTERN_C BID_UINT128 bid128_nextup (BID_UINT128 x
                                   _EXC_FLAGS_PARAM _EXC_MASKS_PARAM
                                   _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT128 bid128_nextdown (BID_UINT128 x _EXC_FLAGS_PARAM
                                     _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT128 bid128_nextafter (BID_UINT128 x,
                                      BID_UINT128 y _EXC_FLAGS_PARAM
                                      _EXC_MASKS_PARAM _EXC_INFO_PARAM);

     BID_EXTERN_C BID_UINT32 bid32_minnum (BID_UINT32 x, BID_UINT32 y _EXC_FLAGS_PARAM
                                 _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT32 bid32_minnum_mag (BID_UINT32 x, BID_UINT32 y _EXC_FLAGS_PARAM
                                 _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT32 bid32_maxnum (BID_UINT32 x, BID_UINT32 y _EXC_FLAGS_PARAM
                                 _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT32 bid32_maxnum_mag (BID_UINT32 x, BID_UINT32 y _EXC_FLAGS_PARAM
                                 _EXC_MASKS_PARAM _EXC_INFO_PARAM);

     BID_EXTERN_C BID_UINT64 bid64_minnum (BID_UINT64 x, BID_UINT64 y _EXC_FLAGS_PARAM
                                 _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT64 bid64_minnum_mag (BID_UINT64 x, BID_UINT64 y _EXC_FLAGS_PARAM
                                 _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT64 bid64_maxnum (BID_UINT64 x, BID_UINT64 y _EXC_FLAGS_PARAM
                                 _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT64 bid64_maxnum_mag (BID_UINT64 x, BID_UINT64 y _EXC_FLAGS_PARAM
                                 _EXC_MASKS_PARAM _EXC_INFO_PARAM);

     BID_EXTERN_C BID_UINT128 bid128_minnum (BID_UINT128 x, BID_UINT128 y _EXC_FLAGS_PARAM
                                 _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT128 bid128_minnum_mag (BID_UINT128 x, BID_UINT128 y _EXC_FLAGS_PARAM
                                 _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT128 bid128_maxnum (BID_UINT128 x, BID_UINT128 y _EXC_FLAGS_PARAM
                                 _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT128 bid128_maxnum_mag (BID_UINT128 x, BID_UINT128 y _EXC_FLAGS_PARAM
                                 _EXC_MASKS_PARAM _EXC_INFO_PARAM);

     BID_EXTERN_C BID_UINT32 bid32_from_int32 (int x
         _RND_MODE_PARAM _EXC_FLAGS_PARAM _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT32 bid32_from_uint32 (unsigned int x
         _RND_MODE_PARAM _EXC_FLAGS_PARAM _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT32 bid32_from_int64 (BID_SINT64 x _RND_MODE_PARAM
                                     _EXC_FLAGS_PARAM _EXC_MASKS_PARAM
                                     _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT32 bid32_from_uint64 (BID_UINT64 _RND_MODE_PARAM
                                      _EXC_FLAGS_PARAM _EXC_MASKS_PARAM
                                      _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT64 bid64_from_int32 (int x _EXC_MASKS_PARAM
                                     _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT64 bid64_from_uint32 (unsigned int x _EXC_MASKS_PARAM
                                      _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT64 bid64_from_int64 (BID_SINT64 x _RND_MODE_PARAM
                                     _EXC_FLAGS_PARAM _EXC_MASKS_PARAM
                                     _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT64 bid64_from_uint64 (BID_UINT64 _RND_MODE_PARAM
                                      _EXC_FLAGS_PARAM _EXC_MASKS_PARAM
                                      _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT128 bid128_from_int32 (int x _EXC_MASKS_PARAM
                                       _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT128 bid128_from_uint32 (unsigned int x _EXC_MASKS_PARAM
                                        _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT128 bid128_from_int64 (BID_SINT64 x _EXC_MASKS_PARAM
                                       _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT128 bid128_from_uint64 (BID_UINT64 x _EXC_MASKS_PARAM
                                        _EXC_INFO_PARAM);

     BID_EXTERN_C int bid32_isSigned (BID_UINT32 x _EXC_MASKS_PARAM
                                _EXC_INFO_PARAM);
     BID_EXTERN_C int bid32_isNormal (BID_UINT32 x _EXC_MASKS_PARAM
                                _EXC_INFO_PARAM);
     BID_EXTERN_C int bid32_isSubnormal (BID_UINT32 x _EXC_MASKS_PARAM
                                   _EXC_INFO_PARAM);
     BID_EXTERN_C int bid32_isFinite (BID_UINT32 x _EXC_MASKS_PARAM
                                _EXC_INFO_PARAM);
     BID_EXTERN_C int bid32_isZero (BID_UINT32 x _EXC_MASKS_PARAM
                              _EXC_INFO_PARAM);
     BID_EXTERN_C int bid32_isInf (BID_UINT32 x _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C int bid32_isSignaling (BID_UINT32 x _EXC_MASKS_PARAM
                                   _EXC_INFO_PARAM);
     BID_EXTERN_C int bid32_isCanonical (BID_UINT32 x _EXC_MASKS_PARAM
                                   _EXC_INFO_PARAM);
     BID_EXTERN_C int bid32_isNaN (BID_UINT32 x _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT32 bid32_copy (BID_UINT32 x _EXC_MASKS_PARAM
                               _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT32 bid32_negate (BID_UINT32 x _EXC_MASKS_PARAM
                                 _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT32 bid32_abs (BID_UINT32 x _EXC_MASKS_PARAM
                              _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT32 bid32_copySign (BID_UINT32 x,
                                   BID_UINT32 y _EXC_MASKS_PARAM
                                   _EXC_INFO_PARAM);
     BID_EXTERN_C int bid32_class (BID_UINT32 x _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C int bid32_sameQuantum (BID_UINT32 x, BID_UINT32 y
                                   _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C int bid32_totalOrder (BID_UINT32 x, BID_UINT32 y
                                  _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C int bid32_totalOrderMag (BID_UINT32 x, BID_UINT32 y
                                     _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C int bid32_radix (BID_UINT32 x _EXC_MASKS_PARAM _EXC_INFO_PARAM);

     BID_EXTERN_C int bid64_isSigned (BID_UINT64 x _EXC_MASKS_PARAM
                                _EXC_INFO_PARAM);
     BID_EXTERN_C int bid64_isNormal (BID_UINT64 x _EXC_MASKS_PARAM
                                _EXC_INFO_PARAM);
     BID_EXTERN_C int bid64_isSubnormal (BID_UINT64 x _EXC_MASKS_PARAM
                                   _EXC_INFO_PARAM);
     BID_EXTERN_C int bid64_isFinite (BID_UINT64 x _EXC_MASKS_PARAM
                                _EXC_INFO_PARAM);
     BID_EXTERN_C int bid64_isZero (BID_UINT64 x _EXC_MASKS_PARAM
                              _EXC_INFO_PARAM);
     BID_EXTERN_C int bid64_isInf (BID_UINT64 x _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C int bid64_isSignaling (BID_UINT64 x _EXC_MASKS_PARAM
                                   _EXC_INFO_PARAM);
     BID_EXTERN_C int bid64_isCanonical (BID_UINT64 x _EXC_MASKS_PARAM
                                   _EXC_INFO_PARAM);
     BID_EXTERN_C int bid64_isNaN (BID_UINT64 x _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT64 bid64_copy (BID_UINT64 x _EXC_MASKS_PARAM
                               _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT64 bid64_negate (BID_UINT64 x _EXC_MASKS_PARAM
                                 _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT64 bid64_abs (BID_UINT64 x _EXC_MASKS_PARAM
                              _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT64 bid64_copySign (BID_UINT64 x,
                                   BID_UINT64 y _EXC_MASKS_PARAM
                                   _EXC_INFO_PARAM);
     BID_EXTERN_C int bid64_class (BID_UINT64 x _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C int bid64_sameQuantum (BID_UINT64 x, BID_UINT64 y
                                   _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C int bid64_totalOrder (BID_UINT64 x, BID_UINT64 y
                                  _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C int bid64_totalOrderMag (BID_UINT64 x, BID_UINT64 y
                                     _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C int bid64_radix (BID_UINT64 x _EXC_MASKS_PARAM _EXC_INFO_PARAM);

     BID_EXTERN_C int bid128_isSigned (BID_UINT128 x _EXC_MASKS_PARAM
                                 _EXC_INFO_PARAM);
     BID_EXTERN_C int bid128_isNormal (BID_UINT128 x _EXC_MASKS_PARAM
                                 _EXC_INFO_PARAM);
     BID_EXTERN_C int bid128_isSubnormal (BID_UINT128 x _EXC_MASKS_PARAM
                                    _EXC_INFO_PARAM);
     BID_EXTERN_C int bid128_isFinite (BID_UINT128 x _EXC_MASKS_PARAM
                                 _EXC_INFO_PARAM);
     BID_EXTERN_C int bid128_isZero (BID_UINT128 x _EXC_MASKS_PARAM
                               _EXC_INFO_PARAM);
     BID_EXTERN_C int bid128_isInf (BID_UINT128 x _EXC_MASKS_PARAM
                              _EXC_INFO_PARAM);
     BID_EXTERN_C int bid128_isSignaling (BID_UINT128 x _EXC_MASKS_PARAM
                                    _EXC_INFO_PARAM);
     BID_EXTERN_C int bid128_isCanonical (BID_UINT128 x _EXC_MASKS_PARAM
                                    _EXC_INFO_PARAM);
     BID_EXTERN_C int bid128_isNaN (BID_UINT128 x _EXC_MASKS_PARAM
                              _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT128 bid128_copy (BID_UINT128 x _EXC_MASKS_PARAM
                                 _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT128 bid128_negate (BID_UINT128 x _EXC_MASKS_PARAM
                                   _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT128 bid128_abs (BID_UINT128 x _EXC_MASKS_PARAM
                                _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT128 bid128_copySign (BID_UINT128 x,
                                     BID_UINT128 y _EXC_MASKS_PARAM
                                     _EXC_INFO_PARAM);
     BID_EXTERN_C int bid128_class (BID_UINT128 x _EXC_MASKS_PARAM
                              _EXC_INFO_PARAM);
     BID_EXTERN_C int bid128_sameQuantum (BID_UINT128 x,
                                    BID_UINT128 y _EXC_MASKS_PARAM
                                    _EXC_INFO_PARAM);
     BID_EXTERN_C int bid128_totalOrder (BID_UINT128 x,
                                   BID_UINT128 y _EXC_MASKS_PARAM
                                   _EXC_INFO_PARAM);
     BID_EXTERN_C int bid128_totalOrderMag (BID_UINT128 x,
                                      BID_UINT128 y _EXC_MASKS_PARAM
                                      _EXC_INFO_PARAM);
     BID_EXTERN_C int bid128_radix (BID_UINT128 x _EXC_MASKS_PARAM
                              _EXC_INFO_PARAM);

     BID_EXTERN_C BID_UINT32 bid32_rem (BID_UINT32 x, BID_UINT32 y
                              _EXC_FLAGS_PARAM
                              _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C int bid32_ilogb (BID_UINT32 x _EXC_FLAGS_PARAM
                               _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT32 bid32_scalbn (BID_UINT32 x,
                                int n _RND_MODE_PARAM _EXC_FLAGS_PARAM
                                _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT32 bid32_ldexp (BID_UINT32 x,
                                int n _RND_MODE_PARAM _EXC_FLAGS_PARAM
                                _EXC_MASKS_PARAM _EXC_INFO_PARAM);

     BID_EXTERN_C BID_UINT64 bid64_rem (BID_UINT64 x, BID_UINT64 y
                              _EXC_FLAGS_PARAM
                              _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C int bid64_ilogb (BID_UINT64 x _EXC_FLAGS_PARAM
                               _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT64 bid64_scalbn (BID_UINT64 x,
                                int n _RND_MODE_PARAM _EXC_FLAGS_PARAM
                                _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT64 bid64_ldexp (BID_UINT64 x,
                                int n _RND_MODE_PARAM _EXC_FLAGS_PARAM
                                _EXC_MASKS_PARAM _EXC_INFO_PARAM);

     BID_EXTERN_C BID_UINT128 bid128_rem (BID_UINT128 x, BID_UINT128 y
                                _EXC_FLAGS_PARAM
                                _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C int bid128_ilogb (BID_UINT128 x
                                 _EXC_FLAGS_PARAM _EXC_MASKS_PARAM
                                 _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT128 bid128_scalbn (BID_UINT128 x,
                                  int n _RND_MODE_PARAM _EXC_FLAGS_PARAM
                                  _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT128 bid128_ldexp (BID_UINT128 x,
                                  int n _RND_MODE_PARAM _EXC_FLAGS_PARAM
                                  _EXC_MASKS_PARAM _EXC_INFO_PARAM);

     BID_EXTERN_C BID_UINT64 bid32_to_bid64 (BID_UINT32 x _EXC_FLAGS_PARAM
                                   _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT128 bid32_to_bid128 (BID_UINT32 x _EXC_FLAGS_PARAM
                                     _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT128 bid64_to_bid128 (BID_UINT64 x _EXC_FLAGS_PARAM
                                     _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT32 bid64_to_bid32 (BID_UINT64 x
                                   _RND_MODE_PARAM _EXC_FLAGS_PARAM
                                   _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT32 bid128_to_bid32 (BID_UINT128 x _RND_MODE_PARAM
                                    _EXC_FLAGS_PARAM
                                    _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT64 bid128_to_bid64 (BID_UINT128 x _RND_MODE_PARAM
                                    _EXC_FLAGS_PARAM
                                    _EXC_MASKS_PARAM _EXC_INFO_PARAM);

     BID_EXTERN_C void bid32_to_string (char *ps, BID_UINT32 x
                                  _EXC_FLAGS_PARAM
                                  _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT32 bid32_from_string (char *ps
                                      _RND_MODE_PARAM _EXC_FLAGS_PARAM
                                      _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid64_to_string (char *ps, BID_UINT64 x
                                  _EXC_FLAGS_PARAM
                                  _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT64 bid64_from_string (char *ps
                                      _RND_MODE_PARAM _EXC_FLAGS_PARAM
                                      _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C void bid128_to_string (char *str, BID_UINT128 x
                                   _EXC_FLAGS_PARAM
                                   _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT128 bid128_from_string (char *ps
                                        _RND_MODE_PARAM _EXC_FLAGS_PARAM
                                        _EXC_MASKS_PARAM
                                        _EXC_INFO_PARAM);

     BID_EXTERN_C BID_UINT32 bid32_quantize (BID_UINT32 x, BID_UINT32 y
                                   _RND_MODE_PARAM _EXC_FLAGS_PARAM
                                   _EXC_MASKS_PARAM _EXC_INFO_PARAM);

     BID_EXTERN_C BID_UINT64 bid64_quantize (BID_UINT64 x, BID_UINT64 y
                                   _RND_MODE_PARAM _EXC_FLAGS_PARAM
                                   _EXC_MASKS_PARAM _EXC_INFO_PARAM);

     BID_EXTERN_C BID_UINT128 bid128_quantize (BID_UINT128 x, BID_UINT128 y
                                     _RND_MODE_PARAM
                                     _EXC_FLAGS_PARAM
                                     _EXC_MASKS_PARAM _EXC_INFO_PARAM);


     BID_EXTERN_C BID_UINT32 binary128_to_bid32 (BINARY128 x
                                       _RND_MODE_PARAM
                                       _EXC_FLAGS_PARAM
                                       _EXC_MASKS_PARAM
                                       _EXC_INFO_PARAM);

     BID_EXTERN_C BID_UINT64 binary128_to_bid64 (BINARY128 x
                                       _RND_MODE_PARAM
                                       _EXC_FLAGS_PARAM
                                       _EXC_MASKS_PARAM
                                       _EXC_INFO_PARAM);

     BID_EXTERN_C BID_UINT128 binary128_to_bid128 (BINARY128 x
                                         _RND_MODE_PARAM
                                         _EXC_FLAGS_PARAM
                                         _EXC_MASKS_PARAM
                                         _EXC_INFO_PARAM);

     BID_EXTERN_C BID_UINT32 binary64_to_bid32 (double x
                                      _RND_MODE_PARAM
                                      _EXC_FLAGS_PARAM
                                      _EXC_MASKS_PARAM _EXC_INFO_PARAM);

     BID_EXTERN_C BID_UINT64 binary64_to_bid64 (double x
                                      _RND_MODE_PARAM
                                      _EXC_FLAGS_PARAM
                                      _EXC_MASKS_PARAM _EXC_INFO_PARAM);

     BID_EXTERN_C BID_UINT128 binary64_to_bid128 (double x
                                        _RND_MODE_PARAM
                                        _EXC_FLAGS_PARAM
                                        _EXC_MASKS_PARAM
                                        _EXC_INFO_PARAM);

     BID_EXTERN_C BID_UINT32 binary80_to_bid32 (BINARY80 x
                                      _RND_MODE_PARAM
                                      _EXC_FLAGS_PARAM
                                      _EXC_MASKS_PARAM _EXC_INFO_PARAM);

     BID_EXTERN_C BID_UINT64 binary80_to_bid64 (BINARY80 x
                                      _RND_MODE_PARAM
                                      _EXC_FLAGS_PARAM
                                      _EXC_MASKS_PARAM _EXC_INFO_PARAM);

     BID_EXTERN_C BID_UINT128 binary80_to_bid128 (BINARY80 x
                                        _RND_MODE_PARAM
                                        _EXC_FLAGS_PARAM
                                        _EXC_MASKS_PARAM
                                        _EXC_INFO_PARAM);

     BID_EXTERN_C BID_UINT32 binary32_to_bid32 (float x
                                      _RND_MODE_PARAM
                                      _EXC_FLAGS_PARAM
                                      _EXC_MASKS_PARAM _EXC_INFO_PARAM);

     BID_EXTERN_C BID_UINT64 binary32_to_bid64 (float x
                                      _RND_MODE_PARAM
                                      _EXC_FLAGS_PARAM
                                      _EXC_MASKS_PARAM _EXC_INFO_PARAM);

     BID_EXTERN_C BID_UINT128 binary32_to_bid128 (float x
                                        _RND_MODE_PARAM
                                        _EXC_FLAGS_PARAM
                                        _EXC_MASKS_PARAM
                                        _EXC_INFO_PARAM);

     BID_EXTERN_C float bid128_to_binary32 (BID_UINT128 x
                                      _RND_MODE_PARAM _EXC_FLAGS_PARAM
                                      _EXC_MASKS_PARAM _EXC_INFO_PARAM);

     BID_EXTERN_C double bid128_to_binary64 (BID_UINT128 x
                                       _RND_MODE_PARAM _EXC_FLAGS_PARAM
                                       _EXC_MASKS_PARAM
                                       _EXC_INFO_PARAM);

     BID_EXTERN_C BINARY80 bid128_to_binary80 (BID_UINT128 x
                                         _RND_MODE_PARAM
                                         _EXC_FLAGS_PARAM
                                         _EXC_MASKS_PARAM
                                         _EXC_INFO_PARAM);

     BID_EXTERN_C BINARY128 bid128_to_binary128 (BID_UINT128 x
                                           _RND_MODE_PARAM
                                           _EXC_FLAGS_PARAM
                                           _EXC_MASKS_PARAM
                                           _EXC_INFO_PARAM);

     BID_EXTERN_C float bid64_to_binary32 (BID_UINT64 x
                                     _RND_MODE_PARAM _EXC_FLAGS_PARAM
                                     _EXC_MASKS_PARAM _EXC_INFO_PARAM);

     BID_EXTERN_C double bid64_to_binary64 (BID_UINT64 x
                                      _RND_MODE_PARAM _EXC_FLAGS_PARAM
                                      _EXC_MASKS_PARAM _EXC_INFO_PARAM);

     BID_EXTERN_C BINARY80 bid64_to_binary80 (BID_UINT64 x
                                        _RND_MODE_PARAM
                                        _EXC_FLAGS_PARAM
                                        _EXC_MASKS_PARAM
                                        _EXC_INFO_PARAM);

     BID_EXTERN_C BINARY128 bid64_to_binary128 (BID_UINT64 x
                                          _RND_MODE_PARAM
                                          _EXC_FLAGS_PARAM
                                          _EXC_MASKS_PARAM
                                          _EXC_INFO_PARAM);

     BID_EXTERN_C float bid32_to_binary32 (BID_UINT32 x
                                     _RND_MODE_PARAM _EXC_FLAGS_PARAM
                                     _EXC_MASKS_PARAM _EXC_INFO_PARAM);

     BID_EXTERN_C double bid32_to_binary64 (BID_UINT32 x
                                      _RND_MODE_PARAM _EXC_FLAGS_PARAM
                                      _EXC_MASKS_PARAM _EXC_INFO_PARAM);

     BID_EXTERN_C BINARY80 bid32_to_binary80 (BID_UINT32 x
                                        _RND_MODE_PARAM
                                        _EXC_FLAGS_PARAM
                                        _EXC_MASKS_PARAM
                                        _EXC_INFO_PARAM);

     BID_EXTERN_C BINARY128 bid32_to_binary128 (BID_UINT32 x
                                          _RND_MODE_PARAM
                                          _EXC_FLAGS_PARAM
                                          _EXC_MASKS_PARAM
                                          _EXC_INFO_PARAM);

     BID_EXTERN_C int bid_is754 (void);

     BID_EXTERN_C int bid_is754R (void);

     BID_EXTERN_C void bid_signalException (_IDEC_flags flagsmask
                                  _EXC_FLAGS_PARAM);

     BID_EXTERN_C void bid_lowerFlags (_IDEC_flags flagsmask _EXC_FLAGS_PARAM);

     BID_EXTERN_C _IDEC_flags bid_testFlags (_IDEC_flags flagsmask
                                   _EXC_FLAGS_PARAM);

     BID_EXTERN_C _IDEC_flags bid_testSavedFlags (_IDEC_flags savedflags,
                                        _IDEC_flags flagsmask);

     BID_EXTERN_C void bid_restoreFlags (_IDEC_flags flagsvalues,
                               _IDEC_flags flagsmask _EXC_FLAGS_PARAM);

     BID_EXTERN_C _IDEC_flags bid_saveFlags (_IDEC_flags flagsmask
                                   _EXC_FLAGS_PARAM);

#if !DECIMAL_GLOBAL_ROUNDING
     BID_EXTERN_C _IDEC_round bid_getDecimalRoundingDirection (_IDEC_round rnd_mode);
#else
     BID_EXTERN_C _IDEC_round bid_getDecimalRoundingDirection (void);
#endif

#if !DECIMAL_GLOBAL_ROUNDING
     BID_EXTERN_C _IDEC_round bid_setDecimalRoundingDirection (_IDEC_round
                                              rounding_mode
                                              _RND_MODE_PARAM);
#else
     BID_EXTERN_C void bid_setDecimalRoundingDirection (_IDEC_round rounding_mode);
#endif

     BID_EXTERN_C BID_UINT32 bid32_add (BID_UINT32 x, BID_UINT32 y
                              _RND_MODE_PARAM _EXC_FLAGS_PARAM
                              _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT32 bid32_sub (BID_UINT32 x, BID_UINT32 y
                              _RND_MODE_PARAM _EXC_FLAGS_PARAM
                              _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT32 bid32_mul (BID_UINT32 x, BID_UINT32 y
                              _RND_MODE_PARAM _EXC_FLAGS_PARAM
                              _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT32 bid32_div (BID_UINT32 x, BID_UINT32 y
                              _RND_MODE_PARAM _EXC_FLAGS_PARAM
                              _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT32 bid32_fma (BID_UINT32 x, BID_UINT32 y, BID_UINT32 z
                              _RND_MODE_PARAM _EXC_FLAGS_PARAM
                              _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT32 bid32_sqrt (BID_UINT32 x
                              _RND_MODE_PARAM _EXC_FLAGS_PARAM
                              _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT32 bid32_cbrt (BID_UINT32 x
                              _RND_MODE_PARAM _EXC_FLAGS_PARAM
                              _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C int bid32_quiet_greater (BID_UINT32 x,
                                     BID_UINT32 y _EXC_FLAGS_PARAM
                                     _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C int bid32_quiet_greater_equal (BID_UINT32 x,
                                           BID_UINT32 y _EXC_FLAGS_PARAM
                                           _EXC_MASKS_PARAM
                                           _EXC_INFO_PARAM);
     BID_EXTERN_C int bid32_quiet_greater_unordered (BID_UINT32 x,
                                               BID_UINT32 y _EXC_FLAGS_PARAM
                                               _EXC_MASKS_PARAM
                                               _EXC_INFO_PARAM);
     BID_EXTERN_C int bid32_quiet_less (BID_UINT32 x,
                                  BID_UINT32 y _EXC_FLAGS_PARAM
                                  _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C int bid32_quiet_less_equal (BID_UINT32 x,
                                        BID_UINT32 y _EXC_FLAGS_PARAM
                                        _EXC_MASKS_PARAM
                                        _EXC_INFO_PARAM);
     BID_EXTERN_C int bid32_quiet_less_unordered (BID_UINT32 x,
                                            BID_UINT32 y _EXC_FLAGS_PARAM
                                            _EXC_MASKS_PARAM
                                            _EXC_INFO_PARAM);
     BID_EXTERN_C int bid32_quiet_not_equal (BID_UINT32 x,
                                       BID_UINT32 y _EXC_FLAGS_PARAM
                                       _EXC_MASKS_PARAM
                                       _EXC_INFO_PARAM);
     BID_EXTERN_C int bid32_quiet_not_greater (BID_UINT32 x,
                                         BID_UINT32 y _EXC_FLAGS_PARAM
                                         _EXC_MASKS_PARAM
                                         _EXC_INFO_PARAM);
     BID_EXTERN_C int bid32_quiet_not_less (BID_UINT32 x,
                                      BID_UINT32 y _EXC_FLAGS_PARAM
                                      _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C int bid32_quiet_ordered (BID_UINT32 x,
                                     BID_UINT32 y _EXC_FLAGS_PARAM
                                     _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C int bid32_quiet_unordered (BID_UINT32 x,
                                       BID_UINT32 y _EXC_FLAGS_PARAM
                                       _EXC_MASKS_PARAM
                                       _EXC_INFO_PARAM);
     BID_EXTERN_C int bid32_signaling_greater (BID_UINT32 x,
                                         BID_UINT32 y _EXC_FLAGS_PARAM
                                         _EXC_MASKS_PARAM
                                         _EXC_INFO_PARAM);
     BID_EXTERN_C int bid32_signaling_greater_equal (BID_UINT32 x,
                                               BID_UINT32 y _EXC_FLAGS_PARAM
                                               _EXC_MASKS_PARAM
                                               _EXC_INFO_PARAM);
     BID_EXTERN_C int bid32_signaling_greater_unordered (BID_UINT32 x,
                                                   BID_UINT32 y
                                                   _EXC_FLAGS_PARAM
                                                   _EXC_MASKS_PARAM
                                                   _EXC_INFO_PARAM);
     BID_EXTERN_C int bid32_signaling_less (BID_UINT32 x,
                                      BID_UINT32 y _EXC_FLAGS_PARAM
                                      _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C int bid32_signaling_less_equal (BID_UINT32 x,
                                            BID_UINT32 y _EXC_FLAGS_PARAM
                                            _EXC_MASKS_PARAM
                                            _EXC_INFO_PARAM);
     BID_EXTERN_C int bid32_signaling_less_unordered (BID_UINT32 x,
                                                BID_UINT32 y
                                                _EXC_FLAGS_PARAM
                                                _EXC_MASKS_PARAM
                                                _EXC_INFO_PARAM);
     BID_EXTERN_C int bid32_signaling_not_greater (BID_UINT32 x,
                                             BID_UINT32 y _EXC_FLAGS_PARAM
                                             _EXC_MASKS_PARAM
                                             _EXC_INFO_PARAM);
     BID_EXTERN_C int bid32_signaling_not_less (BID_UINT32 x,
                                          BID_UINT32 y _EXC_FLAGS_PARAM
                                          _EXC_MASKS_PARAM
                                          _EXC_INFO_PARAM);

     BID_EXTERN_C BID_UINT32 bid32_frexp (BID_UINT32 x, int *exp);
     BID_EXTERN_C BID_UINT64 bid64_frexp (BID_UINT64 x, int *exp);
     BID_EXTERN_C BID_UINT128 bid128_frexp (BID_UINT128 x, int *exp);
     BID_EXTERN_C BID_UINT32 bid32_logb (BID_UINT32 x
                _EXC_FLAGS_PARAM _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT64 bid64_logb (BID_UINT64 x
                _EXC_FLAGS_PARAM _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT128 bid128_logb (BID_UINT128 x
                _EXC_FLAGS_PARAM _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT32 bid32_scalbln (BID_UINT32 x, long int n
        _RND_MODE_PARAM _EXC_FLAGS_PARAM _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT64 bid64_scalbln (BID_UINT64 x, long int n
        _RND_MODE_PARAM _EXC_FLAGS_PARAM _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT128 bid128_scalbln (BID_UINT128 x, long int n
        _RND_MODE_PARAM _EXC_FLAGS_PARAM _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT32 bid32_nearbyint (BID_UINT32 x
        _RND_MODE_PARAM _EXC_FLAGS_PARAM _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT64 bid64_nearbyint (BID_UINT64 x
        _RND_MODE_PARAM _EXC_FLAGS_PARAM _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT128 bid128_nearbyint (BID_UINT128 x
        _RND_MODE_PARAM _EXC_FLAGS_PARAM _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C long int bid32_lrint (BID_UINT32 x
        _RND_MODE_PARAM _EXC_FLAGS_PARAM _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C long int bid64_lrint (BID_UINT64 x
        _RND_MODE_PARAM _EXC_FLAGS_PARAM _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C long int bid128_lrint (BID_UINT128 x
        _RND_MODE_PARAM _EXC_FLAGS_PARAM _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C long long int bid32_llrint (BID_UINT32 x
        _RND_MODE_PARAM _EXC_FLAGS_PARAM _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C long long int bid64_llrint (BID_UINT64 x
        _RND_MODE_PARAM _EXC_FLAGS_PARAM _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C long long int bid128_llrint (BID_UINT128 x
        _RND_MODE_PARAM _EXC_FLAGS_PARAM _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C long int bid32_lround (BID_UINT32 x
        _EXC_FLAGS_PARAM _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C long int bid64_lround (BID_UINT64 x
        _EXC_FLAGS_PARAM _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C long int bid128_lround (BID_UINT128 x
        _EXC_FLAGS_PARAM _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C long long int bid32_llround (BID_UINT32 x
        _EXC_FLAGS_PARAM _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C long long int bid64_llround (BID_UINT64 x
        _EXC_FLAGS_PARAM _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C long long int bid128_llround (BID_UINT128 x
        _EXC_FLAGS_PARAM _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT32 bid32_nan (const char *tagp);
     BID_EXTERN_C BID_UINT64 bid64_nan (const char *tagp);
     BID_EXTERN_C BID_UINT128 bid128_nan (const char *tagp);
     BID_EXTERN_C BID_UINT32 bid32_nexttoward (BID_UINT32 x, BID_UINT128 y
         _EXC_FLAGS_PARAM _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT64 bid64_nexttoward (BID_UINT64 x, BID_UINT128 y
         _EXC_FLAGS_PARAM _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT128 bid128_nexttoward (BID_UINT128 x, BID_UINT128 y
         _EXC_FLAGS_PARAM _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT32 bid32_fdim (BID_UINT32 x, BID_UINT32 y
         _RND_MODE_PARAM _EXC_FLAGS_PARAM _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT64 bid64_fdim (BID_UINT64 x, BID_UINT64 y
         _RND_MODE_PARAM _EXC_FLAGS_PARAM _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C BID_UINT128 bid128_fdim (BID_UINT128 x, BID_UINT128 y
         _RND_MODE_PARAM _EXC_FLAGS_PARAM _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C int bid32_quantexp (BID_UINT32 x
         _EXC_FLAGS_PARAM _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C int bid64_quantexp (BID_UINT64 x
         _EXC_FLAGS_PARAM _EXC_MASKS_PARAM _EXC_INFO_PARAM);
     BID_EXTERN_C int bid128_quantexp (BID_UINT128 x
         _EXC_FLAGS_PARAM _EXC_MASKS_PARAM _EXC_INFO_PARAM);

     BID_EXTERN_C BID_UINT32 bid32_inf (void);
     BID_EXTERN_C BID_UINT64 bid64_inf (void);
     BID_EXTERN_C BID_UINT128 bid128_inf (void);

#endif

// Functions not dependent on different parameters
BID_UINT32  bid_strtod32(const char*  ps_in, char**  endptr);
BID_UINT64  bid_strtod64(const char*  ps_in, char**  endptr);
BID_UINT128 bid_strtod128(const char*  ps_in, char**  endptr);
BID_UINT32  bid_wcstod32(const wchar_t*  ps_in, wchar_t**  endptr);
BID_UINT64  bid_wcstod64(const wchar_t*  ps_in, wchar_t**  endptr);
BID_UINT128 bid_wcstod128(const wchar_t*  ps_in, wchar_t**  endptr);
void bid_feclearexcept( int excepts _EXC_FLAGS_PARAM );
void bid_fegetexceptflag( fexcept_t *flagp, int excepts _EXC_FLAGS_PARAM );
void bid_feraiseexcept( int excepts _EXC_FLAGS_PARAM );
void bid_fesetexceptflag( const fexcept_t *flagp, int excepts _EXC_FLAGS_PARAM );
int bid_fetestexcept( int excepts _EXC_FLAGS_PARAM );

// Internal Functions

     BID_EXTERN_C void
       bid_round64_2_18 (int q,
                     int x,
                     BID_UINT64 C,
                     BID_UINT64 * ptr_Cstar,
                     int *delta_exp,
                     int *ptr_is_midpoint_lt_even,
                     int *ptr_is_midpoint_gt_even,
                     int *ptr_is_inexact_lt_midpoint,
                     int *ptr_is_inexact_gt_midpoint);

     BID_EXTERN_C void
       bid_round128_19_38 (int q,
                       int x,
                       BID_UINT128 C,
                       BID_UINT128 * ptr_Cstar,
                       int *delta_exp,
                       int *ptr_is_midpoint_lt_even,
                       int *ptr_is_midpoint_gt_even,
                       int *ptr_is_inexact_lt_midpoint,
                       int *ptr_is_inexact_gt_midpoint);

     BID_EXTERN_C void
       bid_round192_39_57 (int q,
                       int x,
                       BID_UINT192 C,
                       BID_UINT192 * ptr_Cstar,
                       int *delta_exp,
                       int *ptr_is_midpoint_lt_even,
                       int *ptr_is_midpoint_gt_even,
                       int *ptr_is_inexact_lt_midpoint,
                       int *ptr_is_inexact_gt_midpoint);

     BID_EXTERN_C void
       bid_round256_58_76 (int q,
                       int x,
                       BID_UINT256 C,
                       BID_UINT256 * ptr_Cstar,
                       int *delta_exp,
                       int *ptr_is_midpoint_lt_even,
                       int *ptr_is_midpoint_gt_even,
                       int *ptr_is_inexact_lt_midpoint,
                       int *ptr_is_inexact_gt_midpoint);

#endif

