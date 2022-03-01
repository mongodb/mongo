/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jsmath_h
#define jsmath_h

#include "mozilla/MemoryReporting.h"

#include <stdint.h>

#include "NamespaceImports.h"

namespace js {

using UnaryMathFunctionType = double (*)(double);

// Used for inlining calls to double => double Math functions from JIT code.
// Note that this list does not include all unary Math functions: abs and sqrt
// for example are missing because the JITs optimize them without a C++ call.
enum class UnaryMathFunction : uint8_t {
  Log,
  Sin,
  Cos,
  Exp,
  Tan,
  ACos,
  ASin,
  ATan,
  Log10,
  Log2,
  Log1P,
  ExpM1,
  CosH,
  SinH,
  TanH,
  ACosH,
  ASinH,
  ATanH,
  Trunc,
  Cbrt,
  Floor,
  Ceil,
  Round,
};

extern UnaryMathFunctionType GetUnaryMathFunctionPtr(UnaryMathFunction fun);
extern const char* GetUnaryMathFunctionName(UnaryMathFunction fun);

/*
 * JS math functions.
 */

extern const JSClass MathClass;

extern uint64_t GenerateRandomSeed();

// Fill |seed[0]| and |seed[1]| with random bits, suitable for
// seeding a XorShift128+ random number generator.
extern void GenerateXorShift128PlusSeed(mozilla::Array<uint64_t, 2>& seed);

extern double math_random_impl(JSContext* cx);

extern bool math_random(JSContext* cx, unsigned argc, js::Value* vp);

extern bool math_abs_handle(JSContext* cx, js::HandleValue v,
                            js::MutableHandleValue r);

extern bool math_abs(JSContext* cx, unsigned argc, js::Value* vp);

extern double math_max_impl(double x, double y);

extern bool math_max(JSContext* cx, unsigned argc, js::Value* vp);

extern double math_min_impl(double x, double y);

extern bool math_min(JSContext* cx, unsigned argc, js::Value* vp);

extern double math_sqrt_impl(double x);

extern bool math_sqrt_handle(JSContext* cx, js::HandleValue number,
                             js::MutableHandleValue result);

extern bool math_sqrt(JSContext* cx, unsigned argc, js::Value* vp);

extern bool math_pow(JSContext* cx, unsigned argc, js::Value* vp);

extern bool minmax_impl(JSContext* cx, bool max, js::HandleValue a,
                        js::HandleValue b, js::MutableHandleValue res);

extern bool math_imul_handle(JSContext* cx, HandleValue lhs, HandleValue rhs,
                             MutableHandleValue res);

extern bool math_imul(JSContext* cx, unsigned argc, js::Value* vp);

extern bool RoundFloat32(JSContext* cx, HandleValue v, float* out);

extern bool RoundFloat32(JSContext* cx, HandleValue arg,
                         MutableHandleValue res);

extern bool math_fround(JSContext* cx, unsigned argc, js::Value* vp);

extern bool math_log(JSContext* cx, unsigned argc, js::Value* vp);

extern double math_log_impl(double x);

extern bool math_log_handle(JSContext* cx, HandleValue val,
                            MutableHandleValue res);

extern bool math_use_fdlibm_for_sin_cos_tan();

extern bool math_sin(JSContext* cx, unsigned argc, js::Value* vp);

extern double math_sin_fdlibm_impl(double x);
extern double math_sin_native_impl(double x);

extern bool math_sin_handle(JSContext* cx, HandleValue val,
                            MutableHandleValue res);

extern bool math_cos(JSContext* cx, unsigned argc, js::Value* vp);

extern double math_cos_fdlibm_impl(double x);
extern double math_cos_native_impl(double x);

extern bool math_exp(JSContext* cx, unsigned argc, js::Value* vp);

extern double math_exp_impl(double x);

extern bool math_tan(JSContext* cx, unsigned argc, js::Value* vp);

extern double math_tan_fdlibm_impl(double x);
extern double math_tan_native_impl(double x);

extern bool math_log10(JSContext* cx, unsigned argc, js::Value* vp);

extern bool math_log2(JSContext* cx, unsigned argc, js::Value* vp);

extern bool math_log1p(JSContext* cx, unsigned argc, js::Value* vp);

extern bool math_expm1(JSContext* cx, unsigned argc, js::Value* vp);

extern bool math_cosh(JSContext* cx, unsigned argc, js::Value* vp);

extern bool math_sinh(JSContext* cx, unsigned argc, js::Value* vp);

extern bool math_tanh(JSContext* cx, unsigned argc, js::Value* vp);

extern bool math_acosh(JSContext* cx, unsigned argc, js::Value* vp);

extern bool math_asinh(JSContext* cx, unsigned argc, js::Value* vp);

extern bool math_atanh(JSContext* cx, unsigned argc, js::Value* vp);

extern double ecmaHypot(double x, double y);

extern double hypot3(double x, double y, double z);

extern double hypot4(double x, double y, double z, double w);

extern bool math_hypot(JSContext* cx, unsigned argc, Value* vp);

extern bool math_hypot_handle(JSContext* cx, HandleValueArray args,
                              MutableHandleValue res);

extern bool math_trunc(JSContext* cx, unsigned argc, Value* vp);

extern bool math_sign(JSContext* cx, unsigned argc, Value* vp);

extern bool math_cbrt(JSContext* cx, unsigned argc, Value* vp);

extern bool math_asin(JSContext* cx, unsigned argc, Value* vp);

extern bool math_acos(JSContext* cx, unsigned argc, Value* vp);

extern bool math_atan(JSContext* cx, unsigned argc, Value* vp);

extern bool math_atan2_handle(JSContext* cx, HandleValue y, HandleValue x,
                              MutableHandleValue res);

extern bool math_atan2(JSContext* cx, unsigned argc, Value* vp);

extern double ecmaAtan2(double x, double y);

extern double math_atan_impl(double x);

extern bool math_atan(JSContext* cx, unsigned argc, js::Value* vp);

extern double math_asin_impl(double x);

extern bool math_asin(JSContext* cx, unsigned argc, js::Value* vp);

extern double math_acos_impl(double x);

extern bool math_acos(JSContext* cx, unsigned argc, js::Value* vp);

extern bool math_ceil_handle(JSContext* cx, HandleValue value,
                             MutableHandleValue res);

extern bool math_ceil(JSContext* cx, unsigned argc, Value* vp);

extern double math_ceil_impl(double x);

extern bool math_clz32(JSContext* cx, unsigned argc, Value* vp);

extern bool math_floor_handle(JSContext* cx, HandleValue v,
                              MutableHandleValue r);

extern bool math_floor(JSContext* cx, unsigned argc, Value* vp);

extern double math_floor_impl(double x);

template <typename T>
extern T GetBiggestNumberLessThan(T x);

extern bool math_round_handle(JSContext* cx, HandleValue arg,
                              MutableHandleValue res);

extern bool math_round(JSContext* cx, unsigned argc, Value* vp);

extern double math_round_impl(double x);

extern float math_roundf_impl(float x);

extern double powi(double x, int32_t y);

extern double ecmaPow(double x, double y);

extern double math_log10_impl(double x);

extern double math_log2_impl(double x);

extern double math_log1p_impl(double x);

extern double math_expm1_impl(double x);

extern double math_cosh_impl(double x);

extern double math_sinh_impl(double x);

extern double math_tanh_impl(double x);

extern double math_acosh_impl(double x);

extern double math_asinh_impl(double x);

extern double math_atanh_impl(double x);

extern double math_trunc_impl(double x);

extern float math_truncf_impl(float x);

extern bool math_trunc_handle(JSContext* cx, HandleValue v,
                              MutableHandleValue r);

extern double math_sign_impl(double x);

extern bool math_sign_handle(JSContext* cx, HandleValue v,
                             MutableHandleValue r);

extern double math_cbrt_impl(double x);

} /* namespace js */

#endif /* jsmath_h */
