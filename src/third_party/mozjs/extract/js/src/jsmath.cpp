/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * JS math package.
 */

#include "jsmath.h"

#include "mozilla/CheckedInt.h"
#include "mozilla/FloatingPoint.h"
#include "mozilla/MathAlgorithms.h"
#include "mozilla/RandomNum.h"
#include "mozilla/WrappingOperations.h"

#include <cmath>

#include "fdlibm.h"
#include "jsapi.h"
#include "jstypes.h"

#include "jit/InlinableNatives.h"
#include "js/Class.h"
#include "js/Prefs.h"
#include "js/PropertySpec.h"
#include "util/DifferentialTesting.h"
#include "vm/Float16.h"
#include "vm/JSContext.h"
#include "vm/Realm.h"
#include "vm/Time.h"

#include "vm/JSObject-inl.h"

using namespace js;

using JS::GenericNaN;
using JS::ToNumber;
using mozilla::ExponentComponent;
using mozilla::FloatingPoint;
using mozilla::IsNegative;
using mozilla::IsNegativeZero;
using mozilla::Maybe;
using mozilla::NegativeInfinity;
using mozilla::NumberEqualsInt32;
using mozilla::NumberEqualsInt64;
using mozilla::PositiveInfinity;
using mozilla::WrappingMultiply;

bool js::math_use_fdlibm_for_sin_cos_tan() {
  return JS::Prefs::use_fdlibm_for_sin_cos_tan();
}

static inline bool UseFdlibmForSinCosTan(const CallArgs& args) {
  return math_use_fdlibm_for_sin_cos_tan() ||
         args.callee().nonCCWRealm()->creationOptions().alwaysUseFdlibm();
}

template <UnaryMathFunctionType F>
static bool math_function(JSContext* cx, CallArgs& args) {
  if (args.length() == 0) {
    args.rval().setNaN();
    return true;
  }

  double x;
  if (!ToNumber(cx, args[0], &x)) {
    return false;
  }

  // TODO(post-Warp): Re-evaluate if it's still necessary resp. useful to always
  // type the value as a double.

  // NB: Always stored as a double so the math function can be inlined
  // through MMathFunction.
  double z = F(x);
  args.rval().setDouble(z);
  return true;
}

double js::math_abs_impl(double x) { return mozilla::Abs(x); }

bool js::math_abs(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  if (args.length() == 0) {
    args.rval().setNaN();
    return true;
  }

  double x;
  if (!ToNumber(cx, args[0], &x)) {
    return false;
  }

  args.rval().setNumber(math_abs_impl(x));
  return true;
}

double js::math_acos_impl(double x) {
  AutoUnsafeCallWithABI unsafe;
  return fdlibm_acos(x);
}

static bool math_acos(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  return math_function<math_acos_impl>(cx, args);
}

double js::math_asin_impl(double x) {
  AutoUnsafeCallWithABI unsafe;
  return fdlibm_asin(x);
}

static bool math_asin(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  return math_function<math_asin_impl>(cx, args);
}

double js::math_atan_impl(double x) {
  AutoUnsafeCallWithABI unsafe;
  return fdlibm_atan(x);
}

static bool math_atan(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  return math_function<math_atan_impl>(cx, args);
}

double js::ecmaAtan2(double y, double x) {
  AutoUnsafeCallWithABI unsafe;
  return fdlibm_atan2(y, x);
}

static bool math_atan2(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  double y;
  if (!ToNumber(cx, args.get(0), &y)) {
    return false;
  }

  double x;
  if (!ToNumber(cx, args.get(1), &x)) {
    return false;
  }

  double z = ecmaAtan2(y, x);
  args.rval().setDouble(z);
  return true;
}

double js::math_ceil_impl(double x) {
  AutoUnsafeCallWithABI unsafe;
  return fdlibm_ceil(x);
}

static bool math_ceil(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  if (args.length() == 0) {
    args.rval().setNaN();
    return true;
  }

  double x;
  if (!ToNumber(cx, args[0], &x)) {
    return false;
  }

  args.rval().setNumber(math_ceil_impl(x));
  return true;
}

static bool math_clz32(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  if (args.length() == 0) {
    args.rval().setInt32(32);
    return true;
  }

  uint32_t n;
  if (!ToUint32(cx, args[0], &n)) {
    return false;
  }

  if (n == 0) {
    args.rval().setInt32(32);
    return true;
  }

  args.rval().setInt32(mozilla::CountLeadingZeroes32(n));
  return true;
}

double js::math_cos_fdlibm_impl(double x) {
  AutoUnsafeCallWithABI unsafe;
  return fdlibm_cos(x);
}

double js::math_cos_native_impl(double x) {
  MOZ_ASSERT(!math_use_fdlibm_for_sin_cos_tan());
  AutoUnsafeCallWithABI unsafe;
  return std::cos(x);
}

static bool math_cos(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  if (UseFdlibmForSinCosTan(args)) {
    return math_function<math_cos_fdlibm_impl>(cx, args);
  }
  return math_function<math_cos_native_impl>(cx, args);
}

double js::math_exp_impl(double x) {
  AutoUnsafeCallWithABI unsafe;
  return fdlibm_exp(x);
}

static bool math_exp(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  return math_function<math_exp_impl>(cx, args);
}

double js::math_floor_impl(double x) {
  AutoUnsafeCallWithABI unsafe;
  return fdlibm_floor(x);
}

bool js::math_floor(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  if (args.length() == 0) {
    args.rval().setNaN();
    return true;
  }

  double x;
  if (!ToNumber(cx, args[0], &x)) {
    return false;
  }

  args.rval().setNumber(math_floor_impl(x));
  return true;
}

bool js::math_imul_handle(JSContext* cx, HandleValue lhs, HandleValue rhs,
                          MutableHandleValue res) {
  int32_t a = 0, b = 0;
  if (!lhs.isUndefined() && !ToInt32(cx, lhs, &a)) {
    return false;
  }
  if (!rhs.isUndefined() && !ToInt32(cx, rhs, &b)) {
    return false;
  }

  res.setInt32(WrappingMultiply(a, b));
  return true;
}

static bool math_imul(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  return math_imul_handle(cx, args.get(0), args.get(1), args.rval());
}

// Implements Math.fround (20.2.2.16) up to step 3
bool js::RoundFloat32(JSContext* cx, HandleValue v, float* out) {
  double d;
  bool success = ToNumber(cx, v, &d);
  *out = static_cast<float>(d);
  return success;
}

bool js::RoundFloat32(JSContext* cx, HandleValue arg, MutableHandleValue res) {
  float f;
  if (!RoundFloat32(cx, arg, &f)) {
    return false;
  }

  res.setDouble(static_cast<double>(f));
  return true;
}

double js::RoundFloat32(double d) {
  return static_cast<double>(static_cast<float>(d));
}

static bool math_fround(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  if (args.length() == 0) {
    args.rval().setNaN();
    return true;
  }

  return RoundFloat32(cx, args[0], args.rval());
}

#ifdef NIGHTLY_BUILD
static bool math_f16round(JSContext* cx, unsigned argc, Value* vp) {
  // http://tc39.es/proposal-float16array/#sec-function-properties-of-the-math-object
  CallArgs args = CallArgsFromVp(argc, vp);

  if (args.length() == 0) {
    args.rval().setNaN();
    return true;
  }

  // 1. Let n be ? ToNumber(x).
  double d;
  if (!ToNumber(cx, args[0], &d)) {
    return false;
  }
  // 2. If n is NaN, return NaN.
  // 3. If n is one of +0𝔽, -0𝔽, +∞𝔽, or -∞𝔽, return n.
  // 4. Let n16 be the result of converting n to IEEE 754-2019 binary16 format
  // using roundTiesToEven mode.
  js::float16 f16 = js::float16(d);
  // 5. Let n64 be the result of converting n16 to IEEE 754-2019 binary64
  // format.
  // 6. Return the ECMAScript Number value corresponding to n64.
  args.rval().setDouble(f16.toDouble());
  return true;
}
#endif

double js::math_log_impl(double x) {
  AutoUnsafeCallWithABI unsafe;
  return fdlibm_log(x);
}

static bool math_log(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  return math_function<math_log_impl>(cx, args);
}

double js::math_max_impl(double x, double y) {
  AutoUnsafeCallWithABI unsafe;

  // Math.max(num, NaN) => NaN, Math.max(-0, +0) => +0
  if (x > y || std::isnan(x) || (x == y && IsNegative(y))) {
    return x;
  }
  return y;
}

bool js::math_max(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  double maxval = NegativeInfinity<double>();
  for (unsigned i = 0; i < args.length(); i++) {
    double x;
    if (!ToNumber(cx, args[i], &x)) {
      return false;
    }
    maxval = math_max_impl(x, maxval);
  }
  args.rval().setNumber(maxval);
  return true;
}

double js::math_min_impl(double x, double y) {
  AutoUnsafeCallWithABI unsafe;

  // Math.min(num, NaN) => NaN, Math.min(-0, +0) => -0
  if (x < y || std::isnan(x) || (x == y && IsNegativeZero(x))) {
    return x;
  }
  return y;
}

bool js::math_min(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  double minval = PositiveInfinity<double>();
  for (unsigned i = 0; i < args.length(); i++) {
    double x;
    if (!ToNumber(cx, args[i], &x)) {
      return false;
    }
    minval = math_min_impl(x, minval);
  }
  args.rval().setNumber(minval);
  return true;
}

double js::powi(double x, int32_t y) {
  AutoUnsafeCallWithABI unsafe;

  // It's only safe to optimize this when we can compute with integer values or
  // the exponent is a small, positive constant.
  if (y >= 0) {
    uint32_t n = uint32_t(y);

    // NB: Have to take fast-path for n <= 4 to match |MPow::foldsTo|. Otherwise
    // we risk causing differential testing issues.
    if (n == 0) {
      return 1;
    }
    if (n == 1) {
      return x;
    }
    if (n == 2) {
      return x * x;
    }
    if (n == 3) {
      return x * x * x;
    }
    if (n == 4) {
      double z = x * x;
      return z * z;
    }

    int64_t i;
    if (NumberEqualsInt64(x, &i)) {
      // Special-case: |-0 ** odd| is -0.
      if (i == 0) {
        return (n & 1) ? x : 0;
      }

      // Use int64 to cover cases like |Math.pow(2, 53)|.
      mozilla::CheckedInt64 runningSquare = i;
      mozilla::CheckedInt64 result = 1;
      while (true) {
        if ((n & 1) != 0) {
          result *= runningSquare;
          if (!result.isValid()) {
            break;
          }
        }

        n >>= 1;
        if (n == 0) {
          return static_cast<double>(result.value());
        }

        runningSquare *= runningSquare;
        if (!runningSquare.isValid()) {
          break;
        }
      }
    }

    // Fall-back to use std::pow to reduce floating point precision errors.
  }

  return std::pow(x, static_cast<double>(y));  // Avoid pow(double, int).
}

double js::ecmaPow(double x, double y) {
  AutoUnsafeCallWithABI unsafe;

  /*
   * Use powi if the exponent is an integer-valued double. We don't have to
   * check for NaN since a comparison with NaN is always false.
   */
  int32_t yi;
  if (NumberEqualsInt32(y, &yi)) {
    return powi(x, yi);
  }

  /*
   * Because C99 and ECMA specify different behavior for pow(),
   * we need to wrap the libm call to make it ECMA compliant.
   */
  if (!std::isfinite(y) && (x == 1.0 || x == -1.0)) {
    return GenericNaN();
  }

  /* pow(x, +-0) is always 1, even for x = NaN (MSVC gets this wrong). */
  if (y == 0) {
    return 1;
  }

  /*
   * Special case for square roots. Note that pow(x, 0.5) != sqrt(x)
   * when x = -0.0, so we have to guard for this.
   */
  if (std::isfinite(x) && x != 0.0) {
    if (y == 0.5) {
      return std::sqrt(x);
    }
    if (y == -0.5) {
      return 1.0 / std::sqrt(x);
    }
  }
  return std::pow(x, y);
}

static bool math_pow(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  double x;
  if (!ToNumber(cx, args.get(0), &x)) {
    return false;
  }

  double y;
  if (!ToNumber(cx, args.get(1), &y)) {
    return false;
  }

  double z = ecmaPow(x, y);
  args.rval().setNumber(z);
  return true;
}

uint64_t js::GenerateRandomSeed() {
  Maybe<uint64_t> maybeSeed = mozilla::RandomUint64();

  return maybeSeed.valueOrFrom([] {
    // Use PRMJ_Now() in case we couldn't read random bits from the OS.
    uint64_t timestamp = PRMJ_Now();
    return timestamp ^ (timestamp << 32);
  });
}

void js::GenerateXorShift128PlusSeed(mozilla::Array<uint64_t, 2>& seed) {
  // XorShift128PlusRNG must be initialized with a non-zero seed.
  do {
    seed[0] = GenerateRandomSeed();
    seed[1] = GenerateRandomSeed();
  } while (seed[0] == 0 && seed[1] == 0);
}

mozilla::non_crypto::XorShift128PlusRNG&
Realm::getOrCreateRandomNumberGenerator() {
  if (randomNumberGenerator_.isNothing()) {
    mozilla::Array<uint64_t, 2> seed;
    GenerateXorShift128PlusSeed(seed);
    randomNumberGenerator_.emplace(seed[0], seed[1]);
  }

  return randomNumberGenerator_.ref();
}

double js::math_random_impl(JSContext* cx) {
  return cx->realm()->getOrCreateRandomNumberGenerator().nextDouble();
}

static bool math_random(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  if (js::SupportDifferentialTesting()) {
    args.rval().setDouble(0);
  } else {
    args.rval().setDouble(math_random_impl(cx));
  }
  return true;
}

template <typename T>
T js::GetBiggestNumberLessThan(T x) {
  MOZ_ASSERT(!IsNegative(x));
  MOZ_ASSERT(std::isfinite(x));
  using Bits = typename mozilla::FloatingPoint<T>::Bits;
  Bits bits = mozilla::BitwiseCast<Bits>(x);
  MOZ_ASSERT(bits > 0, "will underflow");
  return mozilla::BitwiseCast<T>(bits - 1);
}

template double js::GetBiggestNumberLessThan<>(double x);
template float js::GetBiggestNumberLessThan<>(float x);

double js::math_round_impl(double x) {
  AutoUnsafeCallWithABI unsafe;

  int32_t ignored;
  if (NumberEqualsInt32(x, &ignored)) {
    return x;
  }

  /* Some numbers are so big that adding 0.5 would give the wrong number. */
  if (ExponentComponent(x) >=
      int_fast16_t(FloatingPoint<double>::kExponentShift)) {
    return x;
  }

  double add = (x >= 0) ? GetBiggestNumberLessThan(0.5) : 0.5;
  return std::copysign(fdlibm_floor(x + add), x);
}

float js::math_roundf_impl(float x) {
  AutoUnsafeCallWithABI unsafe;

  int32_t ignored;
  if (NumberEqualsInt32(x, &ignored)) {
    return x;
  }

  /* Some numbers are so big that adding 0.5 would give the wrong number. */
  if (ExponentComponent(x) >=
      int_fast16_t(FloatingPoint<float>::kExponentShift)) {
    return x;
  }

  float add = (x >= 0) ? GetBiggestNumberLessThan(0.5f) : 0.5f;
  return std::copysign(fdlibm_floorf(x + add), x);
}

/* ES5 15.8.2.15. */
static bool math_round(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  if (args.length() == 0) {
    args.rval().setNaN();
    return true;
  }

  double x;
  if (!ToNumber(cx, args[0], &x)) {
    return false;
  }

  args.rval().setNumber(math_round_impl(x));
  return true;
}

double js::math_sin_fdlibm_impl(double x) {
  AutoUnsafeCallWithABI unsafe;
  return fdlibm_sin(x);
}

double js::math_sin_native_impl(double x) {
  MOZ_ASSERT(!math_use_fdlibm_for_sin_cos_tan());
  AutoUnsafeCallWithABI unsafe;
  return std::sin(x);
}

static bool math_sin(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  if (UseFdlibmForSinCosTan(args)) {
    return math_function<math_sin_fdlibm_impl>(cx, args);
  }
  return math_function<math_sin_native_impl>(cx, args);
}

double js::math_sqrt_impl(double x) {
  AutoUnsafeCallWithABI unsafe;
  return std::sqrt(x);
}

static bool math_sqrt(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  return math_function<math_sqrt_impl>(cx, args);
}

double js::math_tan_fdlibm_impl(double x) {
  AutoUnsafeCallWithABI unsafe;
  return fdlibm_tan(x);
}

double js::math_tan_native_impl(double x) {
  MOZ_ASSERT(!math_use_fdlibm_for_sin_cos_tan());
  AutoUnsafeCallWithABI unsafe;
  return std::tan(x);
}

static bool math_tan(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  if (UseFdlibmForSinCosTan(args)) {
    return math_function<math_tan_fdlibm_impl>(cx, args);
  }
  return math_function<math_tan_native_impl>(cx, args);
}

double js::math_log10_impl(double x) {
  AutoUnsafeCallWithABI unsafe;
  return fdlibm_log10(x);
}

static bool math_log10(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  return math_function<math_log10_impl>(cx, args);
}

double js::math_log2_impl(double x) {
  AutoUnsafeCallWithABI unsafe;
  return fdlibm_log2(x);
}

static bool math_log2(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  return math_function<math_log2_impl>(cx, args);
}

double js::math_log1p_impl(double x) {
  AutoUnsafeCallWithABI unsafe;
  return fdlibm_log1p(x);
}

static bool math_log1p(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  return math_function<math_log1p_impl>(cx, args);
}

double js::math_expm1_impl(double x) {
  AutoUnsafeCallWithABI unsafe;
  return fdlibm_expm1(x);
}

static bool math_expm1(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  return math_function<math_expm1_impl>(cx, args);
}

double js::math_cosh_impl(double x) {
  AutoUnsafeCallWithABI unsafe;
  return fdlibm_cosh(x);
}

static bool math_cosh(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  return math_function<math_cosh_impl>(cx, args);
}

double js::math_sinh_impl(double x) {
  AutoUnsafeCallWithABI unsafe;
  return fdlibm_sinh(x);
}

static bool math_sinh(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  return math_function<math_sinh_impl>(cx, args);
}

double js::math_tanh_impl(double x) {
  AutoUnsafeCallWithABI unsafe;
  return fdlibm_tanh(x);
}

static bool math_tanh(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  return math_function<math_tanh_impl>(cx, args);
}

double js::math_acosh_impl(double x) {
  AutoUnsafeCallWithABI unsafe;
  return fdlibm_acosh(x);
}

static bool math_acosh(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  return math_function<math_acosh_impl>(cx, args);
}

double js::math_asinh_impl(double x) {
  AutoUnsafeCallWithABI unsafe;
  return fdlibm_asinh(x);
}

static bool math_asinh(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  return math_function<math_asinh_impl>(cx, args);
}

double js::math_atanh_impl(double x) {
  AutoUnsafeCallWithABI unsafe;
  return fdlibm_atanh(x);
}

static bool math_atanh(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  return math_function<math_atanh_impl>(cx, args);
}

double js::ecmaHypot(double x, double y) {
  AutoUnsafeCallWithABI unsafe;
  return fdlibm_hypot(x, y);
}

static inline void hypot_step(double& scale, double& sumsq, double x) {
  double xabs = mozilla::Abs(x);
  if (scale < xabs) {
    sumsq = 1 + sumsq * (scale / xabs) * (scale / xabs);
    scale = xabs;
  } else if (scale != 0) {
    sumsq += (xabs / scale) * (xabs / scale);
  }
}

double js::hypot4(double x, double y, double z, double w) {
  AutoUnsafeCallWithABI unsafe;

  // Check for infinities or NaNs so that we can return immediately.
  if (std::isinf(x) || std::isinf(y) || std::isinf(z) || std::isinf(w)) {
    return mozilla::PositiveInfinity<double>();
  }

  if (std::isnan(x) || std::isnan(y) || std::isnan(z) || std::isnan(w)) {
    return GenericNaN();
  }

  double scale = 0;
  double sumsq = 1;

  hypot_step(scale, sumsq, x);
  hypot_step(scale, sumsq, y);
  hypot_step(scale, sumsq, z);
  hypot_step(scale, sumsq, w);

  return scale * std::sqrt(sumsq);
}

double js::hypot3(double x, double y, double z) {
  AutoUnsafeCallWithABI unsafe;
  return hypot4(x, y, z, 0.0);
}

static bool math_hypot(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  return math_hypot_handle(cx, args, args.rval());
}

bool js::math_hypot_handle(JSContext* cx, HandleValueArray args,
                           MutableHandleValue res) {
  // IonMonkey calls the ecmaHypot function directly if two arguments are
  // given. Do that here as well to get the same results.
  if (args.length() == 2) {
    double x, y;
    if (!ToNumber(cx, args[0], &x)) {
      return false;
    }
    if (!ToNumber(cx, args[1], &y)) {
      return false;
    }

    double result = ecmaHypot(x, y);
    res.setDouble(result);
    return true;
  }

  bool isInfinite = false;
  bool isNaN = false;

  double scale = 0;
  double sumsq = 1;

  for (unsigned i = 0; i < args.length(); i++) {
    double x;
    if (!ToNumber(cx, args[i], &x)) {
      return false;
    }

    isInfinite |= std::isinf(x);
    isNaN |= std::isnan(x);
    if (isInfinite || isNaN) {
      continue;
    }

    hypot_step(scale, sumsq, x);
  }

  double result = isInfinite ? PositiveInfinity<double>()
                  : isNaN    ? GenericNaN()
                             : scale * std::sqrt(sumsq);
  res.setDouble(result);
  return true;
}

double js::math_trunc_impl(double x) {
  AutoUnsafeCallWithABI unsafe;
  return fdlibm_trunc(x);
}

float js::math_truncf_impl(float x) {
  AutoUnsafeCallWithABI unsafe;
  return fdlibm_truncf(x);
}

bool js::math_trunc(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  if (args.length() == 0) {
    args.rval().setNaN();
    return true;
  }

  double x;
  if (!ToNumber(cx, args[0], &x)) {
    return false;
  }

  args.rval().setNumber(math_trunc_impl(x));
  return true;
}

double js::math_sign_impl(double x) {
  AutoUnsafeCallWithABI unsafe;

  if (std::isnan(x)) {
    return GenericNaN();
  }

  return x == 0 ? x : x < 0 ? -1 : 1;
}

static bool math_sign(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  if (args.length() == 0) {
    args.rval().setNaN();
    return true;
  }

  double x;
  if (!ToNumber(cx, args[0], &x)) {
    return false;
  }

  args.rval().setNumber(math_sign_impl(x));
  return true;
}

double js::math_cbrt_impl(double x) {
  AutoUnsafeCallWithABI unsafe;
  return fdlibm_cbrt(x);
}

static bool math_cbrt(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  return math_function<math_cbrt_impl>(cx, args);
}

static bool math_toSource(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  args.rval().setString(cx->names().Math);
  return true;
}

UnaryMathFunctionType js::GetUnaryMathFunctionPtr(UnaryMathFunction fun) {
  switch (fun) {
    case UnaryMathFunction::SinNative:
      return math_sin_native_impl;
    case UnaryMathFunction::SinFdlibm:
      return math_sin_fdlibm_impl;
    case UnaryMathFunction::CosNative:
      return math_cos_native_impl;
    case UnaryMathFunction::CosFdlibm:
      return math_cos_fdlibm_impl;
    case UnaryMathFunction::TanNative:
      return math_tan_native_impl;
    case UnaryMathFunction::TanFdlibm:
      return math_tan_fdlibm_impl;
    case UnaryMathFunction::Log:
      return math_log_impl;
    case UnaryMathFunction::Exp:
      return math_exp_impl;
    case UnaryMathFunction::ATan:
      return math_atan_impl;
    case UnaryMathFunction::ASin:
      return math_asin_impl;
    case UnaryMathFunction::ACos:
      return math_acos_impl;
    case UnaryMathFunction::Log10:
      return math_log10_impl;
    case UnaryMathFunction::Log2:
      return math_log2_impl;
    case UnaryMathFunction::Log1P:
      return math_log1p_impl;
    case UnaryMathFunction::ExpM1:
      return math_expm1_impl;
    case UnaryMathFunction::CosH:
      return math_cosh_impl;
    case UnaryMathFunction::SinH:
      return math_sinh_impl;
    case UnaryMathFunction::TanH:
      return math_tanh_impl;
    case UnaryMathFunction::ACosH:
      return math_acosh_impl;
    case UnaryMathFunction::ASinH:
      return math_asinh_impl;
    case UnaryMathFunction::ATanH:
      return math_atanh_impl;
    case UnaryMathFunction::Trunc:
      return math_trunc_impl;
    case UnaryMathFunction::Cbrt:
      return math_cbrt_impl;
    case UnaryMathFunction::Floor:
      return math_floor_impl;
    case UnaryMathFunction::Ceil:
      return math_ceil_impl;
    case UnaryMathFunction::Round:
      return math_round_impl;
  }
  MOZ_CRASH("Unknown function");
}

const char* js::GetUnaryMathFunctionName(UnaryMathFunction fun) {
  switch (fun) {
    case UnaryMathFunction::SinNative:
      return "Sin (native)";
    case UnaryMathFunction::SinFdlibm:
      return "Sin (fdlibm)";
    case UnaryMathFunction::CosNative:
      return "Cos (native)";
    case UnaryMathFunction::CosFdlibm:
      return "Cos (fdlibm)";
    case UnaryMathFunction::TanNative:
      return "Tan (native)";
    case UnaryMathFunction::TanFdlibm:
      return "Tan (fdlibm)";
    case UnaryMathFunction::Log:
      return "Log";
    case UnaryMathFunction::Exp:
      return "Exp";
    case UnaryMathFunction::ACos:
      return "ACos";
    case UnaryMathFunction::ASin:
      return "ASin";
    case UnaryMathFunction::ATan:
      return "ATan";
    case UnaryMathFunction::Log10:
      return "Log10";
    case UnaryMathFunction::Log2:
      return "Log2";
    case UnaryMathFunction::Log1P:
      return "Log1P";
    case UnaryMathFunction::ExpM1:
      return "ExpM1";
    case UnaryMathFunction::CosH:
      return "CosH";
    case UnaryMathFunction::SinH:
      return "SinH";
    case UnaryMathFunction::TanH:
      return "TanH";
    case UnaryMathFunction::ACosH:
      return "ACosH";
    case UnaryMathFunction::ASinH:
      return "ASinH";
    case UnaryMathFunction::ATanH:
      return "ATanH";
    case UnaryMathFunction::Trunc:
      return "Trunc";
    case UnaryMathFunction::Cbrt:
      return "Cbrt";
    case UnaryMathFunction::Floor:
      return "Floor";
    case UnaryMathFunction::Ceil:
      return "Ceil";
    case UnaryMathFunction::Round:
      return "Round";
  }
  MOZ_CRASH("Unknown function");
}

static const JSFunctionSpec math_static_methods[] = {
    JS_FN("toSource", math_toSource, 0, 0),
    JS_INLINABLE_FN("abs", math_abs, 1, 0, MathAbs),
    JS_INLINABLE_FN("acos", math_acos, 1, 0, MathACos),
    JS_INLINABLE_FN("asin", math_asin, 1, 0, MathASin),
    JS_INLINABLE_FN("atan", math_atan, 1, 0, MathATan),
    JS_INLINABLE_FN("atan2", math_atan2, 2, 0, MathATan2),
    JS_INLINABLE_FN("ceil", math_ceil, 1, 0, MathCeil),
    JS_INLINABLE_FN("clz32", math_clz32, 1, 0, MathClz32),
    JS_INLINABLE_FN("cos", math_cos, 1, 0, MathCos),
    JS_INLINABLE_FN("exp", math_exp, 1, 0, MathExp),
    JS_INLINABLE_FN("floor", math_floor, 1, 0, MathFloor),
    JS_INLINABLE_FN("imul", math_imul, 2, 0, MathImul),
    JS_INLINABLE_FN("fround", math_fround, 1, 0, MathFRound),
#ifdef NIGHTLY_BUILD
    // TODO: See Bug 1835034 for JIT support
    JS_FN("f16round", math_f16round, 1, 0),
#endif
    JS_INLINABLE_FN("log", math_log, 1, 0, MathLog),
    JS_INLINABLE_FN("max", math_max, 2, 0, MathMax),
    JS_INLINABLE_FN("min", math_min, 2, 0, MathMin),
    JS_INLINABLE_FN("pow", math_pow, 2, 0, MathPow),
    JS_INLINABLE_FN("random", math_random, 0, 0, MathRandom),
    JS_INLINABLE_FN("round", math_round, 1, 0, MathRound),
    JS_INLINABLE_FN("sin", math_sin, 1, 0, MathSin),
    JS_INLINABLE_FN("sqrt", math_sqrt, 1, 0, MathSqrt),
    JS_INLINABLE_FN("tan", math_tan, 1, 0, MathTan),
    JS_INLINABLE_FN("log10", math_log10, 1, 0, MathLog10),
    JS_INLINABLE_FN("log2", math_log2, 1, 0, MathLog2),
    JS_INLINABLE_FN("log1p", math_log1p, 1, 0, MathLog1P),
    JS_INLINABLE_FN("expm1", math_expm1, 1, 0, MathExpM1),
    JS_INLINABLE_FN("cosh", math_cosh, 1, 0, MathCosH),
    JS_INLINABLE_FN("sinh", math_sinh, 1, 0, MathSinH),
    JS_INLINABLE_FN("tanh", math_tanh, 1, 0, MathTanH),
    JS_INLINABLE_FN("acosh", math_acosh, 1, 0, MathACosH),
    JS_INLINABLE_FN("asinh", math_asinh, 1, 0, MathASinH),
    JS_INLINABLE_FN("atanh", math_atanh, 1, 0, MathATanH),
    JS_INLINABLE_FN("hypot", math_hypot, 2, 0, MathHypot),
    JS_INLINABLE_FN("trunc", math_trunc, 1, 0, MathTrunc),
    JS_INLINABLE_FN("sign", math_sign, 1, 0, MathSign),
    JS_INLINABLE_FN("cbrt", math_cbrt, 1, 0, MathCbrt), JS_FS_END};

static const JSPropertySpec math_static_properties[] = {
    JS_DOUBLE_PS("E", M_E, JSPROP_READONLY | JSPROP_PERMANENT),
    JS_DOUBLE_PS("LOG2E", M_LOG2E, JSPROP_READONLY | JSPROP_PERMANENT),
    JS_DOUBLE_PS("LOG10E", M_LOG10E, JSPROP_READONLY | JSPROP_PERMANENT),
    JS_DOUBLE_PS("LN2", M_LN2, JSPROP_READONLY | JSPROP_PERMANENT),
    JS_DOUBLE_PS("LN10", M_LN10, JSPROP_READONLY | JSPROP_PERMANENT),
    JS_DOUBLE_PS("PI", M_PI, JSPROP_READONLY | JSPROP_PERMANENT),
    JS_DOUBLE_PS("SQRT2", M_SQRT2, JSPROP_READONLY | JSPROP_PERMANENT),
    JS_DOUBLE_PS("SQRT1_2", M_SQRT1_2, JSPROP_READONLY | JSPROP_PERMANENT),

    JS_STRING_SYM_PS(toStringTag, "Math", JSPROP_READONLY),
    JS_PS_END};

static JSObject* CreateMathObject(JSContext* cx, JSProtoKey key) {
  RootedObject proto(cx, &cx->global()->getObjectPrototype());
  return NewTenuredObjectWithGivenProto(cx, &MathClass, proto);
}

static const ClassSpec MathClassSpec = {CreateMathObject,
                                        nullptr,
                                        math_static_methods,
                                        math_static_properties,
                                        nullptr,
                                        nullptr,
                                        nullptr};

const JSClass js::MathClass = {"Math", JSCLASS_HAS_CACHED_PROTO(JSProto_Math),
                               JS_NULL_CLASS_OPS, &MathClassSpec};
