/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jsmath_h
#define jsmath_h

#include "mozilla/MemoryReporting.h"

#include <cmath>

#include "NamespaceImports.h"

namespace js {

typedef double (*UnaryFunType)(double);

class MathCache
{
  public:
    enum MathFuncId {
        Zero,
        Sin, Cos, Tan, Sinh, Cosh, Tanh, Asin, Acos, Atan, Asinh, Acosh, Atanh,
        Sqrt, Log, Log10, Log2, Log1p, Exp, Expm1, Cbrt, Trunc, Sign
    };

  private:
    static const unsigned SizeLog2 = 12;
    static const unsigned Size = 1 << SizeLog2;
    struct Entry { double in; MathFuncId id; double out; };
    Entry table[Size];

  public:
    MathCache();

    unsigned hash(double x, MathFuncId id) {
        union { double d; struct { uint32_t one, two; } s; } u = { x };
        uint32_t hash32 = u.s.one ^ u.s.two;
        hash32 += uint32_t(id) << 8;
        uint16_t hash16 = uint16_t(hash32 ^ (hash32 >> 16));
        return (hash16 & (Size - 1)) ^ (hash16 >> (16 - SizeLog2));
    }

    /*
     * N.B. lookup uses double-equality. This is only safe if hash() maps +0
     * and -0 to different table entries, which is asserted in MathCache().
     */
    double lookup(UnaryFunType f, double x, MathFuncId id) {
        unsigned index = hash(x, id);
        Entry& e = table[index];
        if (e.in == x && e.id == id)
            return e.out;
        e.in = x;
        e.id = id;
        return e.out = f(x);
    }

    bool isCached(double x, MathFuncId id, double *r, unsigned *index) {
        *index = hash(x, id);
        Entry& e = table[*index];
        if (e.in == x && e.id == id) {
            *r = e.out;
            return true;
        }
        return false;
    }

    void store(MathFuncId id, double x, double v, unsigned index) {
        Entry &e = table[index];
        if (e.in == x && e.id == id)
            return;
        e.in = x;
        e.id = id;
        e.out = v;
    }

    size_t sizeOfIncludingThis(mozilla::MallocSizeOf mallocSizeOf);
};

/*
 * JS math functions.
 */

extern JSObject*
InitMathClass(JSContext* cx, HandleObject obj);

// Fill |seed[0]| and |seed[1]| with random bits, suitable for
// seeding a XorShift128+ random number generator.
extern void
GenerateXorShift128PlusSeed(mozilla::Array<uint64_t, 2>& seed);

extern uint64_t
random_next(uint64_t* rngState, int bits);

extern bool
math_random(JSContext* cx, unsigned argc, js::Value* vp);

extern bool
math_abs_handle(JSContext* cx, js::HandleValue v, js::MutableHandleValue r);

extern bool
math_abs(JSContext* cx, unsigned argc, js::Value* vp);

extern double
math_max_impl(double x, double y);

extern bool
math_max(JSContext* cx, unsigned argc, js::Value* vp);

extern double
math_min_impl(double x, double y);

extern bool
math_min(JSContext* cx, unsigned argc, js::Value* vp);

extern bool
math_sqrt(JSContext* cx, unsigned argc, js::Value* vp);

extern bool
math_pow_handle(JSContext* cx, js::HandleValue base, js::HandleValue power,
                js::MutableHandleValue result);

extern bool
math_pow(JSContext* cx, unsigned argc, js::Value* vp);

extern bool
minmax_impl(JSContext* cx, bool max, js::HandleValue a, js::HandleValue b,
            js::MutableHandleValue res);

extern void
math_sincos_uncached(double x, double *sin, double *cos);

extern void
math_sincos_impl(MathCache* mathCache, double x, double *sin, double *cos);

extern bool
math_sqrt_handle(JSContext* cx, js::HandleValue number, js::MutableHandleValue result);

extern bool
math_imul_handle(JSContext* cx, HandleValue lhs, HandleValue rhs, MutableHandleValue res);

extern bool
math_imul(JSContext* cx, unsigned argc, js::Value* vp);

extern bool
RoundFloat32(JSContext* cx, HandleValue v, float* out);

extern bool
RoundFloat32(JSContext* cx, HandleValue arg, MutableHandleValue res);

extern bool
math_fround(JSContext* cx, unsigned argc, js::Value* vp);

extern bool
math_log(JSContext* cx, unsigned argc, js::Value* vp);

extern double
math_log_impl(MathCache* cache, double x);

extern double
math_log_uncached(double x);

extern bool
math_log_handle(JSContext* cx, HandleValue val, MutableHandleValue res);

extern bool
math_sin(JSContext* cx, unsigned argc, js::Value* vp);

extern double
math_sin_impl(MathCache* cache, double x);

extern double
math_sin_uncached(double x);

extern bool
math_sin_handle(JSContext* cx, HandleValue val, MutableHandleValue res);

extern bool
math_cos(JSContext* cx, unsigned argc, js::Value* vp);

extern double
math_cos_impl(MathCache* cache, double x);

extern double
math_cos_uncached(double x);

extern bool
math_exp(JSContext* cx, unsigned argc, js::Value* vp);

extern double
math_exp_impl(MathCache* cache, double x);

extern double
math_exp_uncached(double x);

extern bool
math_tan(JSContext* cx, unsigned argc, js::Value* vp);

extern double
math_tan_impl(MathCache* cache, double x);

extern double
math_tan_uncached(double x);

extern bool
math_log10(JSContext* cx, unsigned argc, js::Value* vp);

extern bool
math_log2(JSContext* cx, unsigned argc, js::Value* vp);

extern bool
math_log1p(JSContext* cx, unsigned argc, js::Value* vp);

extern bool
math_expm1(JSContext* cx, unsigned argc, js::Value* vp);

extern bool
math_cosh(JSContext* cx, unsigned argc, js::Value* vp);

extern bool
math_sinh(JSContext* cx, unsigned argc, js::Value* vp);

extern bool
math_tanh(JSContext* cx, unsigned argc, js::Value* vp);

extern bool
math_acosh(JSContext* cx, unsigned argc, js::Value* vp);

extern bool
math_asinh(JSContext* cx, unsigned argc, js::Value* vp);

extern bool
math_atanh(JSContext* cx, unsigned argc, js::Value* vp);

extern double
ecmaHypot(double x, double y);

extern double
hypot3(double x, double y, double z);

extern double
hypot4(double x, double y, double z, double w);

extern bool
math_hypot(JSContext* cx, unsigned argc, Value* vp);

extern bool
math_hypot_handle(JSContext* cx, HandleValueArray args, MutableHandleValue res);

extern bool
math_trunc(JSContext* cx, unsigned argc, Value* vp);

extern bool
math_sign(JSContext* cx, unsigned argc, Value* vp);

extern bool
math_cbrt(JSContext* cx, unsigned argc, Value* vp);

extern bool
math_asin(JSContext* cx, unsigned argc, Value* vp);

extern bool
math_acos(JSContext* cx, unsigned argc, Value* vp);

extern bool
math_atan(JSContext* cx, unsigned argc, Value* vp);

extern bool
math_atan2_handle(JSContext* cx, HandleValue y, HandleValue x, MutableHandleValue res);

extern bool
math_atan2(JSContext* cx, unsigned argc, Value* vp);

extern double
ecmaAtan2(double x, double y);

extern double
math_atan_impl(MathCache* cache, double x);

extern double
math_atan_uncached(double x);

extern bool
math_atan(JSContext* cx, unsigned argc, js::Value* vp);

extern double
math_asin_impl(MathCache* cache, double x);

extern double
math_asin_uncached(double x);

extern bool
math_asin(JSContext* cx, unsigned argc, js::Value* vp);

extern double
math_acos_impl(MathCache* cache, double x);

extern double
math_acos_uncached(double x);

extern bool
math_acos(JSContext* cx, unsigned argc, js::Value* vp);

extern bool
math_ceil_handle(JSContext* cx, HandleValue value, MutableHandleValue res);

extern bool
math_ceil(JSContext* cx, unsigned argc, Value* vp);

extern double
math_ceil_impl(double x);

extern bool
math_clz32(JSContext* cx, unsigned argc, Value* vp);

extern bool
math_floor_handle(JSContext* cx, HandleValue v, MutableHandleValue r);

extern bool
math_floor(JSContext* cx, unsigned argc, Value* vp);

extern double
math_floor_impl(double x);

template<typename T>
extern T GetBiggestNumberLessThan(T x);

extern bool
math_round_handle(JSContext* cx, HandleValue arg, MutableHandleValue res);

extern bool
math_round(JSContext* cx, unsigned argc, Value* vp);

extern double
math_round_impl(double x);

extern float
math_roundf_impl(float x);

extern double
powi(double x, int y);

extern double
ecmaPow(double x, double y);

extern bool
math_imul(JSContext* cx, unsigned argc, Value* vp);

extern double
math_log10_impl(MathCache* cache, double x);

extern double
math_log10_uncached(double x);

extern double
math_log2_impl(MathCache* cache, double x);

extern double
math_log2_uncached(double x);

extern double
math_log1p_impl(MathCache* cache, double x);

extern double
math_log1p_uncached(double x);

extern double
math_expm1_impl(MathCache* cache, double x);

extern double
math_expm1_uncached(double x);

extern double
math_cosh_impl(MathCache* cache, double x);

extern double
math_cosh_uncached(double x);

extern double
math_sinh_impl(MathCache* cache, double x);

extern double
math_sinh_uncached(double x);

extern double
math_tanh_impl(MathCache* cache, double x);

extern double
math_tanh_uncached(double x);

extern double
math_acosh_impl(MathCache* cache, double x);

extern double
math_acosh_uncached(double x);

extern double
math_asinh_impl(MathCache* cache, double x);

extern double
math_asinh_uncached(double x);

extern double
math_atanh_impl(MathCache* cache, double x);

extern double
math_atanh_uncached(double x);

extern double
math_trunc_impl(MathCache* cache, double x);

extern double
math_trunc_uncached(double x);

extern double
math_sign_impl(MathCache* cache, double x);

extern double
math_sign_uncached(double x);

extern double
math_cbrt_impl(MathCache* cache, double x);

extern double
math_cbrt_uncached(double x);

} /* namespace js */

#endif /* jsmath_h */
