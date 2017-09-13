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


#ifndef __DFP754_H_INCLUDED
#define __DFP754_H_INCLUDED


/****************************************************************************
 *  List of symbols not declared in mathimf.h 
 *  With the exception of strod/wcstod functions, these symbols 
 *  are not specified in the ISO/IEC decimal TR
 ****************************************************************************/

#ifdef __STDC_WANT_DEC_FP__ 

/* Conversions to/from int */
extern _Decimal32  int32_to_decimal32(int);
extern _Decimal64  int32_to_decimal64(int);
extern _Decimal128 int32_to_decimal128(int);
extern __int64 decimal32_to_int64_rnint(_Decimal32);
extern __int64 decimal64_to_int64_rnint(_Decimal64);
extern __int64 decimal128_to_int64_rnint(_Decimal128);

/* Negate */
extern _Decimal32   negated32  (_Decimal32 __x);
extern _Decimal64   negated64  (_Decimal64 __x);
extern _Decimal128  negated128 (_Decimal128 __x);

/* nextup, nextdown */
extern _Decimal32   nextupd32  (_Decimal32 __x);
extern _Decimal64   nextupd64  (_Decimal64 __x);
extern _Decimal128  nextupd128 (_Decimal128 __x);
extern _Decimal32   nextdownd32  (_Decimal32 __x);
extern _Decimal64   nextdownd64  (_Decimal64 __x);
extern _Decimal128  nextdownd128 (_Decimal128 __x);

/* Comparisons */
extern int   iszerod32  (_Decimal32 __x);
extern int   iszerod64  (_Decimal64 __x);
extern int   iszerod128 (_Decimal128 __x);
extern int   isequald32  (_Decimal32 __x, _Decimal32 __y);
extern int   isequald64  (_Decimal64 __x, _Decimal64 __y);
extern int   isequald128 (_Decimal128 __x, _Decimal128 __y);
extern int   isnotequald32  (_Decimal32 __x, _Decimal32 __y);
extern int   isnotequald64  (_Decimal64 __x, _Decimal64 __y);
extern int   isnotequald128 (_Decimal128 __x, _Decimal128 __y);

/* conversions from _Decimalxx (BID) to the DPD format */
extern _Decimal32   decimal32_to_dpd32  (_Decimal32 __x);
extern _Decimal64   decimal64_to_dpd64  (_Decimal64 __x);
extern _Decimal128  decimal128_to_dpd128 (_Decimal128 __x);
extern _Decimal32   dpd32_to_decimal32  (_Decimal32 __x);
extern _Decimal64   dpd64_to_decimal64  (_Decimal64 __x);
extern _Decimal128  dpd128_to_decimal128 (_Decimal128 __x);

/* Conversions to/from IEEE binary FP formats */
extern float   decimal32_to_float  (_Decimal32 __x);
extern float   decimal64_to_float  (_Decimal64 __x);
extern float   decimal128_to_float (_Decimal128 __x);
extern _Decimal32   float_to_decimal32  (float __x);
extern _Decimal64   float_to_decimal64  (float __x);
extern _Decimal128  float_to_decimal128 (float __x);
extern double   decimal32_to_double  (_Decimal32 __x);
extern double   decimal64_to_double  (_Decimal64 __x);
extern double   decimal128_to_double (_Decimal128 __x);
extern _Decimal32   double_to_decimal32  (double __x);
extern _Decimal64   double_to_decimal64  (double __x);
extern _Decimal128  double_to_decimal128 (double __x);
extern long double   decimal32_to_long_double  (_Decimal32 __x);
extern long double   decimal64_to_long_double  (_Decimal64 __x);
extern long double   decimal128_to_long_double (_Decimal128 __x);
extern _Decimal32   long_double_to_decimal32  (long double __x);
extern _Decimal64   long_double_to_decimal64  (long double __x);
extern _Decimal128  long_double_to_decimal128 (long double __x);
// these need an #if directive for _Quad_defined
// Since the directive is not available, will not declare these functions
/*extern _Quad   decimal32_to_quad  (_Decimal32 __x);
extern _Quad   decimal64_to_quad  (_Decimal64 __x);
extern _Quad   decimal128_to_quad (_Decimal128 __x);
extern _Decimal32   quad_to_decimal32  (_Quad __x);
extern _Decimal64   quad_to_decimal64  (_Quad __x);
extern _Decimal128  quad_to_decimal128 (_Quad __x);*/

/* Conversions to/from string */
extern void decimal32_to_string(char*, _Decimal32);
extern void decimal64_to_string(char*, _Decimal64);
extern void decimal128_to_string(char*, _Decimal128);
extern _Decimal32 string_to_decimal32(char*);
extern _Decimal64 string_to_decimal64(char*);
extern _Decimal128 string_to_decimal128(char*);
extern _Decimal32  strtod32(const char* restrict, char** restrict);
extern _Decimal64  strtod64(const char* restrict, char** restrict);
extern _Decimal128 strtod128(const char* restrict, char** restrict);
extern _Decimal32  wcstod32(const wchar_t* restrict, wchar_t** restrict);
extern _Decimal64  wcstod64(const wchar_t* restrict, wchar_t** restrict);
extern _Decimal128 wcstod128(const wchar_t* restrict, wchar_t** restrict);

#endif

#ifdef __STDC_WANT_DEC_FP__ 
extern _Decimal32   acosd32  (_Decimal32 __x);
extern _Decimal64   acosd64  (_Decimal64 __x);
extern _Decimal128  acosd128 (_Decimal128 __x);
extern _Decimal32   asind32  (_Decimal32 __x);
extern _Decimal64   asind64  (_Decimal64 __x);
extern _Decimal128  asind128 (_Decimal128 __x);
extern _Decimal32   atand32  (_Decimal32 __x);
extern _Decimal64   atand64  (_Decimal64 __x);
extern _Decimal128  atand128 (_Decimal128 __x);
extern _Decimal32   atan2d32  (_Decimal32 __y, _Decimal32 __x);
extern _Decimal64   atan2d64  (_Decimal64 __y, _Decimal64 __x);
extern _Decimal128  atan2d128 (_Decimal128 __y, _Decimal128 __x);
extern _Decimal32   cosd32  (_Decimal32 __x);
extern _Decimal64   cosd64  (_Decimal64 __x);
extern _Decimal128  cosd128 (_Decimal128 __x);
extern _Decimal32   sind32  (_Decimal32 __x);
extern _Decimal64   sind64  (_Decimal64 __x);
extern _Decimal128  sind128 (_Decimal128 __x);
extern _Decimal32   tand32  (_Decimal32 __x);
extern _Decimal64   tand64  (_Decimal64 __x);
extern _Decimal128  tand128 (_Decimal128 __x);
extern _Decimal32   acoshd32  (_Decimal32 __x);
extern _Decimal64   acoshd64  (_Decimal64 __x);
extern _Decimal128  acoshd128 (_Decimal128 __x);
extern _Decimal32   asinhd32  (_Decimal32 __x);
extern _Decimal64   asinhd64  (_Decimal64 __x);
extern _Decimal128  asinhd128 (_Decimal128 __x);
extern _Decimal32   atanhd32  (_Decimal32 __x);
extern _Decimal64   atanhd64  (_Decimal64 __x);
extern _Decimal128  atanhd128 (_Decimal128 __x);
extern _Decimal32   coshd32  (_Decimal32 __x);
extern _Decimal64   coshd64  (_Decimal64 __x);
extern _Decimal128  coshd128 (_Decimal128 __x);
extern _Decimal32   sinhd32  (_Decimal32 __x);
extern _Decimal64   sinhd64  (_Decimal64 __x);
extern _Decimal128  sinhd128 (_Decimal128 __x);
extern _Decimal32   tanhd32  (_Decimal32 __x);
extern _Decimal64   tanhd64  (_Decimal64 __x);
extern _Decimal128  tanhd128 (_Decimal128 __x);
extern _Decimal32   expd32   (_Decimal32 __x);
extern _Decimal64   expd64   (_Decimal64 __x);
extern _Decimal128  expd128  (_Decimal128 __x);
extern _Decimal32   exp2d32  (_Decimal32 __x);
extern _Decimal64   exp2d64  (_Decimal64 __x);
extern _Decimal128  exp2d128 (_Decimal128 __x);
extern _Decimal32   expm1d32  (_Decimal32 __x);
extern _Decimal64   expm1d64  (_Decimal64 __x);
extern _Decimal128  expm1d128 (_Decimal128 __x);
extern _Decimal32   frexpd32  (_Decimal32 __x, int *__i);
extern _Decimal64   frexpd64  (_Decimal64 __x, int *__i);
extern _Decimal128  frexpd128 (_Decimal128 __x, int *__i);
extern int   ilogbd32  (_Decimal32 __x);
extern int   ilogbd64  (_Decimal64 __x);
extern int   ilogbd128 (_Decimal128 __x);
extern _Decimal32   ldexpd32  (_Decimal32 __x, int __n);
extern _Decimal64   ldexpd64  (_Decimal64 __x, int __n);
extern _Decimal128  ldexpd128 (_Decimal128 __x, int __n);
extern _Decimal32   logd32  (_Decimal32 __x);
extern _Decimal64   logd64  (_Decimal64 __x);
extern _Decimal128  logd128 (_Decimal128 __x);
extern _Decimal32   log10d32  (_Decimal32 __x);
extern _Decimal64   log10d64  (_Decimal64 __x);
extern _Decimal128  log10d128 (_Decimal128 __x);
extern _Decimal32   log2d32  (_Decimal32 __x);
extern _Decimal64   log2d64  (_Decimal64 __x);
extern _Decimal128  log2d128 (_Decimal128 __x);
extern _Decimal32   log1pd32  (_Decimal32 __x);
extern _Decimal64   log1pd64  (_Decimal64 __x);
extern _Decimal128  log1pd128 (_Decimal128 __x);
extern _Decimal32   logbd32  (_Decimal32 __x);
extern _Decimal64   logbd64  (_Decimal64 __x);
extern _Decimal128  logbd128 (_Decimal128 __x);
extern _Decimal32   modfd32  (_Decimal32 __x, _Decimal32 *__iptr);
extern _Decimal64   modfd64  (_Decimal64 __x, _Decimal64 *__iptr);
extern _Decimal128  modfd128 (_Decimal128 __x, _Decimal128 *__iptr);
extern _Decimal32   scalbnd32  (_Decimal32 __x, int __n);
extern _Decimal64   scalbnd64  (_Decimal64 __x, int __n);
extern _Decimal128  scalbnd128 (_Decimal128 __x, int __n);
extern _Decimal32   scalblnd32  (_Decimal32 __x, long int __n);
extern _Decimal64   scalblnd64  (_Decimal64 __x, long int __n);
extern _Decimal128  scalblnd128 (_Decimal128 __x, long int __n);
extern _Decimal32   cbrtd32  (_Decimal32 __x);
extern _Decimal64   cbrtd64  (_Decimal64 __x);
extern _Decimal128  cbrtd128 (_Decimal128 __x);
extern _Decimal32   fabsd32  (_Decimal32 __x);
extern _Decimal64   fabsd64  (_Decimal64 __x);
extern _Decimal128  fabsd128 (_Decimal128 __x);
extern _Decimal32   hypotd32  (_Decimal32 __x, _Decimal32 __y);
extern _Decimal64   hypotd64  (_Decimal64 __x, _Decimal64 __y);
extern _Decimal128  hypotd128 (_Decimal128 __x, _Decimal128 __y);
extern _Decimal32   powd32   (_Decimal32 __x, _Decimal32 __y);
extern _Decimal64   powd64   (_Decimal64 __x, _Decimal64 __y);
extern _Decimal128  powd128  (_Decimal128 __x, _Decimal128 __y);
extern _Decimal32   sqrtd32  (_Decimal32 __x);
extern _Decimal64   sqrtd64  (_Decimal64 __x);
extern _Decimal128  sqrtd128 (_Decimal128 __x);
extern _Decimal32   erfd32   (_Decimal32 __x);
extern _Decimal64   erfd64   (_Decimal64 __x);
extern _Decimal128  erfd128  (_Decimal128 __x);
extern _Decimal32   erfcd32  (_Decimal32 __x);
extern _Decimal64   erfcd64  (_Decimal64 __x);
extern _Decimal128  erfcd128 (_Decimal128 __x);
extern _Decimal32   lgammad32  (_Decimal32 __x);
extern _Decimal64   lgammad64  (_Decimal64 __x);
extern _Decimal128  lgammad128 (_Decimal128 __x);
extern _Decimal32   tgammad32  (_Decimal32 __x);
extern _Decimal64   tgammad64  (_Decimal64 __x);
extern _Decimal128  tgammad128 (_Decimal128 __x);
extern _Decimal32   ceild32   (_Decimal32 __x);
extern _Decimal64   ceild64   (_Decimal64 __x);
extern _Decimal128  ceild128  (_Decimal128 __x);
extern _Decimal32   floord32  (_Decimal32 __x);
extern _Decimal64   floord64  (_Decimal64 __x);
extern _Decimal128  floord128 (_Decimal128 __x);
extern _Decimal32   nearbyintd32  (_Decimal32 __x);
extern _Decimal64   nearbyintd64  (_Decimal64 __x);
extern _Decimal128  nearbyintd128 (_Decimal128 __x);
extern _Decimal32      rintd32    (_Decimal32 __x);
extern _Decimal64      rintd64    (_Decimal64 __x);
extern _Decimal128     rintd128   (_Decimal128 __x);
extern long int        lrintd32   (_Decimal32 __x);
extern long int        lrintd64   (_Decimal64 __x);
extern long int        lrintd128  (_Decimal128 __x);
extern long long int   llrintd32  (_Decimal32 __x);
extern long long int   llrintd64  (_Decimal64 __x);
extern long long int   llrintd128 (_Decimal128 __x);
extern _Decimal32      roundd32   (_Decimal32 __x);
extern _Decimal64      roundd64   (_Decimal64 __x);
extern _Decimal128     roundd128  (_Decimal128 __x);
extern long int        lroundd32  (_Decimal32 __x);
extern long int        lroundd64  (_Decimal64 __x);
extern long int        lroundd128 (_Decimal128 __x);
extern long long int   llroundd32  (_Decimal32 __x);
extern long long int   llroundd64  (_Decimal64 __x);
extern long long int   llroundd128 (_Decimal128 __x);
extern _Decimal32   truncd32  (_Decimal32 __x);
extern _Decimal64   truncd64  (_Decimal64 __x);
extern _Decimal128  truncd128 (_Decimal128 __x);
extern _Decimal32   fmodd32  (_Decimal32 __x, _Decimal32 __y);
extern _Decimal64   fmodd64  (_Decimal64 __x, _Decimal64 __y);
extern _Decimal128  fmodd128 (_Decimal128 __x, _Decimal128 __y);
extern _Decimal32   remainderd32  (_Decimal32 __x, _Decimal32 __y);
extern _Decimal64   remainderd64  (_Decimal64 __x, _Decimal64 __y);
extern _Decimal128  remainderd128 (_Decimal128 __x, _Decimal128 __y);
extern _Decimal32   copysignd32  (_Decimal32 __x, _Decimal32 __y);
extern _Decimal64   copysignd64  (_Decimal64 __x, _Decimal64 __y);
extern _Decimal128  copysignd128 (_Decimal128 __x, _Decimal128 __y);
extern _Decimal32   nand32  (char *__tagp);
extern _Decimal64   nand64  (char *__tagp);
extern _Decimal128  nand128 (char *__tagp);
extern _Decimal32   nextafterd32  (_Decimal32 __x, _Decimal32 __y);
extern _Decimal64   nextafterd64  (_Decimal64 __x, _Decimal64 __y);
extern _Decimal128  nextafterd128 (_Decimal128 __x, _Decimal128 __y);
extern _Decimal32   nexttowardd32  (_Decimal32 __x, _Decimal128 __y);
extern _Decimal64   nexttowardd64  (_Decimal64 __x, _Decimal128 __y);
extern _Decimal128  nexttowardd128 (_Decimal128 __x, _Decimal128 __y);
extern _Decimal32   fdimd32  (_Decimal32 __x, _Decimal32 __y);
extern _Decimal64   fdimd64  (_Decimal64 __x, _Decimal64 __y);
extern _Decimal128  fdimd128 (_Decimal128 __x, _Decimal128 __y);
extern _Decimal32   fmaxd32  (_Decimal32 __x, _Decimal32 __y);
extern _Decimal64   fmaxd64  (_Decimal64 __x, _Decimal64 __y);
extern _Decimal128  fmaxd128 (_Decimal128 __x, _Decimal128 __y);
extern _Decimal32   fmind32  (_Decimal32 __x, _Decimal32 __y);
extern _Decimal64   fmind64  (_Decimal64 __x, _Decimal64 __y);
extern _Decimal128  fmind128 (_Decimal128 __x, _Decimal128 __y);
extern _Decimal32   fmad32  (_Decimal32 __x,  _Decimal32 __y, _Decimal32 __z);
extern _Decimal64   fmad64  (_Decimal64 __x,  _Decimal64 __y, _Decimal64 __z);
extern _Decimal128  fmad128 (_Decimal128 __x, _Decimal128 __y, _Decimal128 __z);
extern _Decimal32   quantized32  (_Decimal32 __x, _Decimal32 __y);
extern _Decimal64   quantized64  (_Decimal64 __x, _Decimal64 __y);
extern _Decimal128  quantized128 (_Decimal128 __x, _Decimal128 __y);
extern int   samequantumd32  (_Decimal32 __x, _Decimal32 __y);
extern int   samequantumd64  (_Decimal64 __x, _Decimal64 __y);
extern int   samequantumd128 (_Decimal128 __x, _Decimal128 __y);
extern int   quantexpd32  (_Decimal32 __x);
extern int   quantexpd64  (_Decimal64 __x);
extern int   quantexpd128 (_Decimal128 __x);
extern int   isnand32  (_Decimal32 __x);
extern int   isnand64  (_Decimal64 __x);
extern int   isnand128 (_Decimal128 __x);
extern int   isinfd32  (_Decimal32 __x);
extern int   isinfd64  (_Decimal64 __x);
extern int   isinfd128 (_Decimal128 __x);
extern int   isfinited32  (_Decimal32 __x);
extern int   isfinited64  (_Decimal64 __x);
extern int   isfinited128 (_Decimal128 __x);
extern int   isnormald32  (_Decimal32 __x);
extern int   isnormald64  (_Decimal64 __x);
extern int   isnormald128 (_Decimal128 __x);
extern int   signbitd32  (_Decimal32 __x);
extern int   signbitd64  (_Decimal64 __x);
extern int   signbitd128 (_Decimal128 __x);
extern int   fpclassifyd32  (_Decimal32 __x);
extern int   fpclassifyd64  (_Decimal64 __x);
extern int   fpclassifyd128 (_Decimal128 __x);
extern int   isunorderedd32  (_Decimal32 __x, _Decimal32 __y);
extern int   isunorderedd64  (_Decimal64 __x, _Decimal64 __y);
extern int   isunorderedd128 (_Decimal128 __x, _Decimal128 __y);
extern int   isgreaterd32  (_Decimal32 __x, _Decimal32 __y);
extern int   isgreaterd64  (_Decimal64 __x, _Decimal64 __y);
extern int   isgreaterd128 (_Decimal128 __x, _Decimal128 __y);
extern int   isgreaterequald32  (_Decimal32 __x, _Decimal32 __y);
extern int   isgreaterequald64  (_Decimal64 __x, _Decimal64 __y);
extern int   isgreaterequald128 (_Decimal128 __x, _Decimal128 __y);
extern int   islessd32  (_Decimal32 __x, _Decimal32 __y);
extern int   islessd64  (_Decimal64 __x, _Decimal64 __y);
extern int   islessd128 (_Decimal128 __x, _Decimal128 __y);
extern int   islessequald32  (_Decimal32 __x, _Decimal32 __y);
extern int   islessequald64  (_Decimal64 __x, _Decimal64 __y);
extern int   islessequald128 (_Decimal128 __x, _Decimal128 __y);
#endif  /*__STDC_WANT_DEC_FP__*/


#endif  /* __DFP754_H_INCLUDED */


