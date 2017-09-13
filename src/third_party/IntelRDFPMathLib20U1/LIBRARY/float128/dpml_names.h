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

#ifndef DPML_NAMES_H
#define DPML_NAMES_H

/*
 * If this file is used without dpml_private.h, then DPML_NULL_MACRO_TOKEN will
 * be undefined.  Consequently, many of the following macros will not be
 * defined as intended.  So check for a definintion, if one doesn't exist,
 * provide one.  The only requirement is that DPML_NULL_MACRO_TOKEN have a
 * non-zero integer value
 */

#if !defined(DPML_NULL_MACRO_TOKEN)
#       define DPML_NULL_MACRO_TOKEN	1
#endif

/*
 * Set up platform specific name that over-ride default names
 */

#if (OP_SYSTEM == vms)

#   define __VMS_POW_NAME(name,suf)	PASTE_2(__F_SYSTEM_NAME(name),suf)
#   define __VMS_B_POW_NAME(name,suf)	PASTE_2(__B_SYSTEM_NAME(name),suf)
#   define __VMS_INT_POW_NAME(name)	PASTE_2(__USER_NAME(name),_qq)



#   if !defined(LN_BASE_NAME)
#       define LN_BASE_NAME	ln
#   endif

#   if !defined(REM_BASE_NAME)
#       define REM_BASE_NAME	rem
#   endif

#   if !defined(MOD_BASE_NAME)
#       define MOD_BASE_NAME	mod
#   endif

#   if !defined(POW_E_BASE_NAME)
#       define POW_E_BASE_NAME	pow
#   endif

#   if !defined(POW_BASE_NAME)
#       define POW_BASE_NAME	pow_o
#   endif

#   if !defined(POW_Z_BASE_NAME)
#       define POW_Z_BASE_NAME	pow_z
#   endif

#   if !defined(F_POW_E_NAME)
#       define F_POW_E_NAME	__VMS_POW_NAME(POW_E_BASE_NAME, F_CHAR)
#   endif

#   if !defined(F_POW_NAME)
#       define F_POW_NAME	__VMS_POW_NAME(POW_BASE_NAME, F_CHAR)
#   endif

#   if !defined(F_POW_I_NAME)
#       define F_POW_I_NAME	__VMS_POW_NAME(POW_BASE_NAME, q)
#   endif

#   if !defined(F_POW_I_E_NAME)
#       define F_POW_I_E_NAME	__VMS_POW_NAME(POW_E_BASE_NAME, q)
#   endif

#   if !defined(F_POW_I_Z_NAME)
#       define F_POW_I_Z_NAME	__VMS_POW_NAME(POW_Z_BASE_NAME, q)
#   endif

#   if !defined(F_POW_I_II_NAME)
#       define F_POW_I_II_NAME	__VMS_INT_POW_NAME(POW_BASE_NAME)
#   endif

#   if !defined(F_POW_E_I_II_NAME)
#       define F_POW_E_I_II_NAME	__VMS_INT_POW_NAME(POW_E_BASE_NAME)
#   endif

#   if !defined(F_CPOWI_NAME)
#       define F_CPOWI_NAME	__VMS_POW_NAME(CPOW_BASE_NAME, q)
#   endif

#   if !defined(F_FAST_POW_NAME)
#       define F_FAST_POW_NAME  __VMS_POW_NAME(FAST_POW_BASE_NAME, F_CHAR)
#   endif

#   if !defined(F_FAST_POW_E_NAME)
#       define F_FAST_POW_E_NAME  __VMS_POW_NAME(FAST_POW_E_BASE_NAME, F_CHAR)
#   endif

#   if !defined(B_POW_NAME)
#       define B_POW_NAME	__VMS_B_POW_NAME(POW_BASE_NAME, B_CHAR)
#   endif

#   if !defined(B_POW_E_NAME)
#       define B_POW_E_NAME	__VMS_B_POW_NAME(POW_E_BASE_NAME, B_CHAR)
#   endif

#   if !defined(B_POW_I_NAME)
#       define B_POW_I_NAME	__VMS_B_POW_NAME(POW_BASE_NAME, q)
#   endif

#   if !defined(B_POW_I_E_NAME)
#       define B_POW_I_E_NAME	__VMS_B_POW_NAME(POW_E_BASE_NAME, q)
#   endif

#   if !defined(B_POW_I_Z_NAME)
#       define B_POW_I_Z_NAME	__VMS_B_POW_NAME(POW_Z_BASE_NAME, q)
#   endif

#   if !defined(B_CPOWI_NAME)
#       define B_CPOWI_NAME	__VMS_B_POW_NAME(CPOW_BASE_NAME, q)
#   endif

#   if !defined(B_FAST_POW_NAME)
#       define B_FAST_POW_NAME	__VMS_B_POW_NAME(FAST_POW_BASE_NAME, B_CHAR)
#   endif

#   if !defined(B_FAST_POW_E_NAME)
#       define B_FAST_POW_E_NAME  __VMS_B_POW_NAME(FAST_POW_E_BASE_NAME, B_CHAR)
#   endif

#   define __CVT_NAME(type) __INTERNAL_NAME(PASTE_3(cvt_,type,IEEE_VAX_SUFFIX))

#   if !defined(F_CVT_IEEE_TO_VAX_NAME)
#       define F_CVT_FLOAT_IEEE_TO_VAX_NAME	__CVT_NAME(float)
#   endif

#   if !defined(F_CVT_CMPLX_IEEE_TO_VAX_NAME)
#       define F_CVT_CMPLX_IEEE_TO_VAX_NAME	__CVT_NAME(complex)
#   endif

#   if !defined(F_NAME_PREFIX)
#       define F_NAME_PREFIX	math$
#   endif

#   if !defined(F_CVTAS_NAME_PREFIX)
#       define F_CVTAS_NAME_PREFIX	cvtas$
#   endif

#   if !defined(F_CVTAS_SUFFIX)
#       define F_CVTAS_SUFFIX	__F_SUFFIX
#   endif

#   if !defined(F_NAME_SUFFIX)
#       define F_NAME_SUFFIX	__F_SUFFIX
#   endif

#   if !defined(B_NAME_SUFFIX)
#       define B_NAME_SUFFIX	__B_SUFFIX
#   endif

#   if !defined(INTERNAL_PREFIX)
#       define INTERNAL_PREFIX	math$
#   endif

#else

#   if !defined(USER_PREFIX)
#       define USER_PREFIX	__
#   endif

#   if !defined(INTERNAL_PREFIX)
#       define INTERNAL_PREFIX	__dpml_
#   endif

#   if !defined(F_CVTAS_NAME_PREFIX)
#       if ((OP_SYSTEM == wnt) || (OP_SYSTEM == win64))
#           define F_CVTAS_NAME_PREFIX	cvtas_
#       else
#           define F_CVTAS_NAME_PREFIX	__cvtas_
#       endif

#   endif

#endif

/*
**  Default definitions for the "base" name of each funcion.
*/


#ifndef ASIND_BASE_NAME
#    define ASIND_BASE_NAME  asind
#endif

#ifndef ASINH_BASE_NAME
#    define ASINH_BASE_NAME  asinh
#endif

#ifndef ACOSD_BASE_NAME
#    define ACOSD_BASE_NAME  acosd
#endif

#ifndef ACOSH_BASE_NAME
#    define ACOSH_BASE_NAME  acosh
#endif

#ifndef ASIN_BASE_NAME
#    define ASIN_BASE_NAME  asin
#endif

#ifndef ACOS_BASE_NAME
#    define ACOS_BASE_NAME  acos
#endif

#ifndef ATAND_BASE_NAME
#    define ATAND_BASE_NAME  atand
#endif

#ifndef ATAND2_BASE_NAME
#    define ATAND2_BASE_NAME  atand2
#endif

#ifndef ATAN2_BASE_NAME
#    define ATAN2_BASE_NAME  atan2
#endif

#ifndef ATAN_BASE_NAME
#    define ATAN_BASE_NAME  atan
#endif

#ifndef ATANH_BASE_NAME
#    define ATANH_BASE_NAME  atanh
#endif

#ifndef CEIL_BASE_NAME
#    define CEIL_BASE_NAME  ceil
#endif

#ifndef CLASS_BASE_NAME
#   if ((OP_SYSTEM == wnt) || (OP_SYSTEM == win64))
#	define CLASS_BASE_NAME  _fpclass
#   else
#	define CLASS_BASE_NAME  fp_class
#   endif
#endif

#ifndef COPYSIGN_BASE_NAME
#   if ((OP_SYSTEM == wnt) || (OP_SYSTEM == win64))
#	define COPYSIGN_BASE_NAME  _copysign
#   else
#	define COPYSIGN_BASE_NAME  copysign
#   endif
#endif

#ifndef ERF_BASE_NAME
#    define ERF_BASE_NAME  erf
#endif

#ifndef ERFC_BASE_NAME
#    define ERFC_BASE_NAME  erfc
#endif

#ifndef ERFCX_BASE_NAME
#    define ERFCX_BASE_NAME  erfcx
#endif

#ifndef EXP_BASE_NAME
#    define EXP_BASE_NAME  exp
#endif

#ifndef EXP2_BASE_NAME
#    define EXP2_BASE_NAME  exp2
#endif

#ifndef EXP10_BASE_NAME
#    define EXP10_BASE_NAME  exp10
#endif

#ifndef EXPM1_BASE_NAME
#    define EXPM1_BASE_NAME  expm1
#endif

#ifndef FABS_BASE_NAME
#    define FABS_BASE_NAME  fabs
#endif

#ifndef FINITE_BASE_NAME
#   if ((OP_SYSTEM == wnt) || (OP_SYSTEM == win64))
#	define FINITE_BASE_NAME  _finite
#   else
#	define FINITE_BASE_NAME  finite
#   endif
#endif

#ifndef FLOOR_BASE_NAME
#    define FLOOR_BASE_NAME  floor
#endif

#ifndef FREXP_BASE_NAME
#    define FREXP_BASE_NAME  frexp
#endif

#ifndef HYPOT_BASE_NAME
#   if ((OP_SYSTEM == wnt) || (OP_SYSTEM == win64))
#	define HYPOT_BASE_NAME  _hypot
#   else
#	define HYPOT_BASE_NAME  hypot
#   endif
#endif

#ifndef NT_CABS_BASE_NAME
#   define NT_CABS_BASE_NAME _cabs
#endif

#ifndef CABS_BASE_NAME
#   define CABS_BASE_NAME cabs
#endif

#ifndef ISNAN_BASE_NAME
#   if ((OP_SYSTEM == wnt) || (OP_SYSTEM == win64))
#	define ISNAN_BASE_NAME  _isnan
#   else
#	define ISNAN_BASE_NAME  isnan
#   endif
#endif

#ifndef LDEXP_BASE_NAME
#    define LDEXP_BASE_NAME  ldexp
#endif

#ifndef SCALB_BASE_NAME
#   if ((OP_SYSTEM == wnt) || (OP_SYSTEM == win64))
#	define SCALB_BASE_NAME  _scalb
#   else
#	define SCALB_BASE_NAME  scalb
#   endif
#endif

#ifndef SCALBN_BASE_NAME
#    define SCALBN_BASE_NAME  scalbn
#endif

#ifndef SCALBLN_BASE_NAME
#    define SCALBLN_BASE_NAME  scalbln
#endif

#ifndef J0_BASE_NAME
#   if ((OP_SYSTEM == wnt) || (OP_SYSTEM == win64))
#	define J0_BASE_NAME _j0
#   else
#	define J0_BASE_NAME j0
#   endif
#endif

#ifndef J1_BASE_NAME
#   if ((OP_SYSTEM == wnt) || (OP_SYSTEM == win64))
#	define J1_BASE_NAME  _j1
#   else
#	define J1_BASE_NAME j1
#   endif
#endif

#ifndef JN_BASE_NAME
#   if ((OP_SYSTEM == wnt) || (OP_SYSTEM == win64))
#	define JN_BASE_NAME  _jn
#   else
#	define JN_BASE_NAME jn
#   endif
#endif

#ifndef Y0_BASE_NAME
#   if ((OP_SYSTEM == wnt) || (OP_SYSTEM == win64))
#	define Y0_BASE_NAME  _y0
#   else
#	define Y0_BASE_NAME  y0
#   endif
#endif

#ifndef Y1_BASE_NAME
#   if ((OP_SYSTEM == wnt) || (OP_SYSTEM == win64))
#	define Y1_BASE_NAME  _y1
#   else
#	define Y1_BASE_NAME  y1
#   endif
#endif

#ifndef YN_BASE_NAME
#   if ((OP_SYSTEM == wnt) || (OP_SYSTEM ==win64))
#	define YN_BASE_NAME  _yn
#   else
#	define YN_BASE_NAME  yn
#   endif
#endif

#ifndef GAMMA_BASE_NAME
#    define GAMMA_BASE_NAME  gamma
#endif

#ifndef LGAMMA_BASE_NAME
#    define LGAMMA_BASE_NAME  lgamma
#endif

#ifndef TGAMMA_BASE_NAME
#    define TGAMMA_BASE_NAME  tgamma
#endif

#ifndef RT_LGAMMA_BASE_NAME
#    define RT_LGAMMA_BASE_NAME  __lgamma
#endif

#ifndef LOG1P_BASE_NAME
#    define LOG1P_BASE_NAME  log1p
#endif

#ifndef LOG2_BASE_NAME
#    define LOG2_BASE_NAME  log2
#endif

#ifndef LOG10_BASE_NAME
#    define LOG10_BASE_NAME  log10
#endif

#ifndef LN_BASE_NAME
#   define LN_BASE_NAME  log
#endif

#ifndef CMP_BASE_NAME
#   define CMP_BASE_NAME cmp 
#endif

#ifndef LOG_TABLE_BASE_NAME
#   define LOG_TABLE_BASE_NAME log
#endif

#ifndef LOG2_TABLE_BASE_NAME
#   define LOG2_TABLE_BASE_NAME log2
#endif

#ifndef LOG10_TABLE_BASE_NAME
#   define LOG10_TABLE_BASE_NAME log10
#endif

#ifndef F_LOG_TABLE_NAME
#    define F_LOG_TABLE_NAME  __D_TABLE_NAME(LOG_TABLE_BASE_NAME)
#endif

#ifndef F_LOG2_TABLE_NAME
#    define F_LOG2_TABLE_NAME  __D_TABLE_NAME(LOG2_TABLE_BASE_NAME)
#endif

#ifndef F_LOG10_TABLE_NAME
#    define F_LOG10_TABLE_NAME  __D_TABLE_NAME(LOG10_TABLE_BASE_NAME)
#endif

#ifndef F_LOG_BUILD_FILE_NAME
#    define F_LOG_BUILD_FILE_NAME  __D_TABLE_FILE_NAME(LOG_TABLE_BASE_NAME)
#endif

#ifndef F_LOG2_BUILD_FILE_NAME
#    define F_LOG2_BUILD_FILE_NAME  __D_TABLE_FILE_NAME(LOG2_TABLE_BASE_NAME)
#endif

#ifndef F_LOG10_BUILD_FILE_NAME
#    define F_LOG10_BUILD_FILE_NAME  __D_TABLE_FILE_NAME(LOG10_TABLE_BASE_NAME)
#endif

#ifndef LOGB_BASE_NAME
#   if ((OP_SYSTEM == wnt) || (OP_SYSTEM == win64))
#	define LOGB_BASE_NAME  _logb
#   else
#	define LOGB_BASE_NAME  logb
#   endif
#endif

#ifndef ILOGB_BASE_NAME
#   define ILOGB_BASE_NAME  ilogb
#endif

#ifndef REM_BASE_NAME
#   define REM_BASE_NAME  drem
#endif

#ifndef REMAINDER_BASE_NAME
#   define REMAINDER_BASE_NAME  remainder
#endif

#ifndef REMQUO_BASE_NAME
#    define REMQUO_BASE_NAME  remquo
#endif

#ifndef MOD_BASE_NAME
#   define MOD_BASE_NAME  fmod
#endif

#ifndef MODF_BASE_NAME
#    define MODF_BASE_NAME  modf
#endif

#ifndef NINT_BASE_NAME
#    define NINT_BASE_NAME  nint
#endif

#ifndef NEXTAFTER_BASE_NAME
#   if ((OP_SYSTEM == wnt) || (OP_SYSTEM == win64))
#	define NEXTAFTER_BASE_NAME  _nextafter
#   else
#	define NEXTAFTER_BASE_NAME  nextafter
#   endif
#endif

#ifndef NEXTTOWARD_BASE_NAME
#    define NEXTTOWARD_BASE_NAME  nexttoward
#endif

#ifndef NXTAFTR_BASE_NAME
#    define NXTAFTR_BASE_NAME  nxtaftr
#endif

#ifndef POW_BASE_NAME
#    define POW_BASE_NAME  pow
#endif

#ifndef POW_E_BASE_NAME
#   define POW_E_BASE_NAME  pow_e
#endif

#ifndef POW_TABLE_BASE_NAME
#   define POW_TABLE_BASE_NAME	pow
#endif

#ifndef RANDOM_BASE_NAME
#    define RANDOM_BASE_NAME  random
#endif

#ifndef RINT_BASE_NAME
#    define RINT_BASE_NAME  rint
#endif

#ifndef LRINT_BASE_NAME
#    define LRINT_BASE_NAME  lrint
#endif

#ifndef LROUND_BASE_NAME
#    define LROUND_BASE_NAME  lround
#endif

#ifndef LLRINT_BASE_NAME
#    define LLRINT_BASE_NAME  llrint
#endif

#ifndef LLROUND_BASE_NAME
#    define LLROUND_BASE_NAME  llround
#endif

#ifndef FMAX_BASE_NAME
#    define FMAX_BASE_NAME  fmax 
#endif

#ifndef FMIN_BASE_NAME
#    define FMIN_BASE_NAME  fmin
#endif

#ifndef VMS_RANDOM_BASE_NAME
#    define VMS_RANDOM_BASE_NAME  random_L
#endif

#ifndef SIN_BASE_NAME
#    define SIN_BASE_NAME  sin
#endif

#ifndef SIN_VO_BASE_NAME
#   define SIN_VO_BASE_NAME  sin_vo
#endif

#ifndef COS_BASE_NAME
#    define COS_BASE_NAME  cos
#endif

#ifndef COS_VO_BASE_NAME
#   define COS_VO_BASE_NAME  cos_vo
#endif

#ifndef SINCOS_BASE_NAME
#    define SINCOS_BASE_NAME  sincos
#endif

#ifndef SINCOS_VO_BASE_NAME
#   define SINCOS_VO_BASE_NAME  sincos_vo
#endif

#ifndef SIND_BASE_NAME
#    define SIND_BASE_NAME  sind
#endif

#ifndef COSD_BASE_NAME
#    define COSD_BASE_NAME  cosd
#endif

#ifndef SINCOSD_BASE_NAME
#    define SINCOSD_BASE_NAME  sincosd
#endif

#ifndef SINH_BASE_NAME
#    define SINH_BASE_NAME  sinh
#endif

#ifndef COSH_BASE_NAME
#    define COSH_BASE_NAME  cosh
#endif

#ifndef SQRT_BASE_NAME
#    define SQRT_BASE_NAME  sqrt
#endif

#ifndef RSQRT_BASE_NAME
#    define RSQRT_BASE_NAME  rsqrt
#endif

#ifndef CBRT_BASE_NAME
#    define CBRT_BASE_NAME  cbrt
#endif

#ifndef TAN_BASE_NAME
#    define TAN_BASE_NAME  tan
#endif

#ifndef COT_BASE_NAME
#    define COT_BASE_NAME  cot
#endif

#ifndef TANCOT_BASE_NAME
#    define TANCOT_BASE_NAME  tancot
#endif

#ifndef TAND_BASE_NAME
#    define TAND_BASE_NAME  tand
#endif

#ifndef COTD_BASE_NAME
#    define COTD_BASE_NAME  cotd
#endif

#ifndef TANCOTD_BASE_NAME
#    define TANCOTD_BASE_NAME  tancotd
#endif

#ifndef TANH_BASE_NAME
#    define TANH_BASE_NAME  tanh
#endif

#ifndef TRIG_CONS_BASE_NAME
#   define TRIG_CONS_BASE_NAME   trig_cons
#endif

#ifndef TRIGD_CONS_BASE_NAME
#   define TRIGD_CONS_BASE_NAME   trigd_cons
#endif

#ifndef TRIG_REDUCE_BASE_NAME
#   define TRIG_REDUCE_BASE_NAME   trig_reduce
#endif

#ifndef TRIG_REDUCE_VO_BASE_NAME
#   define TRIG_REDUCE_VO_BASE_NAME   trig_reduce_vo
#endif

#ifndef TRIGD_REDUCE_BASE_NAME
#   define TRIGD_REDUCE_BASE_NAME   trigd_reduce
#endif

#ifndef TRUNC_BASE_NAME
#    define TRUNC_BASE_NAME   trunc
#endif

#ifndef UNORDERED_BASE_NAME
#    define UNORDERED_BASE_NAME   unordered
#endif

#ifndef CCOS_BASE_NAME
#    define CCOS_BASE_NAME  ccos
#endif

#ifndef CDIV_BASE_NAME
#    define CDIV_BASE_NAME  cdiv
#endif

#ifndef CEXP_BASE_NAME
#    define CEXP_BASE_NAME  cexp
#endif

#ifndef CLOG_BASE_NAME
#    define CLOG_BASE_NAME  clog
#endif

#ifndef CMUL_BASE_NAME
#    define CMUL_BASE_NAME  cmul
#endif

#ifndef CPOW_BASE_NAME
#    define CPOW_BASE_NAME  cpow
#endif

#ifndef CPOWI_BASE_NAME
#    define CPOWI_BASE_NAME  cpowi
#endif

#ifndef CSIN_BASE_NAME
#    define CSIN_BASE_NAME  csin
#endif

#ifndef CSQRT_BASE_NAME
#    define CSQRT_BASE_NAME  csqrt
#endif

#ifndef CACOS_BASE_NAME
#    define CACOS_BASE_NAME  cacos
#endif

#ifndef CASIN_BASE_NAME
#    define CASIN_BASE_NAME  casin
#endif

#ifndef CATAN_BASE_NAME
#    define CATAN_BASE_NAME  catan
#endif

#ifndef CTAN_BASE_NAME
#    define CTAN_BASE_NAME  ctan
#endif

#ifndef CCOSH_BASE_NAME
#    define CCOSH_BASE_NAME  ccosh
#endif

#ifndef CSINH_BASE_NAME
#    define CSINH_BASE_NAME  csinh
#endif

#ifndef CTANH_BASE_NAME
#    define CTANH_BASE_NAME  ctanh
#endif

#ifndef CACOSH_BASE_NAME
#    define CACOSH_BASE_NAME  cacosh
#endif

#ifndef CASINH_BASE_NAME
#    define CASINH_BASE_NAME  casinh
#endif

#ifndef CATANH_BASE_NAME
#    define CATANH_BASE_NAME  catanh
#endif

#ifndef CARG_BASE_NAME
#    define CARG_BASE_NAME  carg
#endif

#ifndef CIMAG_BASE_NAME
#    define CIMAG_BASE_NAME  cimag
#endif

#ifndef CREAL_BASE_NAME
#    define CREAL_BASE_NAME  creal
#endif

#ifndef CPROJ_BASE_NAME
#    define CPROJ_BASE_NAME  cproj
#endif

#ifndef CONJ_BASE_NAME
#    define CONJ_BASE_NAME  conj
#endif

#ifndef NAN_BASE_NAME
#    define NAN_BASE_NAME  nan
#endif

#ifndef STRING_TO_NAN_BASE_NAME
#    define STRING_TO_NAN_BASE_NAME  string_to_nan
#endif

#ifndef FDIM_BASE_NAME
#    define FDIM_BASE_NAME  fdim
#endif

#ifndef FMA_BASE_NAME
#    define FMA_BASE_NAME  fma
#endif

#ifndef SIGNIFICAND_BASE_NAME
#    define SIGNIFICAND_BASE_NAME  significand
#endif

#ifndef POW_I_BASE_NAME
#    define POW_I_BASE_NAME  powi
#endif

#ifndef POW_I_E_BASE_NAME
#    define POW_I_E_BASE_NAME  powi_e
#endif

#ifndef POW_I_Z_BASE_NAME
#    define POW_I_Z_BASE_NAME  powi_z
#endif

#ifndef POW_I_II_BASE_NAME
#    define POW_I_II_BASE_NAME  powii
#endif

#ifndef POW_E_I_II_BASE_NAME
#    define POW_E_I_II_BASE_NAME  powii_e
#endif

#ifndef SINHCOSH_BASE_NAME
#    define SINHCOSH_BASE_NAME  sinhcosh
#endif

#ifndef MUL_BASE_NAME
#   define MUL_BASE_NAME  mul
#endif

#ifndef DIV_BASE_NAME
#   define DIV_BASE_NAME  div
#endif

#ifndef ADD_BASE_NAME
#   define ADD_BASE_NAME  add
#endif

#ifndef SUB_BASE_NAME
#   define SUB_BASE_NAME  sub
#endif

#ifndef NEG_BASE_NAME
#   define NEG_BASE_NAME  neg
#endif

#ifndef ITOF_BASE_NAME
#   define ITOF_BASE_NAME  itof
#endif

#ifndef CMP_BASE_NAME
#   define CMP_BASE_NAME cmp
#endif
/*
**  Default base names for the fast routines.
*/

#ifndef FAST_ACOS_BASE_NAME
#    define FAST_ACOS_BASE_NAME  __FAST_NAME(ACOS_BASE_NAME)
#endif

#ifndef FAST_ASIN_BASE_NAME
#    define FAST_ASIN_BASE_NAME  __FAST_NAME(ASIN_BASE_NAME)
#endif

#ifndef FAST_ATAN_BASE_NAME
#    define FAST_ATAN_BASE_NAME  __FAST_NAME(ATAN_BASE_NAME)
#endif

#ifndef FAST_EXP_BASE_NAME
#    define FAST_EXP_BASE_NAME  __FAST_NAME(EXP_BASE_NAME)
#endif

#ifndef FAST_LN_BASE_NAME
#   define FAST_LN_BASE_NAME  __FAST_NAME(LN_BASE_NAME)
#endif

#ifndef FAST_LOG10_BASE_NAME
#    define FAST_LOG10_BASE_NAME  __FAST_NAME(LOG10_BASE_NAME)
#endif

#ifndef FAST_SINCOS_BASE_NAME
#    define FAST_SINCOS_BASE_NAME  __FAST_NAME(SINCOS_BASE_NAME)
#endif

#ifndef FAST_SIN_BASE_NAME
#    define FAST_SIN_BASE_NAME  __FAST_NAME(SIN_BASE_NAME)
#endif

#ifndef FAST_COS_BASE_NAME
#    define FAST_COS_BASE_NAME  __FAST_NAME(COS_BASE_NAME)
#endif

#ifndef FAST_SINCOSD_BASE_NAME
#    define FAST_SINCOSD_BASE_NAME  __FAST_NAME(SINCOSD_BASE_NAME)
#endif

#ifndef FAST_SIND_BASE_NAME
#    define FAST_SIND_BASE_NAME  __FAST_NAME(SIND_BASE_NAME)
#endif

#ifndef FAST_COSD_BASE_NAME
#    define FAST_COSD_BASE_NAME  __FAST_NAME(COSD_BASE_NAME)
#endif

#ifndef FAST_TAN_BASE_NAME
#    define FAST_TAN_BASE_NAME  __FAST_NAME(TAN_BASE_NAME)
#endif

#ifndef FAST_ATAN2_BASE_NAME
#    define FAST_ATAN2_BASE_NAME  __FAST_NAME(ATAN2_BASE_NAME)
#endif

#ifndef FAST_HYPOT_BASE_NAME
#   if ((OP_SYSTEM == wnt) || (OP_SYSTEM == win64))
#	define FAST_HYPOT_BASE_NAME  __FAST_NAME(hypot)
#   else
#	define FAST_HYPOT_BASE_NAME  __FAST_NAME(HYPOT_BASE_NAME)
#   endif
#endif

#ifndef FAST_POW_BASE_NAME
#    define FAST_POW_BASE_NAME  __FAST_NAME(POW_BASE_NAME)
#endif

#ifndef FAST_POW_E_BASE_NAME
#   define FAST_POW_E_BASE_NAME  __FAST_NAME(POW_E_BASE_NAME)
#endif

#ifndef FAST_SQRT_BASE_NAME
#    define FAST_SQRT_BASE_NAME  __FAST_NAME(SQRT_BASE_NAME)
#endif

#ifndef FAST_POW_TABLE_BASE_NAME
#   define FAST_POW_TABLE_BASE_NAME	F_pow
#endif

/*
**  Default definitions for the entry point name of each dpml function.
*/

#ifndef F_ASIND_NAME
#    define F_ASIND_NAME  __F_SYSTEM_NAME(ASIND_BASE_NAME)
#endif

#ifndef F_ASINH_NAME
#    define F_ASINH_NAME  __F_SYSTEM_NAME(ASINH_BASE_NAME)
#endif

#ifndef F_ACOSD_NAME
#    define F_ACOSD_NAME  __F_SYSTEM_NAME(ACOSD_BASE_NAME)
#endif

#ifndef F_ACOSH_NAME
#    define F_ACOSH_NAME  __F_SYSTEM_NAME(ACOSH_BASE_NAME)
#endif

#ifndef F_ASIN_NAME
#    define F_ASIN_NAME  __F_SYSTEM_NAME(ASIN_BASE_NAME)
#endif

#ifndef F_ACOS_NAME
#    define F_ACOS_NAME  __F_SYSTEM_NAME(ACOS_BASE_NAME)
#endif

#ifndef F_ATAND_NAME
#    define F_ATAND_NAME  __F_SYSTEM_NAME(ATAND_BASE_NAME)
#endif

#ifndef F_ATAND2_NAME
#    define F_ATAND2_NAME  __F_SYSTEM_NAME(ATAND2_BASE_NAME)
#endif

#ifndef F_ATAN2_NAME
#    define F_ATAN2_NAME  __F_SYSTEM_NAME(ATAN2_BASE_NAME)
#endif

#ifndef F_ATAN_NAME
#    define F_ATAN_NAME  __F_SYSTEM_NAME(ATAN_BASE_NAME)
#endif

#ifndef F_ATANH_NAME
#    define F_ATANH_NAME  __F_SYSTEM_NAME(ATANH_BASE_NAME)
#endif

#ifndef F_CEIL_NAME
#    define F_CEIL_NAME  __F_SYSTEM_NAME(CEIL_BASE_NAME)
#endif

#ifndef F_CLASS_NAME
#    define F_CLASS_NAME  __F_SYSTEM_NAME(CLASS_BASE_NAME)
#endif

#ifndef F_COPYSIGN_NAME
#    define F_COPYSIGN_NAME  __F_SYSTEM_NAME(COPYSIGN_BASE_NAME)
#endif

#ifndef F_ERF_NAME
#    define F_ERF_NAME  __F_SYSTEM_NAME(ERF_BASE_NAME)
#endif

#ifndef F_ERFC_NAME
#    define F_ERFC_NAME  __F_SYSTEM_NAME(ERFC_BASE_NAME)
#endif

#ifndef F_ERFCX_NAME
#    define F_ERFCX_NAME  __F_SYSTEM_NAME(ERFCX_BASE_NAME)
#endif

#ifndef F_EXP_NAME
#    define F_EXP_NAME  __F_SYSTEM_NAME(EXP_BASE_NAME)
#endif

#ifndef F_EXP2_NAME
#    define F_EXP2_NAME  __F_SYSTEM_NAME(EXP2_BASE_NAME)
#endif

#ifndef F_EXP10_NAME
#    define F_EXP10_NAME  __F_SYSTEM_NAME(EXP10_BASE_NAME)
#endif

#ifndef F_EXP_TABLE_NAME
#    define F_EXP_TABLE_NAME  __D_TABLE_NAME(EXP_BASE_NAME)
#endif

#ifndef F_EXP_BUILD_FILE_NAME
#    define F_EXP_BUILD_FILE_NAME  __D_TABLE_FILE_NAME(EXP_BASE_NAME)
#endif

#ifndef F_EXP_SPECIAL_ENTRY_NAME
#   define F_EXP_SPECIAL_ENTRY_NAME \
		PASTE_2(__F_INTERNAL_NAME(EXP_BASE_NAME), _special_entry_point)
#endif

#ifndef F_EXPM1_NAME
#    define F_EXPM1_NAME  __F_SYSTEM_NAME(EXPM1_BASE_NAME)
#endif

#ifndef F_FABS_NAME
#    define F_FABS_NAME  __F_SYSTEM_NAME(FABS_BASE_NAME)
#endif

#ifndef F_FINITE_NAME
#    define F_FINITE_NAME  __F_SYSTEM_NAME(FINITE_BASE_NAME)
#endif

#ifndef F_FLOOR_NAME
#    define F_FLOOR_NAME  __F_SYSTEM_NAME(FLOOR_BASE_NAME)
#endif

#ifndef F_FREXP_NAME
#    define F_FREXP_NAME  __F_SYSTEM_NAME(FREXP_BASE_NAME)
#endif

#ifndef F_HYPOT_NAME
#    define F_HYPOT_NAME  __F_SYSTEM_NAME(HYPOT_BASE_NAME)
#endif

#ifndef F_NT_CABS_NAME
#   define F_NT_CABS_NAME __F_SYSTEM_NAME( NT_CABS_BASE_NAME )
#endif

#ifndef F_CABS_NAME
#   define F_CABS_NAME __F_SYSTEM_NAME(CABS_BASE_NAME )
#endif

#ifndef F_ISNAN_NAME
#    define F_ISNAN_NAME  __F_SYSTEM_NAME(ISNAN_BASE_NAME)
#endif

#ifndef F_LDEXP_NAME
#    define F_LDEXP_NAME  __F_SYSTEM_NAME(LDEXP_BASE_NAME)
#endif

#ifndef F_SCALB_NAME
#    define F_SCALB_NAME  __F_SYSTEM_NAME(SCALB_BASE_NAME)
#endif

#ifndef F_SCALBN_NAME
#    define F_SCALBN_NAME  __F_SYSTEM_NAME(SCALBN_BASE_NAME)
#endif

#ifndef F_SCALBLN_NAME
#    define F_SCALBLN_NAME  __F_SYSTEM_NAME(SCALBLN_BASE_NAME)
#endif

#ifndef F_J0_NAME
#    define F_J0_NAME  __F_SYSTEM_NAME(J0_BASE_NAME)
#endif

#ifndef F_J1_NAME
#    define F_J1_NAME  __F_SYSTEM_NAME(J1_BASE_NAME)
#endif

#ifndef F_JN_NAME
#    define F_JN_NAME  __F_SYSTEM_NAME(JN_BASE_NAME)
#endif

#ifndef F_Y0_NAME
#    define F_Y0_NAME  __F_SYSTEM_NAME(Y0_BASE_NAME)
#endif

#ifndef F_Y1_NAME
#    define F_Y1_NAME  __F_SYSTEM_NAME(Y1_BASE_NAME)
#endif

#ifndef F_YN_NAME
#    define F_YN_NAME  __F_SYSTEM_NAME(YN_BASE_NAME)
#endif

#ifndef F_GAMMA_NAME
#    define F_GAMMA_NAME  __F_SYSTEM_NAME(GAMMA_BASE_NAME)
#endif

#ifndef F_LGAMMA_NAME
#    define F_LGAMMA_NAME  __F_SYSTEM_NAME(LGAMMA_BASE_NAME)
#endif

#ifndef F_TGAMMA_NAME
#    define F_TGAMMA_NAME  __F_SYSTEM_NAME(TGAMMA_BASE_NAME)
#endif

#ifndef F_RT_LGAMMA_NAME
#    define F_RT_LGAMMA_NAME  __F_SYSTEM_NAME(RT_LGAMMA_BASE_NAME)
#endif

#ifndef F_LOG1P_NAME
#    define F_LOG1P_NAME  __F_SYSTEM_NAME(LOG1P_BASE_NAME)
#endif

#ifndef F_LOG2_NAME
#    define F_LOG2_NAME  __F_SYSTEM_NAME(LOG2_BASE_NAME)
#endif

#ifndef F_LOG10_NAME
#    define F_LOG10_NAME  __F_SYSTEM_NAME(LOG10_BASE_NAME)
#endif

#ifndef F_LN_NAME
#    define F_LN_NAME  __F_SYSTEM_NAME(LN_BASE_NAME)
#endif

#ifndef F_LOGB_NAME
#    define F_LOGB_NAME  __F_SYSTEM_NAME(LOGB_BASE_NAME)
#endif

#ifndef F_ILOGB_NAME
#    define F_ILOGB_NAME  __F_SYSTEM_NAME(ILOGB_BASE_NAME)
#endif

#ifndef F_REM_NAME
#    define F_REM_NAME  __F_SYSTEM_NAME(REM_BASE_NAME)
#endif

#ifndef F_REMAINDER_NAME
#    define F_REMAINDER_NAME  __F_SYSTEM_NAME(REMAINDER_BASE_NAME)
#endif

#ifndef F_REMQUO_NAME
#    define F_REMQUO_NAME  __F_SYSTEM_NAME(REMQUO_BASE_NAME)
#endif

#ifndef F_MOD_NAME
#    define F_MOD_NAME  __F_SYSTEM_NAME(MOD_BASE_NAME)
#endif

#ifndef F_MODF_NAME
#    define F_MODF_NAME  __F_SYSTEM_NAME(MODF_BASE_NAME)
#endif

#ifndef F_NINT_NAME
#    define F_NINT_NAME  __F_SYSTEM_NAME(NINT_BASE_NAME)
#endif

#ifndef F_NEXTAFTER_NAME
#    define F_NEXTAFTER_NAME  __F_SYSTEM_NAME(NEXTAFTER_BASE_NAME)
#endif

#ifndef F_NEXTTOWARD_NAME
#    define F_NEXTTOWARD_NAME  __F_SYSTEM_NAME(NEXTTOWARD_BASE_NAME)
#endif

#ifndef F_NXTAFTR_NAME
#    define F_NXTAFTR_NAME __F_USER_NAME(NXTAFTR_BASE_NAME) 
#endif

#ifndef F_POW_NAME
#   define F_POW_NAME  __F_SYSTEM_NAME(POW_BASE_NAME)
#endif

#ifndef F_POW_E_NAME
#   define F_POW_E_NAME __F_USER_NAME(POW_E_BASE_NAME)
#endif

#ifndef F_POW_TABLE_NAME
#    define F_POW_TABLE_NAME  __F_TABLE_NAME(POW_TABLE_BASE_NAME)
#endif

#ifndef F_POW_BUILD_FILE_NAME
#    if defined(ONE_TYPE)
#        define F_POW_BUILD_FILE_NAME  __F_TABLE_FILE_NAME(POW_TABLE_BASE_NAME)
#    else
#        define F_POW_BUILD_FILE_NAME  __B_TABLE_FILE_NAME(POW_TABLE_BASE_NAME)
#    endif
#endif

#if !defined(SPECIAL_EXP_HEADER)
#   define SPECIAL_EXP_HEADER	ADD_EXTENSION(ADD_BUILD_PREFIX(special_exp),h)  
#endif

#ifndef F_RANDOM_NAME
#    define F_RANDOM_NAME  __F_SYSTEM_NAME(RANDOM_BASE_NAME)
#endif

#ifndef F_VMS_RANDOM_NAME
#    define F_VMS_RANDOM_NAME  __F_SYSTEM_NAME(VMS_RANDOM_BASE_NAME)
#endif

#ifndef F_RINT_NAME
#    define F_RINT_NAME  __F_SYSTEM_NAME(RINT_BASE_NAME)
#endif

#ifndef F_LRINT_NAME
#    define F_LRINT_NAME  __F_SYSTEM_NAME(LRINT_BASE_NAME)
#endif

#ifndef F_LROUND_NAME
#    define F_LROUND_NAME  __F_SYSTEM_NAME(LROUND_BASE_NAME)
#endif

#ifndef F_LLRINT_NAME
#    define F_LLRINT_NAME  __F_SYSTEM_NAME(LLRINT_BASE_NAME)
#endif

#ifndef F_LLROUND_NAME
#    define F_LLROUND_NAME  __F_SYSTEM_NAME(LLROUND_BASE_NAME)
#endif

#ifndef F_SIN_NAME
#    define F_SIN_NAME  __F_SYSTEM_NAME(SIN_BASE_NAME)
#endif

#ifndef F_SIN_VO_NAME
#    define F_SIN_VO_NAME  __F_USER_NAME(SIN_VO_BASE_NAME)
#endif

#ifndef F_COS_NAME
#    define F_COS_NAME  __F_SYSTEM_NAME(COS_BASE_NAME)
#endif

#ifndef F_COS_VO_NAME
#    define F_COS_VO_NAME  __F_USER_NAME(COS_VO_BASE_NAME)
#endif

#ifndef F_SINCOS_NAME
#    define F_SINCOS_NAME  __F_SYSTEM_NAME(SINCOS_BASE_NAME)
#endif

#ifndef F_SINCOS_VO_NAME
#    define F_SINCOS_VO_NAME  __F_USER_NAME(SINCOS_VO_BASE_NAME)
#endif

#ifndef F_SIND_NAME
#    define F_SIND_NAME  __F_SYSTEM_NAME(SIND_BASE_NAME)
#endif

#ifndef F_COSD_NAME
#    define F_COSD_NAME  __F_SYSTEM_NAME(COSD_BASE_NAME)
#endif

#ifndef F_SINCOSD_NAME
#    define F_SINCOSD_NAME  __F_SYSTEM_NAME(SINCOSD_BASE_NAME)
#endif

#ifndef F_SINH_NAME
#    define F_SINH_NAME  __F_SYSTEM_NAME(SINH_BASE_NAME)
#endif

#ifndef F_COSH_NAME
#    define F_COSH_NAME  __F_SYSTEM_NAME(COSH_BASE_NAME)
#endif

#ifndef F_SQRT_NAME
#    define F_SQRT_NAME  __F_SYSTEM_NAME(SQRT_BASE_NAME)
#endif

#ifndef F_SQRT_TABLE_NAME
#    define F_SQRT_TABLE_NAME  __F_TABLE_NAME(SQRT_BASE_NAME)
#endif

#ifndef F_RSQRT_NAME
#    define F_RSQRT_NAME  __F_USER_NAME(RSQRT_BASE_NAME)
#endif

#ifndef F_RSQRT_TABLE_NAME
#    define F_RSQRT_TABLE_NAME  __F_TABLE_NAME(RSQRT_BASE_NAME)
#endif

#ifndef F_CBRT_NAME
#    define F_CBRT_NAME  __F_SYSTEM_NAME(CBRT_BASE_NAME)
#endif

#ifndef F_CBRT_TABLE_NAME
#    define F_CBRT_TABLE_NAME  __F_TABLE_NAME(CBRT_BASE_NAME)
#endif

#ifndef F_TAN_NAME
#    define F_TAN_NAME  __F_SYSTEM_NAME(TAN_BASE_NAME)
#endif

#ifndef F_COT_NAME
#    define F_COT_NAME  __F_SYSTEM_NAME(COT_BASE_NAME)
#endif

#ifndef F_TANCOT_NAME
#    define F_TANCOT_NAME  __F_SYSTEM_NAME(TANCOT_BASE_NAME)
#endif

#ifndef F_TAND_NAME
#    define F_TAND_NAME  __F_SYSTEM_NAME(TAND_BASE_NAME)
#endif

#ifndef F_COTD_NAME
#    define F_COTD_NAME  __F_SYSTEM_NAME(COTD_BASE_NAME)
#endif

#ifndef F_TANCOTD_NAME
#    define F_TANCOTD_NAME  __F_SYSTEM_NAME(TANCOTD_BASE_NAME)
#endif

#ifndef F_TANH_NAME
#    define F_TANH_NAME  __F_SYSTEM_NAME(TANH_BASE_NAME)
#endif

#ifndef F_TRIG_CONS_NAME
#    define F_TRIG_CONS_NAME  __F_USER_NAME(TRIG_CONS_BASE_NAME)
#endif

#ifndef F_TRIGD_CONS_NAME
#    define F_TRIGD_CONS_NAME  __F_USER_NAME(TRIGD_CONS_BASE_NAME)
#endif

#ifndef F_TRIG_REDUCE_NAME
#    define F_TRIG_REDUCE_NAME  __F_USER_NAME(TRIG_REDUCE_BASE_NAME)
#endif

#ifndef F_TRIG_REDUCE_VO_NAME
#    define F_TRIG_REDUCE_VO_NAME  __F_USER_NAME(TRIG_REDUCE_VO_BASE_NAME)
#endif

#ifndef F_TRIGD_REDUCE_NAME
#    define F_TRIGD_REDUCE_NAME  __F_USER_NAME(TRIGD_REDUCE_BASE_NAME)
#endif

#ifndef F_TRUNC_NAME
#    define F_TRUNC_NAME  __F_SYSTEM_NAME(TRUNC_BASE_NAME)
#endif

#ifndef F_UNORDERED_NAME
#    define F_UNORDERED_NAME  __F_SYSTEM_NAME(UNORDERED_BASE_NAME)
#endif

#ifndef F_CCOS_NAME
#    define F_CCOS_NAME  __F_SYSTEM_NAME(CCOS_BASE_NAME)
#endif

#ifndef F_CDIV_NAME
#    define F_CDIV_NAME  __F_SYSTEM_NAME(CDIV_BASE_NAME)
#endif

#ifndef F_CEXP_NAME
#    define F_CEXP_NAME  __F_SYSTEM_NAME(CEXP_BASE_NAME)
#endif

#ifndef F_CLOG_NAME
#    define F_CLOG_NAME  __F_SYSTEM_NAME(CLOG_BASE_NAME)
#endif

#ifndef F_CMUL_NAME
#    define F_CMUL_NAME  __F_SYSTEM_NAME(CMUL_BASE_NAME)
#endif

#ifndef F_CPOW_NAME
#    define F_CPOW_NAME  __F_SYSTEM_NAME(CPOW_BASE_NAME)
#endif

#ifndef F_CPOWI_NAME
#   define F_CPOWI_NAME  __F_SYSTEM_NAME(CPOWI_BASE_NAME)
#endif

#ifndef F_CSIN_NAME
#    define F_CSIN_NAME  __F_SYSTEM_NAME(CSIN_BASE_NAME)
#endif

#ifndef F_CSQRT_NAME
#    define F_CSQRT_NAME  __F_SYSTEM_NAME(CSQRT_BASE_NAME)
#endif

#ifndef F_CACOS_NAME
#    define F_CACOS_NAME  __F_SYSTEM_NAME(CACOS_BASE_NAME)
#endif

#ifndef F_CASIN_NAME
#    define F_CASIN_NAME  __F_SYSTEM_NAME(CASIN_BASE_NAME)
#endif

#ifndef F_CATAN_NAME
#    define F_CATAN_NAME  __F_SYSTEM_NAME(CATAN_BASE_NAME)
#endif

#ifndef F_CTAN_NAME
#    define F_CTAN_NAME  __F_SYSTEM_NAME(CTAN_BASE_NAME)
#endif

#ifndef F_CCOSH_NAME
#    define F_CCOSH_NAME  __F_SYSTEM_NAME(CCOSH_BASE_NAME)
#endif

#ifndef F_CSINH_NAME
#    define F_CSINH_NAME  __F_SYSTEM_NAME(CSINH_BASE_NAME)
#endif

#ifndef F_CTANH_NAME
#    define F_CTANH_NAME  __F_SYSTEM_NAME(CTANH_BASE_NAME)
#endif

#ifndef F_CACOSH_NAME
#    define F_CACOSH_NAME  __F_SYSTEM_NAME(CACOSH_BASE_NAME)
#endif

#ifndef F_CASINH_NAME
#    define F_CASINH_NAME  __F_SYSTEM_NAME(CASINH_BASE_NAME)
#endif

#ifndef F_CATANH_NAME
#    define F_CATANH_NAME  __F_SYSTEM_NAME(CATANH_BASE_NAME)
#endif

#ifndef F_CARG_NAME
#    define F_CARG_NAME  __F_SYSTEM_NAME(CARG_BASE_NAME)
#endif

#ifndef F_CIMAG_NAME
#    define F_CIMAG_NAME  __F_SYSTEM_NAME(CIMAG_BASE_NAME)
#endif

#ifndef F_CREAL_NAME
#    define F_CREAL_NAME  __F_SYSTEM_NAME(CREAL_BASE_NAME)
#endif

#ifndef F_CPROJ_NAME
#    define F_CPROJ_NAME  __F_SYSTEM_NAME(CPROJ_BASE_NAME)
#endif

#ifndef F_CONJ_NAME
#    define F_CONJ_NAME  __F_SYSTEM_NAME(CONJ_BASE_NAME)
#endif

#ifndef F_NAN_NAME
#    define F_NAN_NAME  __F_SYSTEM_NAME(NAN_BASE_NAME)
#endif

#ifndef F_CVTAS_NAN_NAME
#    define F_CVTAS_NAN_NAME \
      PASTE_3(F_CVTAS_NAME_PREFIX, STRING_TO_NAN_BASE_NAME, F_CVTAS_SUFFIX)
#endif

#ifndef F_FDIM_NAME
#    define F_FDIM_NAME  __F_SYSTEM_NAME(FDIM_BASE_NAME)
#endif

#ifndef F_FMAX_NAME
#    define F_FMAX_NAME  __F_SYSTEM_NAME(FMAX_BASE_NAME)
#endif

#ifndef F_FMIN_NAME
#    define F_FMIN_NAME  __F_SYSTEM_NAME(FMIN_BASE_NAME)
#endif

#ifndef F_FMA_NAME
#    define F_FMA_NAME  __F_SYSTEM_NAME(FMA_BASE_NAME)
#endif

#ifndef F_SIGNIFICAND_NAME
#    define F_SIGNIFICAND_NAME  __F_SYSTEM_NAME(SIGNIFICAND_BASE_NAME)
#endif

#ifndef F_POW_I_NAME
#    define F_POW_I_NAME  __F_SYSTEM_NAME(POW_I_BASE_NAME)
#endif

#ifndef F_POW_I_Z_NAME
#    define F_POW_I_Z_NAME  __F_SYSTEM_NAME(POW_I_Z_BASE_NAME)
#endif

#ifndef F_POW_I_E_NAME
#    define F_POW_I_E_NAME  __F_USER_NAME(POW_I_E_BASE_NAME)
#endif

#ifndef F_POW_I_II_NAME
#   define F_POW_I_II_NAME  __SYSTEM_NAME(POW_I_II_BASE_NAME)
#endif

#ifndef F_POW_E_I_II_NAME
#   define F_POW_E_I_II_NAME  __USER_NAME(POW_E_I_II_BASE_NAME)
#endif

#ifndef F_SINHCOSH_NAME
#    define F_SINHCOSH_NAME  __F_SYSTEM_NAME(SINHCOSH_BASE_NAME)
#endif

#ifndef F_MUL_NAME
#   define F_MUL_NAME  __F_SYSTEM_NAME(MUL_BASE_NAME)
#endif

#ifndef F_DIV_NAME
#   define F_DIV_NAME  __F_SYSTEM_NAME(DIV_BASE_NAME)
#endif

#ifndef F_ADD_NAME
#   define F_ADD_NAME  __F_SYSTEM_NAME(ADD_BASE_NAME)
#endif

#ifndef F_SUB_NAME
#   define F_SUB_NAME  __F_SYSTEM_NAME(SUB_BASE_NAME  )
#endif

#ifndef F_NEG_NAME
#   define F_NEG_NAME  __F_SYSTEM_NAME(NEG_BASE_NAME)
#endif

#ifndef F_ITOF_NAME
#   define F_ITOF_NAME  __F_SYSTEM_NAME(ITOF_BASE_NAME)
#endif

#ifndef F_CMP_NAME
#   define F_CMP_NAME __F_SYSTEM_NAME(CMP_BASE_NAME)
#endif

/*
**  Default definitions for the fast entry points.
*/

#ifndef F_FAST_ACOS_NAME
#    define F_FAST_ACOS_NAME  __F_SYSTEM_NAME(FAST_ACOS_BASE_NAME)
#endif

#ifndef F_FAST_ASIN_NAME
#    define F_FAST_ASIN_NAME  __F_SYSTEM_NAME(FAST_ASIN_BASE_NAME)
#endif

#ifndef F_FAST_ATAN_NAME
#    define F_FAST_ATAN_NAME  __F_SYSTEM_NAME(FAST_ATAN_BASE_NAME)
#endif

#ifndef F_FAST_EXP_NAME
#    define F_FAST_EXP_NAME  __F_SYSTEM_NAME(FAST_EXP_BASE_NAME)
#endif

#ifndef F_FAST_EXP_TABLE_NAME
#    define F_FAST_EXP_TABLE_NAME  __D_TABLE_NAME(FAST_EXP_BASE_NAME)
#endif

#ifndef F_FAST_EXP_BUILD_FILE_NAME
#    define F_FAST_EXP_BUILD_FILE_NAME  __D_TABLE_FILE_NAME(FAST_EXP_BASE_NAME)
#endif

#ifndef F_FAST_LN_NAME
#    define F_FAST_LN_NAME  __F_SYSTEM_NAME(FAST_LN_BASE_NAME)
#endif

#ifndef F_FAST_LOG10_NAME
#    define F_FAST_LOG10_NAME  __F_SYSTEM_NAME(FAST_LOG10_BASE_NAME)
#endif

#ifndef F_FAST_SINCOS_NAME
#    define F_FAST_SINCOS_NAME  __F_SYSTEM_NAME(FAST_SINCOS_BASE_NAME)
#endif

#ifndef F_FAST_SIN_NAME
#    define F_FAST_SIN_NAME  __F_SYSTEM_NAME(FAST_SIN_BASE_NAME)
#endif

#ifndef F_FAST_COS_NAME
#    define F_FAST_COS_NAME  __F_SYSTEM_NAME(FAST_COS_BASE_NAME)
#endif

#ifndef F_FAST_SINCOSD_NAME
#    define F_FAST_SINCOSD_NAME  __F_SYSTEM_NAME(FAST_SINCOSD_BASE_NAME)
#endif

#ifndef F_FAST_SIND_NAME
#    define F_FAST_SIND_NAME  __F_SYSTEM_NAME(FAST_SIND_BASE_NAME)
#endif

#ifndef F_FAST_COSD_NAME
#    define F_FAST_COSD_NAME  __F_SYSTEM_NAME(FAST_COSD_BASE_NAME)
#endif

#ifndef F_FAST_TAN_NAME
#    define F_FAST_TAN_NAME  __F_SYSTEM_NAME(FAST_TAN_BASE_NAME)
#endif

#ifndef F_FAST_ATAN2_NAME
#    define F_FAST_ATAN2_NAME  __F_SYSTEM_NAME(FAST_ATAN2_BASE_NAME)
#endif

#ifndef F_FAST_HYPOT_NAME
#    define F_FAST_HYPOT_NAME  __F_SYSTEM_NAME(FAST_HYPOT_BASE_NAME)
#endif

#ifndef F_FAST_POW_NAME
#   define F_FAST_POW_NAME  __F_SYSTEM_NAME(FAST_POW_BASE_NAME)
#endif

#ifndef F_FAST_POW_E_NAME
#   define F_FAST_POW_E_NAME __F_USER_NAME(FAST_POW_E_BASE_NAME)
#endif

#ifndef F_FAST_POW_TABLE_NAME
#    define F_FAST_POW_TABLE_NAME  __B_TABLE_NAME(FAST_POW_TABLE_BASE_NAME)
#endif

#ifndef F_FAST_POW_BUILD_FILE_NAME
#    define F_FAST_POW_BUILD_FILE_NAME   F_POW_BUILD_FILE_NAME
#endif

#ifndef F_FAST_SQRT_NAME
#    define F_FAST_SQRT_NAME  __F_SYSTEM_NAME(FAST_SQRT_BASE_NAME)
#endif


/*
**  Backup function name definitions
*/

#ifdef B_TYPE


#ifndef B_ASIND_NAME
#    define B_ASIND_NAME  __B_SYSTEM_NAME(ASIND_BASE_NAME)
#endif

#ifndef B_ASINH_NAME
#    define B_ASINH_NAME  __B_SYSTEM_NAME(ASINH_BASE_NAME)
#endif

#ifndef B_ACOSD_NAME
#    define B_ACOSD_NAME  __B_SYSTEM_NAME(ACOSD_BASE_NAME)
#endif

#ifndef B_ACOSH_NAME
#    define B_ACOSH_NAME  __B_SYSTEM_NAME(ACOSH_BASE_NAME)
#endif

#ifndef B_ASIN_NAME
#    define B_ASIN_NAME  __B_SYSTEM_NAME(ASIN_BASE_NAME)
#endif

#ifndef B_ACOS_NAME
#    define B_ACOS_NAME  __B_SYSTEM_NAME(ACOS_BASE_NAME)
#endif

#ifndef B_ATAND_NAME
#    define B_ATAND_NAME  __B_SYSTEM_NAME(ATAND_BASE_NAME)
#endif

#ifndef B_ATAND2_NAME
#    define B_ATAND2_NAME  __B_SYSTEM_NAME(ATAND2_BASE_NAME)
#endif

#ifndef B_ATAN2_NAME
#    define B_ATAN2_NAME  __B_SYSTEM_NAME(ATAN2_BASE_NAME)
#endif

#ifndef B_ATAN_NAME
#    define B_ATAN_NAME  __B_SYSTEM_NAME(ATAN_BASE_NAME)
#endif

#ifndef B_ATANH_NAME
#    define B_ATANH_NAME  __B_SYSTEM_NAME(ATANH_BASE_NAME)
#endif

#ifndef B_CEIL_NAME
#    define B_CEIL_NAME  __B_SYSTEM_NAME(CEIL_BASE_NAME)
#endif

#ifndef B_CLASS_NAME
#    define B_CLASS_NAME  __B_SYSTEM_NAME(CLASS_BASE_NAME)
#endif

#ifndef B_COPYSIGN_NAME
#    define B_COPYSIGN_NAME  __B_SYSTEM_NAME(COPYSIGN_BASE_NAME)
#endif

#ifndef B_ERF_NAME
#    define B_ERF_NAME  __B_SYSTEM_NAME(ERF_BASE_NAME)
#endif

#ifndef B_ERFC_NAME
#    define B_ERFC_NAME  __B_SYSTEM_NAME(ERFC_BASE_NAME)
#endif

#ifndef B_ERFCX_NAME
#    define B_ERFCX_NAME  __B_SYSTEM_NAME(ERFCX_BASE_NAME)
#endif

#ifndef B_EXP_NAME
#    define B_EXP_NAME  __B_SYSTEM_NAME(EXP_BASE_NAME)
#endif

#ifndef B_EXP2_NAME
#    define B_EXP2_NAME  __B_SYSTEM_NAME(EXP2_BASE_NAME)
#endif

#ifndef B_EXP_SPECIAL_ENTRY_NAME
#   define B_EXP_SPECIAL_ENTRY_NAME \
		PASTE_2(__B_INTERNAL_NAME(EXP_BASE_NAME), _special_entry_point)
#endif

#ifndef B_EXPM1_NAME
#    define B_EXPM1_NAME  __B_SYSTEM_NAME(EXPM1_BASE_NAME)
#endif

#ifndef B_FABS_NAME
#    define B_FABS_NAME  __B_SYSTEM_NAME(FABS_BASE_NAME)
#endif

#ifndef B_FINITE_NAME
#    define B_FINITE_NAME  __B_SYSTEM_NAME(FINITE_BASE_NAME)
#endif

#ifndef B_FLOOR_NAME
#    define B_FLOOR_NAME  __B_SYSTEM_NAME(FLOOR_BASE_NAME)
#endif

#ifndef B_FREXP_NAME
#    define B_FREXP_NAME  __B_SYSTEM_NAME(FREXP_BASE_NAME)
#endif

#ifndef B_HYPOT_NAME
#    define B_HYPOT_NAME  __B_SYSTEM_NAME(HYPOT_BASE_NAME)
#endif

#ifndef B_CABS_NAME
#    define B_CABS_NAME  __B_SYSTEM_NAME(CABS_BASE_NAME)
#endif

#ifndef B_ISNAN_NAME
#    define B_ISNAN_NAME  __B_SYSTEM_NAME(ISNAN_BASE_NAME)
#endif

#ifndef B_LDEXP_NAME
#    define B_LDEXP_NAME  __B_SYSTEM_NAME(LDEXP_BASE_NAME)
#endif

#ifndef B_SCALB_NAME
#    define B_SCALB_NAME  __B_SYSTEM_NAME(SCALB_BASE_NAME)
#endif

#ifndef B_SCALBN_NAME
#    define B_SCALBN_NAME  __B_SYSTEM_NAME(SCALBN_BASE_NAME)
#endif

#ifndef B_SCALBLN_NAME
#    define B_SCALBLN_NAME  __B_SYSTEM_NAME(SCALBLN_BASE_NAME)
#endif

#ifndef B_J0_NAME
#    define B_J0_NAME  __B_SYSTEM_NAME(J0_BASE_NAME)
#endif

#ifndef B_J1_NAME
#    define B_J1_NAME  __B_SYSTEM_NAME(J1_BASE_NAME)
#endif

#ifndef B_JN_NAME
#    define B_JN_NAME  __B_SYSTEM_NAME(JN_BASE_NAME)
#endif

#ifndef B_Y0_NAME
#    define B_Y0_NAME  __B_SYSTEM_NAME(Y0_BASE_NAME)
#endif

#ifndef B_Y1_NAME
#    define B_Y1_NAME  __B_SYSTEM_NAME(Y1_BASE_NAME)
#endif

#ifndef B_YN_NAME
#    define B_YN_NAME  __B_SYSTEM_NAME(YN_BASE_NAME)
#endif

#ifndef B_GAMMA_NAME
#    define B_GAMMA_NAME  __B_SYSTEM_NAME(GAMMA_BASE_NAME)
#endif

#ifndef B_LGAMMA_NAME
#    define B_LGAMMA_NAME  __B_SYSTEM_NAME(LGAMMA_BASE_NAME)
#endif

#ifndef B_TGAMMA_NAME
#    define B_TGAMMA_NAME  __B_SYSTEM_NAME(TGAMMA_BASE_NAME)
#endif

#ifndef B_RT_LGAMMA_NAME
#    define B_RT_LGAMMA_NAME  __B_SYSTEM_NAME(RT_LGAMMA_BASE_NAME)
#endif

#ifndef B_LOG1P_NAME
#    define B_LOG1P_NAME  __B_SYSTEM_NAME(LOG1P_BASE_NAME)
#endif

#ifndef B_LOG2_NAME
#    define B_LOG2_NAME  __B_SYSTEM_NAME(LOG2_BASE_NAME)
#endif

#ifndef B_LOG10_NAME
#    define B_LOG10_NAME  __B_SYSTEM_NAME(LOG10_BASE_NAME)
#endif

#ifndef B_LN_NAME
#    define B_LN_NAME  __B_SYSTEM_NAME(LN_BASE_NAME)
#endif

#ifndef B_LOGB_NAME
#    define B_LOGB_NAME  __B_SYSTEM_NAME(LOGB_BASE_NAME)
#endif

#ifndef B_ILOGB_NAME
#    define B_ILOGB_NAME  __B_SYSTEM_NAME(ILOGB_BASE_NAME)
#endif

#ifndef B_REM_NAME
#    define B_REM_NAME  __B_SYSTEM_NAME(REM_BASE_NAME)
#endif

#ifndef B_REMAINDER_NAME
#    define B_REMAINDER_NAME  __B_SYSTEM_NAME(REMAINDER_BASE_NAME)
#endif

#ifndef B_REMQUO_NAME
#    define B_REMQUO_NAME  __B_SYSTEM_NAME(REMQUO_BASE_NAME)
#endif

#ifndef B_MOD_NAME
#    define B_MOD_NAME  __B_SYSTEM_NAME(MOD_BASE_NAME)
#endif

#ifndef B_MODF_NAME
#    define B_MODF_NAME  __B_SYSTEM_NAME(MODF_BASE_NAME)
#endif

#ifndef B_NINT_NAME
#    define B_NINT_NAME  __B_SYSTEM_NAME(NINT_BASE_NAME)
#endif

#ifndef B_NEXTAFTER_NAME
#    define B_NEXTAFTER_NAME  __B_SYSTEM_NAME(NEXTAFTER_BASE_NAME)
#endif

#ifndef B_NEXTTOWARD_NAME
#    define B_NEXTTOWARD_NAME  __B_SYSTEM_NAME(NEXTTOWARD_BASE_NAME)
#endif

#ifndef B_NXTAFTR_NAME
#    define B_NXTAFTR_NAME  __B_USER_NAME(NXTAFTR_BASE_NAME)
#endif


#ifndef B_POW_NAME
#   define B_POW_NAME  __B_SYSTEM_NAME(POW_BASE_NAME)
#endif

#ifndef B_POW_E_NAME
#   define B_POW_E_NAME __B_USER_NAME(POW_E_BASE_NAME)
#endif

#ifndef B_RANDOM_NAME
#    define B_RANDOM_NAME  __B_SYSTEM_NAME(RANDOM_BASE_NAME)
#endif

#ifndef B_VMS_RANDOM_NAME
#    define B_VMS_RANDOM_NAME  __B_SYSTEM_NAME(VMS_RANDOM_BASE_NAME)
#endif

#ifndef B_RINT_NAME
#    define B_RINT_NAME  __B_SYSTEM_NAME(RINT_BASE_NAME)
#endif

#ifndef B_LRINT_NAME
#    define B_LRINT_NAME  __B_SYSTEM_NAME(LRINT_BASE_NAME)
#endif

#ifndef B_LROUND_NAME
#    define B_LROUND_NAME  __B_SYSTEM_NAME(LROUND_BASE_NAME)
#endif

#ifndef B_LLRINT_NAME
#    define B_LLRINT_NAME  __B_SYSTEM_NAME(LLRINT_BASE_NAME)
#endif

#ifndef B_LLROUND_NAME
#    define B_LLROUND_NAME  __B_SYSTEM_NAME(LLROUND_BASE_NAME)
#endif

#ifndef B_SIN_NAME
#    define B_SIN_NAME  __B_SYSTEM_NAME(SIN_BASE_NAME)
#endif

#ifndef B_SIN_VO_NAME
#    define B_SIN_VO_NAME  __B_SYSTEM_NAME(SIN_VO_BASE_NAME)
#endif

#ifndef B_COS_NAME
#    define B_COS_NAME  __B_SYSTEM_NAME(COS_BASE_NAME)
#endif

#ifndef B_COS_VO_NAME
#    define B_COS_VO_NAME  __B_SYSTEM_NAME(COS_VO_BASE_NAME)
#endif

#ifndef B_SINCOS_NAME
#    define B_SINCOS_NAME  __B_SYSTEM_NAME(SINCOS_BASE_NAME)
#endif

#ifndef B_SINCOS_VO_NAME
#    define B_SINCOS_VO_NAME  __B_SYSTEM_NAME(SINCOS_VO_BASE_NAME)
#endif

#ifndef B_SIND_NAME
#    define B_SIND_NAME  __B_SYSTEM_NAME(SIND_BASE_NAME)
#endif

#ifndef B_COSD_NAME
#    define B_COSD_NAME  __B_SYSTEM_NAME(COSD_BASE_NAME)
#endif

#ifndef B_SINCOSD_NAME
#    define B_SINCOSD_NAME  __B_SYSTEM_NAME(SINCOSD_BASE_NAME)
#endif

#ifndef B_SINH_NAME
#    define B_SINH_NAME  __B_SYSTEM_NAME(SINH_BASE_NAME)
#endif

#ifndef B_COSH_NAME
#    define B_COSH_NAME  __B_SYSTEM_NAME(COSH_BASE_NAME)
#endif

#ifndef B_SQRT_NAME
#    define B_SQRT_NAME  __B_SYSTEM_NAME(SQRT_BASE_NAME)
#endif

#ifndef B_SQRT_TABLE_NAME
#    define B_SQRT_TABLE_NAME  __B_TABLE_NAME(SQRT_BASE_NAME)
#endif

#ifndef B_RSQRT_NAME
#    define B_RSQRT_NAME  __B_USER_NAME(RSQRT_BASE_NAME)
#endif

#ifndef B_RSQRT_TABLE_NAME
#    define B_RSQRT_TABLE_NAME  __B_TABLE_NAME(RSQRT_BASE_NAME)
#endif

#ifndef B_CBRT_NAME
#    define B_CBRT_NAME  __B_SYSTEM_NAME(CBRT_BASE_NAME)
#endif

#ifndef B_CBRT_TABLE_NAME
#    define B_CBRT_TABLE_NAME  __B_TABLE_NAME(CBRT_BASE_NAME)
#endif

#ifndef B_TAN_NAME
#    define B_TAN_NAME  __B_SYSTEM_NAME(TAN_BASE_NAME)
#endif

#ifndef B_COT_NAME
#    define B_COT_NAME  __B_SYSTEM_NAME(COT_BASE_NAME)
#endif

#ifndef B_TANCOT_NAME
#    define B_TANCOT_NAME  __B_SYSTEM_NAME(TANCOT_BASE_NAME)
#endif

#ifndef B_TAND_NAME
#    define B_TAND_NAME  __B_SYSTEM_NAME(TAND_BASE_NAME)
#endif

#ifndef B_COTD_NAME
#    define B_COTD_NAME  __B_SYSTEM_NAME(COTD_BASE_NAME)
#endif

#ifndef B_TANCOTD_NAME
#    define B_TANCOTD_NAME  __B_SYSTEM_NAME(TANCOTD_BASE_NAME)
#endif

#ifndef B_TANH_NAME
#    define B_TANH_NAME  __B_SYSTEM_NAME(TANH_BASE_NAME)
#endif

#ifndef B_TRIG_CONS_NAME
#    define B_TRIG_CONS_NAME  __B_USER_NAME(TRIG_CONS_BASE_NAME)
#endif

#ifndef B_TRIGD_CONS_NAME
#    define B_TRIGD_CONS_NAME  __B_USER_NAME(TRIGD_CONS_BASE_NAME)
#endif

#ifndef B_TRIG_REDUCE_NAME
#    define B_TRIG_REDUCE_NAME  __B_USER_NAME(TRIG_REDUCE_BASE_NAME)
#endif

#ifndef B_TRIG_REDUCE_VO_NAME
#    define B_TRIG_REDUCE_VO_NAME  __B_USER_NAME(TRIG_REDUCE_VO_BASE_NAME)
#endif

#ifndef B_TRIGD_REDUCE_NAME
#    define B_TRIGD_REDUCE_NAME  __B_USER_NAME(TRIGD_REDUCE_BASE_NAME)
#endif

#ifndef B_TRUNC_NAME
#    define B_TRUNC_NAME  __B_SYSTEM_NAME(TRUNC_BASE_NAME)
#endif

#ifndef B_UNORDERED_NAME
#    define B_UNORDERED_NAME  __B_SYSTEM_NAME(UNORDERED_BASE_NAME)
#endif

#ifndef B_CCOS_NAME
#    define B_CCOS_NAME  __B_SYSTEM_NAME(CCOS_BASE_NAME)
#endif

#ifndef B_CDIV_NAME
#    define B_CDIV_NAME  __B_SYSTEM_NAME(CDIV_BASE_NAME)
#endif

#ifndef B_CEXP_NAME
#    define B_CEXP_NAME  __B_SYSTEM_NAME(CEXP_BASE_NAME)
#endif

#ifndef B_CLOG_NAME
#    define B_CLOG_NAME  __B_SYSTEM_NAME(CLOG_BASE_NAME)
#endif

#ifndef B_CMUL_NAME
#    define B_CMUL_NAME  __B_SYSTEM_NAME(CMUL_BASE_NAME)
#endif

#ifndef B_CPOW_NAME
#    define B_CPOW_NAME  __B_SYSTEM_NAME(CPOW_BASE_NAME)
#endif

#ifndef B_CPOWI_NAME
#   define B_CPOWI_NAME  __B_SYSTEM_NAME(CPOWI_BASE_NAME)
#endif

#ifndef B_CSIN_NAME
#    define B_CSIN_NAME  __B_SYSTEM_NAME(CSIN_BASE_NAME)
#endif

#ifndef B_CSQRT_NAME
#    define B_CSQRT_NAME  __B_SYSTEM_NAME(CSQRT_BASE_NAME)
#endif

#ifndef B_CACOS_NAME
#    define B_CACOS_NAME  __B_SYSTEM_NAME(CACOS_BASE_NAME)
#endif

#ifndef B_CASIN_NAME
#    define B_CASIN_NAME  __B_SYSTEM_NAME(CASIN_BASE_NAME)
#endif

#ifndef B_CATAN_NAME
#    define B_CATAN_NAME  __B_SYSTEM_NAME(CATAN_BASE_NAME)
#endif

#ifndef B_CTAN_NAME
#    define B_CTAN_NAME  __B_SYSTEM_NAME(CTAN_BASE_NAME)
#endif

#ifndef B_CCOSH_NAME
#    define B_CCOSH_NAME  __B_SYSTEM_NAME(CCOSH_BASE_NAME)
#endif

#ifndef B_CSINH_NAME
#    define B_CSINH_NAME  __B_SYSTEM_NAME(CSINH_BASE_NAME)
#endif

#ifndef B_CTANH_NAME
#    define B_CTANH_NAME  __B_SYSTEM_NAME(CTANH_BASE_NAME)
#endif


#ifndef B_CACOSH_NAME
#    define B_CACOSH_NAME  __B_SYSTEM_NAME(CACOSH_BASE_NAME)
#endif

#ifndef B_CASINH_NAME
#    define B_CASINH_NAME  __B_SYSTEM_NAME(CASINH_BASE_NAME)
#endif

#ifndef B_CATANH_NAME
#    define B_CATANH_NAME  __B_SYSTEM_NAME(CATANH_BASE_NAME)
#endif

#ifndef B_CARG_NAME
#    define B_CARG_NAME  __B_SYSTEM_NAME(CARG_BASE_NAME)
#endif

#ifndef B_CIMAG_NAME
#    define B_CIMAG_NAME  __B_SYSTEM_NAME(CIMAG_BASE_NAME)
#endif

#ifndef B_CREAL_NAME
#    define B_CREAL_NAME  __B_SYSTEM_NAME(CREAL_BASE_NAME)
#endif

#ifndef B_CPROJ_NAME
#    define B_CPROJ_NAME  __B_SYSTEM_NAME(CPROJ_BASE_NAME)
#endif

#ifndef B_CONJ_NAME
#    define B_CONJ_NAME  __B_SYSTEM_NAME(CONJ_BASE_NAME)
#endif

#ifndef B_NAN_NAME
#    define B_NAN_NAME  __B_SYSTEM_NAME(NAN_BASE_NAME)
#endif

#ifndef B_FDIM_NAME
#    define B_FDIM_NAME  __B_SYSTEM_NAME(FDIM_BASE_NAME)
#endif

#ifndef B_FMAX_NAME
#    define B_FMAX_NAME  __B_SYSTEM_NAME(FMAX_BASE_NAME)
#endif

#ifndef B_FMIN_NAME
#    define B_FMIN_NAME  __B_SYSTEM_NAME(FMIN_BASE_NAME)
#endif

#ifndef B_SIGNIFICAND_NAME
#    define B_SIGNIFICAND_NAME  __B_SYSTEM_NAME(SIGNIFICAND_BASE_NAME)
#endif

#ifndef B_FMA_NAME
#    define B_FMA_NAME  __B_SYSTEM_NAME(FMA_BASE_NAME)
#endif

#ifndef B_POW_I_NAME
#    define B_POW_I_NAME  __B_SYSTEM_NAME(POW_I_BASE_NAME)
#endif

#ifndef B_POW_I_II_NAME
#    define B_POW_I_II_NAME  __B_SYSTEM_NAME(POW_I_II_BASE_NAME)
#endif

#ifndef B_SINHCOSH_NAME
#    define B_SINHCOSH_NAME  __B_SYSTEM_NAME(SINHCOSH_BASE_NAME)
#endif

#ifndef B_MUL_NAME
#   define B_MUL_NAME  __B_SYSTEM_NAME(MUL_BASE_NAME)
#endif

#ifndef B_DIV_NAME
#   define B_DIV_NAME  __B_SYSTEM_NAME(DIV_BASE_NAME)
#endif

#ifndef B_ADD_NAME
#   define B_ADD_NAME  __B_SYSTEM_NAME(ADD_BASE_NAME)
#endif

#ifndef B_SUB_NAME
#   define B_SUB_NAME  __B_SYSTEM_NAME(SUB_BASE_NAME  )
#endif

#ifndef B_NEG_NAME
#   define B_NEG_NAME  __B_SYSTEM_NAME(NEG_BASE_NAME)
#endif

#ifndef B_ITOF_NAME
#   define B_ITOF_NAME  __B_SYSTEM_NAME(ITOF_BASE_NAME)
#endif

#ifndef B_CMP_NAME
#   define B_CMP_NAME __B_SYSTEM_NAME(CMP_BASE_NAME)
#endif

/*
**  Default definitions for the fast backup entry points.
*/

#ifndef B_FAST_ACOS_NAME
#    define B_FAST_ACOS_NAME  __B_SYSTEM_NAME(FAST_ACOS_BASE_NAME)
#endif

#ifndef B_FAST_ASIN_NAME
#    define B_FAST_ASIN_NAME  __B_SYSTEM_NAME(FAST_ASIN_BASE_NAME)
#endif

#ifndef B_FAST_ATAN_NAME
#    define B_FAST_ATAN_NAME  __B_SYSTEM_NAME(FAST_ATAN_BASE_NAME)
#endif

#ifndef B_FAST_EXP_NAME
#    define B_FAST_EXP_NAME  __B_SYSTEM_NAME(FAST_EXP_BASE_NAME)
#endif

#ifndef B_FAST_LN_NAME
#    define B_FAST_LN_NAME  __B_SYSTEM_NAME(FAST_LN_BASE_NAME)
#endif

#ifndef B_FAST_LOG10_NAME
#    define B_FAST_LOG10_NAME  __B_SYSTEM_NAME(FAST_LOG10_BASE_NAME)
#endif

#ifndef B_FAST_SINCOS_NAME
#    define B_FAST_SINCOS_NAME  __B_SYSTEM_NAME(FAST_SINCOS_BASE_NAME)
#endif

#ifndef B_FAST_SIN_NAME
#    define B_FAST_SIN_NAME  __B_SYSTEM_NAME(FAST_SIN_BASE_NAME)
#endif

#ifndef B_FAST_COS_NAME
#    define B_FAST_COS_NAME  __B_SYSTEM_NAME(FAST_COS_BASE_NAME)
#endif

#ifndef B_FAST_SINCOSD_NAME
#    define B_FAST_SINCOSD_NAME  __B_SYSTEM_NAME(FAST_SINCOSD_BASE_NAME)
#endif

#ifndef B_FAST_SIND_NAME
#    define B_FAST_SIND_NAME  __B_SYSTEM_NAME(FAST_SIND_BASE_NAME)
#endif

#ifndef B_FAST_COSD_NAME
#    define B_FAST_COSD_NAME  __B_SYSTEM_NAME(FAST_COSD_BASE_NAME)
#endif

#ifndef B_FAST_TAN_NAME
#    define B_FAST_TAN_NAME  __B_SYSTEM_NAME(FAST_TAN_BASE_NAME)
#endif

#ifndef B_FAST_ATAN2_NAME
#    define B_FAST_ATAN2_NAME  __B_SYSTEM_NAME(FAST_ATAN2_BASE_NAME)
#endif

#ifndef B_FAST_HYPOT_NAME
#    define B_FAST_HYPOT_NAME  __B_SYSTEM_NAME(FAST_HYPOT_BASE_NAME)
#endif

#ifndef B_FAST_POW_NAME
#   define B_FAST_POW_NAME  __B_SYSTEM_NAME(FAST_POW_BASE_NAME)
#endif

#ifndef B_FAST_POW_E_NAME
#   define B_FAST_POW_E_NAME __B_USER_NAME(FAST_POW_E_BASE_NAME)
#endif

#ifndef B_FAST_SQRT_NAME
#    define B_FAST_SQRT_NAME  __B_SYSTEM_NAME(FAST_SQRT_BASE_NAME)
#endif



#endif  /* B_TYPE */



/*
**  Special names for 128 bits.
*/


#ifndef D_SQRT_TABLE_NAME
#    define D_SQRT_TABLE_NAME  __D_TABLE_NAME(SQRT_BASE_NAME)
#endif

#ifndef D_RSQRT_TABLE_NAME
#    define D_RSQRT_TABLE_NAME  __D_TABLE_NAME(RSQRT_BASE_NAME)
#endif



/*
**  Default names for include files which need to be globally visible within
**  the DPML.
*/

#ifndef F_TRIG_CONS_BUILD_FILE_NAME
#    define F_TRIG_CONS_BUILD_FILE_NAME  __BUILD_FILE_NAME_C(TRIG_CONS_BASE_NAME)
#endif

#ifndef F_TRIGD_CONS_BUILD_FILE_NAME
#    define F_TRIGD_CONS_BUILD_FILE_NAME  __BUILD_FILE_NAME_C(TRIGD_CONS_BASE_NAME)
#endif

#ifndef F_SINCOS_BUILD_FILE_NAME
#    define F_SINCOS_BUILD_FILE_NAME  __BUILD_FILE_NAME_C(SINCOS_BASE_NAME)
#endif

#ifndef F_TANCOT_BUILD_FILE_NAME
#    define F_TANCOT_BUILD_FILE_NAME  __F_TABLE_FILE_NAME(TANCOT_BASE_NAME)
#endif

#ifndef FOUR_OVER_PI_TABLE_NAME
#   define FOUR_OVER_PI_TABLE_NAME   __TABLE_NAME(four_over_pi)
#endif

#ifndef FOUR_OVER_PI_BUILD_FILE_NAME
#    define FOUR_OVER_PI_BUILD_FILE_NAME \
	ADD_EXTENSION(ADD_BUILD_PREFIX(four_over_pi),c)
#endif

#ifndef POW_ANSI_C_ERROR_BUILD_FILE_NAME
#    define POW_ANSI_C_ERROR_BUILD_FILE_NAME \
        ADD_EXTENSION(ADD_BUILD_PREFIX(pow_ansi_c_error),c)  
#endif

#ifndef POW_FORTRAN_ERROR_BUILD_FILE_NAME
#    define POW_FORTRAN_ERROR_BUILD_FILE_NAME \
        ADD_EXTENSION(ADD_BUILD_PREFIX(pow_fortran_error),c)
#endif

#ifndef POW_ANSI_C_ERROR_TABLE_NAME
#    define POW_ANSI_C_ERROR_TABLE_NAME  __TABLE_NAME(pow_ansi_c_error)
#endif

#ifndef POW_FORTRAN_ERROR_TABLE_NAME
#    define POW_FORTRAN_ERROR_TABLE_NAME  __TABLE_NAME(pow_fortran_error)
#endif


/*
** Establish default prefixes, suffixes and file extensions.
*/


#ifndef F_NAME_PREFIX
#   define F_NAME_PREFIX DPML_NULL_MACRO_TOKEN
#endif

#ifndef BUILD_PREFIX
#    define BUILD_PREFIX dpml_
#endif

#ifndef USER_PREFIX
#   define USER_PREFIX	F_NAME_PREFIX
#endif

#ifndef TABLE_PREFIX
#   define TABLE_PREFIX		USER_PREFIX
#endif


#ifndef INTERNAL_PREFIX
#   define INTERNAL_PREFIX	dpml_
#endif

#ifndef __F_SUFFIX
#   define  __F_SUFFIX  PASTE_2(_, F_CHAR)
#endif

#if !defined(F_CVTAS_SUFFIX)
#   define F_CVTAS_SUFFIX	__F_SUFFIX
#endif

#ifndef F_NAME_SUFFIX
#   if SINGLE_PRECISION
#       define F_NAME_SUFFIX    f
#   elif QUAD_PRECISION
#       define F_NAME_SUFFIX    l
#   else
#       define F_NAME_SUFFIX    DPML_NULL_MACRO_TOKEN
#   endif
#endif

#ifndef BUILD_SUFFIX
#    define BUILD_SUFFIX __F_SUFFIX
#endif

#ifndef D_BUILD_SUFFIX
#    define D_BUILD_SUFFIX __D_SUFFIX
#endif

#ifndef TABLE_SUFFIX
#   if (__F_SUFFIX == DPML_NULL_MACRO_TOKEN)
#       define TABLE_SUFFIX _table
#   else
#       define TABLE_SUFFIX PASTE_2(__F_SUFFIX, _table)
#   endif
#endif

#ifndef F_TABLE_SUFFIX
#   if (__F_SUFFIX == DPML_NULL_MACRO_TOKEN)
#       define F_TABLE_SUFFIX _table
#   else
#       define F_TABLE_SUFFIX PASTE_2(__F_SUFFIX, _table)
#   endif
#endif

#ifndef BUILD_FILE_EXTENSION
#   if defined(MAKE_COMMON)
#       define BUILD_FILE_EXTENSION c
#   else
#       define BUILD_FILE_EXTENSION h
#   endif
#endif



#ifdef B_TYPE

#ifndef __B_SUFFIX
#   define  __B_SUFFIX  PASTE_2(_, B_CHAR)
#endif

#ifndef B_NAME_SUFFIX
#   if QUAD_PRECISION
#       define B_NAME_SUFFIX Q_CHAR
#   else
#       define B_NAME_SUFFIX DPML_NULL_MACRO_TOKEN
#   endif
#endif

#ifndef B_TABLE_SUFFIX
#   if (__B_SUFFIX == DPML_NULL_MACRO_TOKEN)
#       define B_TABLE_SUFFIX _table
#   else
#       define B_TABLE_SUFFIX PASTE_2(__B_SUFFIX, _table)
#   endif
#endif

#endif  /* B_TYPE */



/*
**  Macros for D tables (128 bits).
*/


#ifndef __D_SUFFIX
#   define  __D_SUFFIX  PASTE_2(_, D_CHAR)
#endif

#ifndef D_TABLE_SUFFIX
#   if (__D_SUFFIX == DPML_NULL_MACRO_TOKEN)
#       define D_TABLE_SUFFIX _table
#   else
#       define D_TABLE_SUFFIX PASTE_2(__D_SUFFIX, _table)
#   endif
#endif



/*
** Macros for constructing file names.
*/

#define ADD_EXTENSION(filename,ext) filename.ext

/*
** The following code is designed to test for "null" macros before choosing
** an appropriate PASTE macro.  The ANSI C standard does not define how macros
** should behave when given null arguments and some compilers signal errors
** under these conditions (e.g. at the time of this writing, HP's ANSI C
** compiler under HP/UX).
**
** First define macro for adding prefixes
*/

#if (F_NAME_PREFIX == DPML_NULL_MACRO_TOKEN)
#   define __SYSTEM_NAME(base)	base
#else
#   define __SYSTEM_NAME(base)	PASTE_2(F_NAME_PREFIX, base)
#endif

#if (USER_PREFIX == DPML_NULL_MACRO_TOKEN)
#   define __USER_NAME(base)	base
#else
#   define __USER_NAME(base)	PASTE_2(USER_PREFIX, base)
#endif

#if (TABLE_PREFIX == DPML_NULL_MACRO_TOKEN)
#   define __TABLE_NAME(base)	base
#else
#   define __TABLE_NAME(base)	PASTE_2(TABLE_PREFIX, base)
#endif

#if (INTERNAL_PREFIX == DPML_NULL_MACRO_TOKEN)
#   define __INTERNAL_NAME(base)    base
#else
#   define __INTERNAL_NAME(base)    PASTE_2(INTERNAL_PREFIX, base)
#endif

#if (BUILD_PREFIX == DPML_NULL_MACRO_TOKEN)
#    define ADD_BUILD_PREFIX(base)	base
#else
#    define ADD_BUILD_PREFIX(base)	PASTE_2(BUILD_PREFIX, base)
#endif

/*
** Define macros for adding suffixes
*/

#if (F_NAME_SUFFIX == DPML_NULL_MACRO_TOKEN)
#   define ADD_F_SUFFIX(base)		base
#else
#   define ADD_F_SUFFIX(base)		PASTE_2(base, F_NAME_SUFFIX)
#endif

#if (B_NAME_SUFFIX == DPML_NULL_MACRO_TOKEN)
#   define ADD_B_SUFFIX(base)		base
#else
#   define ADD_B_SUFFIX(base)		PASTE_2(base, B_NAME_SUFFIX)
#endif

#if (D_NAME_SUFFIX == DPML_NULL_MACRO_TOKEN)
#   define ADD_D_SUFFIX(base)		base
#else
#   define ADD_D_SUFFIX(base)		PASTE_2(base, D_NAME_SUFFIX)
#endif

#if (BUILD_SUFFIX == DPML_NULL_MACRO_TOKEN)
#   define __BUILD_NAME(base)    ADD_BUILD_PREFIX(base)
#else
#   define __BUILD_NAME(base)    PASTE_2(ADD_BUILD_PREFIX(base), BUILD_SUFFIX)
#endif

#define __F_USER_NAME(base)	  ADD_F_SUFFIX(__USER_NAME(base))
#define __F_INTERNAL_NAME(base)	  ADD_F_SUFFIX(__INTERNAL_NAME(base))
#define __F_SYSTEM_NAME(base)	  ADD_F_SUFFIX(__SYSTEM_NAME(base))
#define __F_TABLE_NAME(base)	  PASTE_2(__TABLE_NAME(base), TABLE_SUFFIX)
#define __F_TABLE_FILE_ROOT(base) PASTE_2(ADD_BUILD_PREFIX(base),F_TABLE_SUFFIX)
#define __F_TABLE_FILE_NAME(base) ADD_EXTENSION(__F_TABLE_FILE_ROOT(base),BUILD_FILE_EXTENSION)

#define __B_USER_NAME(base)	  ADD_B_SUFFIX(__USER_NAME(base))
#define __B_INTERNAL_NAME(base)	  ADD_B_SUFFIX(__INTERNAL_NAME(base))
#define __B_SYSTEM_NAME(base)	  ADD_B_SUFFIX(__SYSTEM_NAME(base))
#define __B_TABLE_NAME(base)	  PASTE_2(__TABLE_NAME(base), B_TABLE_SUFFIX)
#define __B_TABLE_FILE_ROOT(base) PASTE_2(ADD_BUILD_PREFIX(base),B_TABLE_SUFFIX)
#define __B_TABLE_FILE_NAME(base) ADD_EXTENSION(__B_TABLE_FILE_ROOT(base),BUILD_FILE_EXTENSION)

#ifndef __FAST_NAME
#   define __FAST_NAME(base)	PASTE_2(F_, base)
#endif

#define __D_TABLE_NAME(base)	  PASTE_2(__TABLE_NAME(base), D_TABLE_SUFFIX)
#define __D_BUILD_FILE_NAME(base) ADD_EXTENSION(__D_BUILD_NAME(base),BUILD_FILE_EXTENSION)
#define __D_TABLE_FILE_ROOT(base) PASTE_2(ADD_BUILD_PREFIX(base),D_TABLE_SUFFIX)
#define __D_TABLE_FILE_NAME(base) ADD_EXTENSION(__D_TABLE_FILE_ROOT(base),BUILD_FILE_EXTENSION)


#define __BUILD_FILE_NAME(base)    ADD_EXTENSION(__BUILD_NAME(base),BUILD_FILE_EXTENSION)
#define __MP_FILE_NAME(base)       ADD_EXTENSION(__BUILD_NAME(base),mp)
#define __BUILD_FILE_NAME_C(base)  ADD_EXTENSION(__BUILD_NAME(base),c)
#define __BUILD_FILE_NAME_H(base)  ADD_EXTENSION(__BUILD_NAME(base),h)


/*
** Macros defining "generic" file names.
*/


#if !defined(TABLE_NAME) && !DONT_DEFAULT_TABLE_NAME
#    define TABLE_NAME  __F_TABLE_NAME(BASE_NAME)
#endif

#ifndef BUILD_NAME
#    define BUILD_NAME  __BUILD_NAME(BASE_NAME)
#endif

#ifndef BUILD_FILE_NAME
#    define BUILD_FILE_NAME __BUILD_FILE_NAME(BASE_NAME)
#endif

#ifndef TMP_FILE
#   define TMP_FILE	ADD_EXTENSION(BUILD_FILE_NAME,tmp)
#endif

#ifndef MP_FILE_NAME
#    define MP_FILE_NAME    __MP_FILE_NAME(BASE_NAME)
#endif

#endif  /* DPML_NAMES_H */
