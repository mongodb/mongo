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

#define m(x) x

#define __wrap___bid64_from_int32    m(int32_to_decimal64)
//#define __wrap___bid64_from_uint32
//#define __wrap___bid64_from_int64
//#define __wrap___bid64_from_uint64
#define __wrap___bid128_from_int32    m(int32_to_decimal128)
//#define __wrap___bid128_from_uint32
//#define __wrap___bid128_from_int64
//#define __wrap___bid128_from_uint64
#define __wrap___bid32_from_int32    m(int32_to_decimal32)
//#define __wrap___bid32_from_uint32
//#define __wrap___bid32_from_int64
//#define __wrap___bid32_from_uint64
#define __wrap___bid_to_dpd32        m(decimal32_to_dpd32)
#define __wrap___bid_to_dpd64        m(decimal64_to_dpd64)
#define __wrap___bid_dpd_to_bid32    m(dpd32_to_decimal32)
#define __wrap___bid_dpd_to_bid64    m(dpd64_to_decimal64)
#define __wrap___bid_to_dpd128       m(decimal128_to_dpd128)
#define __wrap___bid_dpd_to_bid128   m(dpd128_to_decimal128)
#define __wrap___bid32_to_binary32     m(decimal32_to_float)
#define __wrap___bid64_to_binary32     m(decimal64_to_float)
#define __wrap___bid128_to_binary32    m(decimal128_to_float)
#define __wrap___bid32_to_binary64     m(decimal32_to_double)
#define __wrap___bid64_to_binary64     m(decimal64_to_double)
#define __wrap___bid128_to_binary64    m(decimal128_to_double)
#define __wrap___bid32_to_binary80     m(decimal32_to_long_double)
#define __wrap___bid64_to_binary80     m(decimal64_to_long_double)
#define __wrap___bid128_to_binary80    m(decimal128_to_long_double)
#define __wrap___bid32_to_binary128    m(decimal32_to_quad)
#define __wrap___bid64_to_binary128    m(decimal64_to_quad)
#define __wrap___bid128_to_binary128   m(decimal128_to_quad)
#define __wrap___binary32_to_bid32     m(float_to_decimal32)
#define __wrap___binary64_to_bid32     m(double_to_decimal32)
#define __wrap___binary80_to_bid32     m(long_double_to_decimal32)
#define __wrap___binary128_to_bid32    m(quad_to_decimal32)
#define __wrap___binary32_to_bid64     m(float_to_decimal64)
#define __wrap___binary64_to_bid64     m(double_to_decimal64)
#define __wrap___binary80_to_bid64     m(long_double_to_decimal64)
#define __wrap___binary128_to_bid64    m(quad_to_decimal64)
#define __wrap___binary32_to_bid128    m(float_to_decimal128)
#define __wrap___binary64_to_bid128    m(double_to_decimal128)
#define __wrap___binary80_to_bid128    m(long_double_to_decimal128)
#define __wrap___binary128_to_bid128   m(quad_to_decimal128)
//#define __wrap___bid64_to_bid128
//#define __wrap___bid128_to_bid64
//#define __wrap___bid32_to_bid64
//#define __wrap___bid64_to_bid32
//#define __wrap___bid32_to_bid128
//#define __wrap___bid128_to_bid32
//#define __wrap___bid32_sub
#define __wrap___bid32_tgamma m(tgammad32)
#define __wrap___bid32_tanh  m(tanhd32)
#define __wrap___bid32_tan   m(tand32)
#define __wrap___bid32_sinh  m(sinhd32)
#define __wrap___bid32_sin   m(sind32)
#define __wrap___bid32_pow   m(powd32)
#define __wrap___bid32_log2   m(log2d32)
#define __wrap___bid32_log1p  m(log1pd32)
#define __wrap___bid32_log10  m(log10d32)
#define __wrap___bid32_log    m(logd32)
#define __wrap___bid32_lgamma m(lgammad32)
#define __wrap___bid32_hypot  m(hypotd32)
#define __wrap___bid32_expm1  m(expm1d32)
#define __wrap___bid32_exp2   m(exp2d32)
#define __wrap___bid32_exp10  m(exp10d32)
#define __wrap___bid32_exp    m(expd32)
#define __wrap___bid32_erfc   m(erfcd32)
#define __wrap___bid32_erf    m(erfd32)
#define __wrap___bid32_cosh  m(coshd32)
#define __wrap___bid32_cos   m(cosd32)
#define __wrap___bid32_cbrt  m(cbrtd32)
#define __wrap___bid32_atanh  m(atanhd32)
#define __wrap___bid32_atan2  m(atan2d32)
#define __wrap___bid32_atan   m(atand32)
#define __wrap___bid32_asinh m(asinhd32)
#define __wrap___bid32_asin  m(asind32)
#define __wrap___bid32_acosh  m(acoshd32)
#define __wrap___bid32_acos   m(acosd32)
#define __wrap___bid_wcstod128  m(wcstod128)
#define __wrap___bid_wcstod64   m(wcstod64)
#define __wrap___bid_wcstod32   m(wcstod32)
#define __wrap___bid_strtod128  m(strtod128)
#define __wrap___bid_strtod64   m(strtod64)
#define __wrap___bid_strtod32   m(strtod32)
//#define __wrap___bid128_to_uint8_rnint
//#define __wrap___bid128_to_uint8_xrnint
//#define __wrap___bid128_to_uint8_rninta
//#define __wrap___bid128_to_uint8_xrninta
//#define __wrap___bid128_to_uint8_int
//#define __wrap___bid128_to_uint8_xint
//#define __wrap___bid128_to_uint8_floor
//#define __wrap___bid128_to_uint8_ceil
//#define __wrap___bid128_to_uint8_xfloor
//#define __wrap___bid128_to_uint8_xceil
//#define __wrap___bid128_to_uint64_rnint
//#define __wrap___bid128_to_uint64_xrnint
//#define __wrap___bid128_to_uint64_floor
//#define __wrap___bid128_to_uint64_xfloor
//#define __wrap___bid128_to_uint64_ceil
//#define __wrap___bid128_to_uint64_xceil
//#define __wrap___bid128_to_uint64_int
//#define __wrap___bid128_to_uint64_xint
//#define __wrap___bid128_to_uint64_rninta
//#define __wrap___bid128_to_uint64_xrninta
//#define __wrap___bid128_to_uint32_rnint
//#define __wrap___bid128_to_uint32_xrnint
//#define __wrap___bid128_to_uint32_floor
//#define __wrap___bid128_to_uint32_xfloor
//#define __wrap___bid128_to_uint32_ceil
//#define __wrap___bid128_to_uint32_xceil
//#define __wrap___bid128_to_uint32_int
//#define __wrap___bid128_to_uint32_xint
//#define __wrap___bid128_to_uint32_rninta
//#define __wrap___bid128_to_uint32_xrninta
//#define __wrap___bid128_to_uint16_rnint
//#define __wrap___bid128_to_uint16_xrnint
//#define __wrap___bid128_to_uint16_rninta
//#define __wrap___bid128_to_uint16_xrninta
//#define __wrap___bid128_to_uint16_int
//#define __wrap___bid128_to_uint16_xint
//#define __wrap___bid128_to_uint16_floor
//#define __wrap___bid128_to_uint16_ceil
//#define __wrap___bid128_to_uint16_xfloor
//#define __wrap___bid128_to_uint16_xceil
//#define __wrap___bid128_to_int8_rnint
//#define __wrap___bid128_to_int8_xrnint
//#define __wrap___bid128_to_int8_rninta
//#define __wrap___bid128_to_int8_xrninta
//#define __wrap___bid128_to_int8_int
//#define __wrap___bid128_to_int8_xint
//#define __wrap___bid128_to_int8_floor
//#define __wrap___bid128_to_int8_ceil
//#define __wrap___bid128_to_int8_xfloor
//#define __wrap___bid128_to_int8_xceil
#define __wrap___bid128_to_int64_rnint   m(decimal128_to_int64_rnint)
//#define __wrap___bid128_to_int64_xrnint
//#define __wrap___bid128_to_int64_floor
//#define __wrap___bid128_to_int64_xfloor
//#define __wrap___bid128_to_int64_ceil
//#define __wrap___bid128_to_int64_xceil
//#define __wrap___bid128_to_int64_int
//#define __wrap___bid128_to_int64_xint
//#define __wrap___bid128_to_int64_rninta
//#define __wrap___bid128_to_int64_xrninta
//#define __wrap___bid128_to_int32_rnint
//#define __wrap___bid128_to_int32_xrnint
//#define __wrap___bid128_to_int32_floor
//#define __wrap___bid128_to_int32_xfloor
//#define __wrap___bid128_to_int32_ceil
//#define __wrap___bid128_to_int32_xceil
//#define __wrap___bid128_to_int32_int
//#define __wrap___bid128_to_int32_xint
//#define __wrap___bid128_to_int32_rninta
//#define __wrap___bid128_to_int32_xrninta
//#define __wrap___bid128_to_int16_rnint
//#define __wrap___bid128_to_int16_xrnint
//#define __wrap___bid128_to_int16_rninta
//#define __wrap___bid128_to_int16_xrninta
//#define __wrap___bid128_to_int16_int
//#define __wrap___bid128_to_int16_xint
//#define __wrap___bid128_to_int16_floor
//#define __wrap___bid128_to_int16_ceil
//#define __wrap___bid128_to_int16_xfloor
//#define __wrap___bid128_to_int16_xceil
#define __wrap___bid128_to_string     m(decimal128_to_string)
#define __wrap___bid128_from_string   m(string_to_decimal128)
#define __wrap___bid128_sqrt   m(sqrtd128)
//#define __wrap___bid128d_sqrt
#define __wrap___bid128_scalbln  m(scalblnd128)
#define __wrap___bid128_scalbn   m(scalbnd128)
#define __wrap___bid128_round_integral_exact        m(rintd128)
//#define __wrap___bid128_round_integral_nearest_even
#define __wrap___bid128_round_integral_negative     m(floord128)
#define __wrap___bid128_round_integral_positive     m(ceild128)
#define __wrap___bid128_round_integral_zero         m(truncd128)
#define __wrap___bid128_round_integral_nearest_away m(roundd128)
#define __wrap___bid128_rem       m(remainderd128)
#define __wrap___bid128_quantize  m(quantized128)
#define __wrap___bid128_quantexp  m(quantexpd128)
#define __wrap___bid128_isSigned    m(signbitd128)
#define __wrap___bid128_isNormal    m(isnormald128)
//#define __wrap___bid128_isSubnormal
#define __wrap___bid128_isZero   m(iszerod128)
#define __wrap___bid128_isInf    m(isinfd128)
//#define __wrap___bid128_isSignaling
//#define __wrap___bid128_isCanonical
#define __wrap___bid128_isNaN    m(isnand128)
//#define __wrap___bid128_copy
#define __wrap___bid128_negate   m(negated128)
#define __wrap___bid128_abs       m(fabsd128)
#define __wrap___bid128_copySign  m(copysignd128)
#define __wrap___bid128_class    m(fpclassifyd128)
//#define __wrap___bid128_sameQuantum
//#define __wrap___bid128_totalOrder
//#define __wrap___bid128_totalOrderMag
//#define __wrap___bid128_radix
#define __wrap___bid128_isFinite    m(isfinited128)
#define __wrap___bid128_nexttoward   m(nexttoward128)
#define __wrap___bid128_nextup       m(nextupd128)
#define __wrap___bid128_nextdown     m(nextdownd128)
#define __wrap___bid128_nextafter    m(nextafterd128)
#define __wrap___bid128_nearbyint    m(nearbyintd128)
//#define __wrap___bid128_mul
#define __wrap___bid128_modf  m(modf128)
#define __wrap___bid128_minnum   m(fmind128)
//#define __wrap___bid128_minnum_mag
#define __wrap___bid128_maxnum   m(fmax128)
//#define __wrap___bid128_maxnum_mag
#define __wrap___bid128_lround m(lroundd128)
#define __wrap___bid128_lrint  m(lrintd128)
#define __wrap___bid128_logb   m(logbd128)
#define __wrap___bid128_ilogb  m(ilogbd128)
#define __wrap___bid128_llrint m(llrintd128)
#define __wrap___bid128_ldexp  m(ldexpd128)
#define __wrap___bid128_frexp  m(frexpd128)
#define __wrap___bid128_fmod   m(fmodd128)
#define __wrap___bid128_fma    m(fmad128)
#define __wrap___bid128_fdim   m(fdimd128)
//#define __wrap___bid128_div
//#define __wrap___bid128dd_div
//#define __wrap___bid128dq_div
//#define __wrap___bid128qd_div
#define __wrap___bid128_quiet_equal    m(isequald128)
#define __wrap___bid128_quiet_greater    m(isgreaterd128)
#define __wrap___bid128_quiet_greater_equal    m(isgreaterequald128)
//#define __wrap___bid128_quiet_greater_unordered
#define __wrap___bid128_quiet_less       m(islessd128)
#define __wrap___bid128_quiet_less_equal    m(islessequald128)
//#define __wrap___bid128_quiet_less_unordered
#define __wrap___bid128_quiet_not_equal     m(isnotequald128)
//#define __wrap___bid128_quiet_not_greater
//#define __wrap___bid128_quiet_not_less
//#define __wrap___bid128_quiet_ordered
#define __wrap___bid128_quiet_unordered    m(isunorderedd128)
//#define __wrap___bid128_signaling_greater
//#define __wrap___bid128_signaling_greater_equal
//#define __wrap___bid128_signaling_greater_unordered
//#define __wrap___bid128_signaling_less
//#define __wrap___bid128_signaling_less_equal
//#define __wrap___bid128_signaling_less_unordered
//#define __wrap___bid128_signaling_not_greater
//#define __wrap___bid128_signaling_not_less
//#define __wrap___bid128_add
//#define __wrap___bid128_sub
//#define __wrap___bid64_to_uint8_rnint
//#define __wrap___bid64_to_uint8_xrnint
//#define __wrap___bid64_to_uint8_rninta
//#define __wrap___bid64_to_uint8_xrninta
//#define __wrap___bid64_to_uint8_int
//#define __wrap___bid64_to_uint8_xint
//#define __wrap___bid64_to_uint8_floor
//#define __wrap___bid64_to_uint8_ceil
//#define __wrap___bid64_to_uint8_xfloor
//#define __wrap___bid64_to_uint8_xceil
//#define __wrap___bid64_to_uint64_rnint
//#define __wrap___bid64_to_uint64_xrnint
//#define __wrap___bid64_to_uint64_floor
//#define __wrap___bid64_to_uint64_xfloor
//#define __wrap___bid64_to_uint64_ceil
//#define __wrap___bid64_to_uint64_xceil
//#define __wrap___bid64_to_uint64_int
//#define __wrap___bid64_to_uint64_xint
//#define __wrap___bid64_to_uint64_rninta
//#define __wrap___bid64_to_uint64_xrninta
//#define __wrap___bid64_to_uint32_rnint
//#define __wrap___bid64_to_uint32_xrnint
//#define __wrap___bid64_to_uint32_floor
//#define __wrap___bid64_to_uint32_xfloor
//#define __wrap___bid64_to_uint32_ceil
//#define __wrap___bid64_to_uint32_xceil
//#define __wrap___bid64_to_uint32_int
//#define __wrap___bid64_to_uint32_xint
//#define __wrap___bid64_to_uint32_rninta
//#define __wrap___bid64_to_uint32_xrninta
//#define __wrap___bid64_to_uint16_rnint
//#define __wrap___bid64_to_uint16_xrnint
//#define __wrap___bid64_to_uint16_rninta
//#define __wrap___bid64_to_uint16_xrninta
//#define __wrap___bid64_to_uint16_int
//#define __wrap___bid64_to_uint16_xint
//#define __wrap___bid64_to_uint16_floor
//#define __wrap___bid64_to_uint16_ceil
//#define __wrap___bid64_to_uint16_xfloor
//#define __wrap___bid64_to_uint16_xceil
//#define __wrap___bid64_to_int8_rnint
//#define __wrap___bid64_to_int8_xrnint
//#define __wrap___bid64_to_int8_rninta
//#define __wrap___bid64_to_int8_xrninta
//#define __wrap___bid64_to_int8_int
//#define __wrap___bid64_to_int8_xint
//#define __wrap___bid64_to_int8_floor
//#define __wrap___bid64_to_int8_ceil
//#define __wrap___bid64_to_int8_xfloor
//#define __wrap___bid64_to_int8_xceil
#define __wrap___bid64_to_int64_rnint  m(decimal64_to_int64_rnint)
//#define __wrap___bid64_to_int64_xrnint
//#define __wrap___bid64_to_int64_floor
//#define __wrap___bid64_to_int64_xfloor
//#define __wrap___bid64_to_int64_ceil
//#define __wrap___bid64_to_int64_xceil
//#define __wrap___bid64_to_int64_int
//#define __wrap___bid64_to_int64_xint
//#define __wrap___bid64_to_int64_rninta
//#define __wrap___bid64_to_int64_xrninta
//#define __wrap___bid64_to_int32_rnint
//#define __wrap___bid64_to_int32_xrnint
//#define __wrap___bid64_to_int32_floor
//#define __wrap___bid64_to_int32_xfloor
//#define __wrap___bid64_to_int32_ceil
//#define __wrap___bid64_to_int32_xceil
//#define __wrap___bid64_to_int32_int
//#define __wrap___bid64_to_int32_xint
//#define __wrap___bid64_to_int32_rninta
//#define __wrap___bid64_to_int32_xrninta
//#define __wrap___bid64_to_int16_rnint
//#define __wrap___bid64_to_int16_xrnint
//#define __wrap___bid64_to_int16_rninta
//#define __wrap___bid64_to_int16_xrninta
//#define __wrap___bid64_to_int16_int
//#define __wrap___bid64_to_int16_xint
//#define __wrap___bid64_to_int16_floor
//#define __wrap___bid64_to_int16_ceil
//#define __wrap___bid64_to_int16_xfloor
//#define __wrap___bid64_to_int16_xceil
#define __wrap___bid64_to_string     m(decimal64_to_string)
#define __wrap___bid64_from_string   m(string_to_decimal64)
#define __wrap___bid64_sqrt   m(sqrtd64)
//#define __wrap___bid64q_sqrt
#define __wrap___bid64_scalbln  m(scalblnd64)
#define __wrap___bid64_scalbn   m(scalbnd64)
#define __wrap___bid64_round_integral_exact        m(rintd64)
//#define __wrap___bid64_round_integral_nearest_even
#define __wrap___bid64_round_integral_negative     m(floord64)
#define __wrap___bid64_round_integral_positive     m(ceild64)
#define __wrap___bid64_round_integral_zero         m(truncd64)
#define __wrap___bid64_round_integral_nearest_away m(roundd64)
#define __wrap___bid64_rem       m(remainderd64)
#define __wrap___bid64_quantize  m(quantized64)
#define __wrap___bid64_quantexp  m(quantexpd64)
#define __wrap___bid64_isSigned    m(signbitd64)
#define __wrap___bid64_isNormal    m(isnormald64)
//#define __wrap___bid64_isSubnormal
#define __wrap___bid64_isZero   m(iszerod64)
#define __wrap___bid64_isInf    m(isinfd64)
//#define __wrap___bid64_isSignaling
//#define __wrap___bid64_isCanonical
#define __wrap___bid64_isNaN    m(isnand64)
//#define __wrap___bid64_copy
#define __wrap___bid64_negate   m(negated64)
#define __wrap___bid64_abs       m(fabsd64)
#define __wrap___bid64_copySign  m(copysignd64)
#define __wrap___bid64_class    m(fpclassifyd64)
//#define __wrap___bid64_sameQuantum
//#define __wrap___bid64_totalOrder
//#define __wrap___bid64_totalOrderMag
//#define __wrap___bid64_radix
#define __wrap___bid64_isFinite    m(isfinited64)
#define __wrap___bid64_nexttoward   m(nexttoward64)
#define __wrap___bid64_nextup       m(nextupd64)
#define __wrap___bid64_nextdown     m(nextdownd64)
#define __wrap___bid64_nextafter    m(nextafterd64)
#define __wrap___bid64_nearbyint    m(nearbyintd64)
//#define __wrap___bid64_mul
#define __wrap___bid64_modf   m(modfd64)
#define __wrap___bid64_minnum   m(fmind64)
//#define __wrap___bid64_minnum_mag
#define __wrap___bid64_maxnum   m(fmaxd64)
//#define __wrap___bid64_maxnum_mag
#define __wrap___bid64_lround  m(lroundd64)
#define __wrap___bid64_lrint  m(lrintd64)
#define __wrap___bid64_logb   m(logbd64)
#define __wrap___bid64_ilogb  m(ilogbd64)
#define __wrap___bid64_llrint   m(llrintd64)
#define __wrap___bid64_ldexp  m(ldexpd64)
#define __wrap___bid64_frexp  m(frexpd64)
#define __wrap___bid64_fmod   m(fmodd64)
#define __wrap___bid64_fma    m(fmad64)
#define __wrap___bid64_fdim   m(fdimd64)
//#define __wrap___bid64_div
//#define __wrap___bid64dq_div
//#define __wrap___bid64qd_div
//#define __wrap___bid64qq_div
#define __wrap___bid64_quiet_equal    m(isequald64)
#define __wrap___bid64_quiet_greater    m(isgreaterd64)
#define __wrap___bid64_quiet_greater_equal    m(isgreaterequald64)
//#define __wrap___bid64_quiet_greater_unordered
#define __wrap___bid64_quiet_less    m(islessd64)
#define __wrap___bid64_quiet_less_equal    m(islessequald64)
//#define __wrap___bid64_quiet_less_unordered
#define __wrap___bid64_quiet_not_equal     m(isnotequald64)
//#define __wrap___bid64_quiet_not_greater
//#define __wrap___bid64_quiet_not_less
//#define __wrap___bid64_quiet_ordered
#define __wrap___bid64_quiet_unordered    m(isunorderedd64)
//#define __wrap___bid64_signaling_greater
//#define __wrap___bid64_signaling_greater_equal
//#define __wrap___bid64_signaling_greater_unordered
//#define __wrap___bid64_signaling_less
//#define __wrap___bid64_signaling_less_equal
//#define __wrap___bid64_signaling_less_unordered
//#define __wrap___bid64_signaling_not_greater
//#define __wrap___bid64_signaling_not_less
//#define __wrap___bid64_sub
//#define __wrap___bid64_add
//#define __wrap___bid32_to_uint8_rnint
//#define __wrap___bid32_to_uint8_xrnint
//#define __wrap___bid32_to_uint8_rninta
//#define __wrap___bid32_to_uint8_xrninta
//#define __wrap___bid32_to_uint8_int
//#define __wrap___bid32_to_uint8_xint
//#define __wrap___bid32_to_uint8_floor
//#define __wrap___bid32_to_uint8_ceil
//#define __wrap___bid32_to_uint8_xfloor
//#define __wrap___bid32_to_uint8_xceil
//#define __wrap___bid32_to_uint64_rnint
//#define __wrap___bid32_to_uint64_xrnint
//#define __wrap___bid32_to_uint64_floor
//#define __wrap___bid32_to_uint64_xfloor
//#define __wrap___bid32_to_uint64_ceil
//#define __wrap___bid32_to_uint64_xceil
//#define __wrap___bid32_to_uint64_int
//#define __wrap___bid32_to_uint64_xint
//#define __wrap___bid32_to_uint64_rninta
//#define __wrap___bid32_to_uint64_xrninta
//#define __wrap___bid32_to_uint32_rnint
//#define __wrap___bid32_to_uint32_xrnint
//#define __wrap___bid32_to_uint32_floor
//#define __wrap___bid32_to_uint32_xfloor
//#define __wrap___bid32_to_uint32_ceil
//#define __wrap___bid32_to_uint32_xceil
//#define __wrap___bid32_to_uint32_int
//#define __wrap___bid32_to_uint32_xint
//#define __wrap___bid32_to_uint32_rninta
//#define __wrap___bid32_to_uint32_xrninta
//#define __wrap___bid32_to_uint16_rnint
//#define __wrap___bid32_to_uint16_xrnint
//#define __wrap___bid32_to_uint16_rninta
//#define __wrap___bid32_to_uint16_xrninta
//#define __wrap___bid32_to_uint16_int
//#define __wrap___bid32_to_uint16_xint
//#define __wrap___bid32_to_uint16_floor
//#define __wrap___bid32_to_uint16_ceil
//#define __wrap___bid32_to_uint16_xfloor
//#define __wrap___bid32_to_uint16_xceil
//#define __wrap___bid32_to_int8_rnint
//#define __wrap___bid32_to_int8_xrnint
//#define __wrap___bid32_to_int8_rninta
//#define __wrap___bid32_to_int8_xrninta
//#define __wrap___bid32_to_int8_int
//#define __wrap___bid32_to_int8_xint
//#define __wrap___bid32_to_int8_floor
//#define __wrap___bid32_to_int8_ceil
//#define __wrap___bid32_to_int8_xfloor
//#define __wrap___bid32_to_int8_xceil
#define __wrap___bid32_to_int64_rnint    m(decimal32_to_int64_rnint)
//#define __wrap___bid32_to_int64_xrnint
//#define __wrap___bid32_to_int64_floor
//#define __wrap___bid32_to_int64_xfloor
//#define __wrap___bid32_to_int64_ceil
//#define __wrap___bid32_to_int64_xceil
//#define __wrap___bid32_to_int64_int
//#define __wrap___bid32_to_int64_xint
//#define __wrap___bid32_to_int64_rninta
//#define __wrap___bid32_to_int64_xrninta
//#define __wrap___bid32_to_int32_rnint
//#define __wrap___bid32_to_int32_xrnint
//#define __wrap___bid32_to_int32_floor
//#define __wrap___bid32_to_int32_xfloor
//#define __wrap___bid32_to_int32_ceil
//#define __wrap___bid32_to_int32_xceil
//#define __wrap___bid32_to_int32_int
//#define __wrap___bid32_to_int32_xint
//#define __wrap___bid32_to_int32_rninta
//#define __wrap___bid32_to_int32_xrninta
//#define __wrap___bid32_to_int16_rnint
//#define __wrap___bid32_to_int16_xrnint
//#define __wrap___bid32_to_int16_rninta
//#define __wrap___bid32_to_int16_xrninta
//#define __wrap___bid32_to_int16_int
//#define __wrap___bid32_to_int16_xint
//#define __wrap___bid32_to_int16_floor
//#define __wrap___bid32_to_int16_ceil
//#define __wrap___bid32_to_int16_xfloor
//#define __wrap___bid32_to_int16_xceil
#define __wrap___bid32_to_string     m(decimal32_to_string)
#define __wrap___bid32_from_string   m(string_to_decimal32)
#define __wrap___bid32_sqrt     m(sqrtd32)
#define __wrap___bid32_scalbln  m(scalblnd32)
#define __wrap___bid32_scalbn   m(scalbnd32)
#define __wrap___bid32_round_integral_exact        m(rintd32)
//#define __wrap___bid32_round_integral_nearest_even
#define __wrap___bid32_round_integral_negative     m(floord32)
#define __wrap___bid32_round_integral_positive     m(ceild32)
#define __wrap___bid32_round_integral_zero         m(truncd32)
#define __wrap___bid32_round_integral_nearest_away m(roundd32)
#define __wrap___bid32_rem       m(remainderd32)
#define __wrap___bid32_quantize  m(quantized32)
#define __wrap___bid32_quantexp  m(quantexpd32)
#define __wrap___bid32_isSigned    m(signbitd32)
#define __wrap___bid32_isNormal    m(isnormald32)
//#define __wrap___bid32_isSubnormal
#define __wrap___bid32_isZero   m(iszerod32)
#define __wrap___bid32_isInf    m(isinfd32)
//#define __wrap___bid32_isSignaling
//#define __wrap___bid32_isCanonical
#define __wrap___bid32_isNaN    m(isnand32)
//#define __wrap___bid32_copy
#define __wrap___bid32_negate   m(negated32)
#define __wrap___bid32_abs       m(fabsd32)
#define __wrap___bid32_copySign  m(copysignd32)
#define __wrap___bid32_class    m(fpclassifyd32)
//#define __wrap___bid32_sameQuantum
//#define __wrap___bid32_totalOrder
//#define __wrap___bid32_totalOrderMag
//#define __wrap___bid32_radix
#define __wrap___bid32_nan        m(nand32)
#define __wrap___bid64_nan        m(nand64)
#define __wrap___bid128_nan       m(nand128)
#define __wrap___bid32_isFinite    m(isfinited32)
#define __wrap___bid32_nexttoward     m(nexttoward32)
#define __wrap___bid32_nextup        m(nextupd32)
#define __wrap___bid32_nextdown      m(nextdownd32)
#define __wrap___bid32_nextafter     m(nextafterd32)
#define __wrap___bid32_nearbyint     m(nearbyintd32)
//#define __wrap___bid32_mul
#define __wrap___bid32_modf   m(modfd32)
#define __wrap___bid32_minnum     m(fmind32)
//#define __wrap___bid32_minnum_mag
#define __wrap___bid32_maxnum     m(fmaxd32)
//#define __wrap___bid32_maxnum_mag
#define __wrap___bid32_lround m(lroundd32)
#define __wrap___bid32_lrint  m(lrintd32)
#define __wrap___bid32_logb   m(logbd32)
#define __wrap___bid32_ilogb  m(ilogbd32)
#define __wrap___bid32_llrint m(llrintd32)
#define __wrap___bid32_ldexp  m(ldexpd32)
#define __wrap___bid32_frexp  m(frexpd32)
#define __wrap___bid32_fmod   m(fmodd32)
#define __wrap___bid32_fma    m(fmad32)
#define __wrap___bid32_fdim   m(fdimd32)
//#define __wrap___bid32_div
#define __wrap___bid32_quiet_equal    m(isequald32)
#define __wrap___bid32_quiet_greater    m(isgreaterd32)
#define __wrap___bid32_quiet_greater_equal    m(isgreaterequald32)
//#define __wrap___bid32_quiet_greater_unordered
#define __wrap___bid32_quiet_less       m(islessd32)
#define __wrap___bid32_quiet_less_equal    m(islessequald32)
//#define __wrap___bid32_quiet_less_unordered
#define __wrap___bid32_quiet_not_equal     m(isnotequald32)
//#define __wrap___bid32_quiet_not_greater
//#define __wrap___bid32_quiet_not_less
//#define __wrap___bid32_quiet_ordered
#define __wrap___bid32_quiet_unordered    m(isunorderedd32)
//#define __wrap___bid32_signaling_greater
//#define __wrap___bid32_signaling_greater_equal
//#define __wrap___bid32_signaling_greater_unordered
//#define __wrap___bid32_signaling_less
//#define __wrap___bid32_signaling_less_equal
//#define __wrap___bid32_signaling_less_unordered
//#define __wrap___bid32_signaling_not_greater
//#define __wrap___bid32_signaling_not_less
//#define __wrap___bid32_add
#define __wrap___bid128_tgamma  m(tgammad128)
#define __wrap___bid128_tanh  m(tanhd128)
#define __wrap___bid128_tan   m(tand128)
#define __wrap___bid128_sinh  m(sinhd128)
#define __wrap___bid128_sin   m(sind128)
#define __wrap___bid128_pow   m(powd128)
#define __wrap___bid128_log2   m(log2d128)
#define __wrap___bid128_log1p  m(log1pd128)
#define __wrap___bid128_log10  m(log10d128)
#define __wrap___bid128_log    m(logd128)
#define __wrap___bid128_lgamma m(lgammad128)
#define __wrap___bid128_hypot  m(hypotd128)
#define __wrap___bid128_expm1  m(expm1d128)
#define __wrap___bid128_exp2   m(exp2d128)
#define __wrap___bid128_exp10  m(exp10d128)
#define __wrap___bid128_exp    m(expd128)
#define __wrap___bid128_erfc   m(erfcd128)
#define __wrap___bid128_erf    m(erfd128)
#define __wrap___bid128_cosh  m(coshd128)
#define __wrap___bid128_cos   m(cosd128)
#define __wrap___bid128_cbrt  m(cbrtd128)
#define __wrap___bid128_atanh  m(atanhd128)
#define __wrap___bid128_atan2  m(atan2d128)
#define __wrap___bid128_atan   m(atand128)
#define __wrap___bid128_asinh  m(asinhd128)
#define __wrap___bid128_asin   m(asind128)
#define __wrap___bid128_acosh  m(acoshd128)
#define __wrap___bid128_acos   m(acosd128)
#define __wrap___bid64_tgamma  m(tgammad64)
#define __wrap___bid64_tanh  m(tanhd64)
#define __wrap___bid64_tan   m(tand64)
#define __wrap___bid64_sinh  m(sinhd64)
#define __wrap___bid64_sin   m(sind64)
#define __wrap___bid64_pow   m(powd64)
#define __wrap___bid64_log2   m(log2d64)
#define __wrap___bid64_log1p  m(log1pd64)
#define __wrap___bid64_log10  m(log10d64)
#define __wrap___bid64_log    m(logd64)
#define __wrap___bid64_lgamma m(lgammad64)
#define __wrap___bid64_hypot  m(hypotd64)
#define __wrap___bid64_expm1  m(expm1d64)
#define __wrap___bid64_exp2   m(exp2d64)
#define __wrap___bid64_exp10  m(exp10d64)
#define __wrap___bid64_exp    m(expd64)
#define __wrap___bid64_erfc   m(erfcd64)
#define __wrap___bid64_erf    m(erfd64)
#define __wrap___bid64_cosh  m(coshd64)
#define __wrap___bid64_cos   m(cosd64)
#define __wrap___bid64_cbrt  m(cbrtd64)
#define __wrap___bid64_atanh  m(atanhd64)
#define __wrap___bid64_atan2  m(atan2d64)
#define __wrap___bid64_atan   m(atand64)
#define __wrap___bid64_asinh  m(asinhd64)
#define __wrap___bid64_asin   m(asind64)
#define __wrap___bid64_acosh   m(acoshd64)
#define __wrap___bid64_acos    m(acosd64)



#include "dfp754.h"

