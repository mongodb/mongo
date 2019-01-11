/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * JS math package.
 */

#include "jsmath.h"

#include "mozilla/FloatingPoint.h"
#include "mozilla/MathAlgorithms.h"
#include "mozilla/MemoryReporting.h"
#include "mozilla/Unused.h"
#include "mozilla/WrappingOperations.h"

#include <algorithm>  // for std::max
#include <fcntl.h>
#ifdef XP_UNIX
# include <unistd.h>
#endif

#include "fdlibm.h"
#include "jsapi.h"
#include "jslibmath.h"
#include "jstypes.h"

#include "jit/InlinableNatives.h"
#include "js/Class.h"
#include "util/Windows.h"
#include "vm/JSAtom.h"
#include "vm/JSCompartment.h"
#include "vm/JSContext.h"
#include "vm/Time.h"

#include "vm/JSObject-inl.h"

#if defined(XP_WIN)
// #define needed to link in RtlGenRandom(), a.k.a. SystemFunction036.  See the
// "Community Additions" comment on MSDN here:
// https://msdn.microsoft.com/en-us/library/windows/desktop/aa387694.aspx
# define SystemFunction036 NTAPI SystemFunction036
# include <ntsecapi.h>
# undef SystemFunction036
#endif

#if defined(ANDROID) || defined(XP_DARWIN) || defined(__DragonFly__) || \
    defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
# include <stdlib.h>
# define HAVE_ARC4RANDOM
#endif

#if defined(__linux__)
# include <linux/random.h> // For GRND_NONBLOCK.
# include <sys/syscall.h> // For SYS_getrandom.

// Older glibc versions don't define SYS_getrandom, so we define it here if
// it's not available. See bug 995069.
# if defined(__x86_64__)
#  define GETRANDOM_NR 318
# elif defined(__i386__)
#  define GETRANDOM_NR 355
# elif defined(__aarch64__)
#  define GETRANDOM_NR 278
# elif defined(__arm__)
#  define GETRANDOM_NR 384
# elif defined(__powerpc__)
#  define GETRANDOM_NR 359
# elif defined(__s390__)
#  define GETRANDOM_NR 349
# elif defined(__mips__)
#  include <sgidefs.h>
#  if _MIPS_SIM == _MIPS_SIM_ABI32
#    define GETRANDOM_NR 4353
#  elif _MIPS_SIM == _MIPS_SIM_ABI64
#    define GETRANDOM_NR 5313
#  elif _MIPS_SIM == _MIPS_SIM_NABI32
#    define GETRANDOM_NR 6317
#  endif
# endif

# if defined(SYS_getrandom)
// We have SYS_getrandom. Use it to check GETRANDOM_NR. Only do this if we set
// GETRANDOM_NR so tier 3 platforms with recent glibc are not forced to define
// it for no good reason.
#  if defined(GETRANDOM_NR)
static_assert(GETRANDOM_NR == SYS_getrandom,
              "GETRANDOM_NR should match the actual SYS_getrandom value");
#  endif
# else
#  define SYS_getrandom GETRANDOM_NR
# endif

# if defined(GRND_NONBLOCK)
static_assert(GRND_NONBLOCK == 1, "If GRND_NONBLOCK is not 1 the #define below is wrong");
# else
#  define GRND_NONBLOCK 1
# endif

#endif // defined(__linux__)

using namespace js;

using mozilla::Abs;
using mozilla::NumberEqualsInt32;
using mozilla::NumberIsInt32;
using mozilla::ExponentComponent;
using mozilla::FloatingPoint;
using mozilla::IsFinite;
using mozilla::IsInfinite;
using mozilla::IsNaN;
using mozilla::IsNegative;
using mozilla::IsNegativeZero;
using mozilla::PositiveInfinity;
using mozilla::NegativeInfinity;
using mozilla::WrappingMultiply;
using JS::ToNumber;
using JS::GenericNaN;

static const JSConstDoubleSpec math_constants[] = {
    {"E"      ,  M_E       },
    {"LOG2E"  ,  M_LOG2E   },
    {"LOG10E" ,  M_LOG10E  },
    {"LN2"    ,  M_LN2     },
    {"LN10"   ,  M_LN10    },
    {"PI"     ,  M_PI      },
    {"SQRT2"  ,  M_SQRT2   },
    {"SQRT1_2",  M_SQRT1_2 },
    {nullptr  ,  0         }
};

MathCache::MathCache() {
    memset(table, 0, sizeof(table));

    /* See comments in lookup(). */
    MOZ_ASSERT(IsNegativeZero(-0.0));
    MOZ_ASSERT(!IsNegativeZero(+0.0));
    MOZ_ASSERT(hash(-0.0, MathCache::Sin) != hash(+0.0, MathCache::Sin));
}

size_t
MathCache::sizeOfIncludingThis(mozilla::MallocSizeOf mallocSizeOf)
{
    return mallocSizeOf(this);
}

const Class js::MathClass = {
    js_Math_str,
    JSCLASS_HAS_CACHED_PROTO(JSProto_Math)
};

bool
js::math_abs_handle(JSContext* cx, js::HandleValue v, js::MutableHandleValue r)
{
    double x;
    if (!ToNumber(cx, v, &x))
        return false;

    double z = Abs(x);
    r.setNumber(z);

    return true;
}

bool
js::math_abs(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    if (args.length() == 0) {
        args.rval().setNaN();
        return true;
    }

    return math_abs_handle(cx, args[0], args.rval());
}

double
js::math_acos_impl(MathCache* cache, double x)
{
    AutoUnsafeCallWithABI unsafe;
    return cache->lookup(fdlibm::acos, x, MathCache::Acos);
}

double
js::math_acos_uncached(double x)
{
    AutoUnsafeCallWithABI unsafe;
    return fdlibm::acos(x);
}

bool
js::math_acos(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    if (args.length() == 0) {
        args.rval().setNaN();
        return true;
    }

    double x;
    if (!ToNumber(cx, args[0], &x))
        return false;

    MathCache* mathCache = cx->caches().getMathCache(cx);
    if (!mathCache)
        return false;

    double z = math_acos_impl(mathCache, x);
    args.rval().setDouble(z);
    return true;
}

double
js::math_asin_impl(MathCache* cache, double x)
{
    AutoUnsafeCallWithABI unsafe;
    return cache->lookup(fdlibm::asin, x, MathCache::Asin);
}

double
js::math_asin_uncached(double x)
{
    AutoUnsafeCallWithABI unsafe;
    return fdlibm::asin(x);
}

bool
js::math_asin(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    if (args.length() == 0) {
        args.rval().setNaN();
        return true;
    }

    double x;
    if (!ToNumber(cx, args[0], &x))
        return false;

    MathCache* mathCache = cx->caches().getMathCache(cx);
    if (!mathCache)
        return false;

    double z = math_asin_impl(mathCache, x);
    args.rval().setDouble(z);
    return true;
}

double
js::math_atan_impl(MathCache* cache, double x)
{
    AutoUnsafeCallWithABI unsafe;
    return cache->lookup(fdlibm::atan, x, MathCache::Atan);
}

double
js::math_atan_uncached(double x)
{
    AutoUnsafeCallWithABI unsafe;
    return fdlibm::atan(x);
}

bool
js::math_atan(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    if (args.length() == 0) {
        args.rval().setNaN();
        return true;
    }

    double x;
    if (!ToNumber(cx, args[0], &x))
        return false;

    MathCache* mathCache = cx->caches().getMathCache(cx);
    if (!mathCache)
        return false;

    double z = math_atan_impl(mathCache, x);
    args.rval().setDouble(z);
    return true;
}

double
js::ecmaAtan2(double y, double x)
{
    AutoUnsafeCallWithABI unsafe;
    return fdlibm::atan2(y, x);
}

bool
js::math_atan2_handle(JSContext* cx, HandleValue y, HandleValue x, MutableHandleValue res)
{
    double dy;
    if (!ToNumber(cx, y, &dy))
        return false;

    double dx;
    if (!ToNumber(cx, x, &dx))
        return false;

    double z = ecmaAtan2(dy, dx);
    res.setDouble(z);
    return true;
}

bool
js::math_atan2(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    return math_atan2_handle(cx, args.get(0), args.get(1), args.rval());
}

double
js::math_ceil_impl(double x)
{
    AutoUnsafeCallWithABI unsafe;
    return fdlibm::ceil(x);
}

bool
js::math_ceil_handle(JSContext* cx, HandleValue v, MutableHandleValue res)
{
    double d;
    if(!ToNumber(cx, v, &d))
        return false;

    double result = math_ceil_impl(d);
    res.setNumber(result);
    return true;
}

bool
js::math_ceil(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    if (args.length() == 0) {
        args.rval().setNaN();
        return true;
    }

    return math_ceil_handle(cx, args[0], args.rval());
}

bool
js::math_clz32(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    if (args.length() == 0) {
        args.rval().setInt32(32);
        return true;
    }

    uint32_t n;
    if (!ToUint32(cx, args[0], &n))
        return false;

    if (n == 0) {
        args.rval().setInt32(32);
        return true;
    }

    args.rval().setInt32(mozilla::CountLeadingZeroes32(n));
    return true;
}

double
js::math_cos_impl(MathCache* cache, double x)
{
    AutoUnsafeCallWithABI unsafe;
    return cache->lookup(cos, x, MathCache::Cos);
}

double
js::math_cos_uncached(double x)
{
    AutoUnsafeCallWithABI unsafe;
    return cos(x);
}

bool
js::math_cos(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    if (args.length() == 0) {
        args.rval().setNaN();
        return true;
    }

    double x;
    if (!ToNumber(cx, args[0], &x))
        return false;

    MathCache* mathCache = cx->caches().getMathCache(cx);
    if (!mathCache)
        return false;

    double z = math_cos_impl(mathCache, x);
    args.rval().setDouble(z);
    return true;
}

double
js::math_exp_impl(MathCache* cache, double x)
{
    AutoUnsafeCallWithABI unsafe;
    return cache->lookup(fdlibm::exp, x, MathCache::Exp);
}

double
js::math_exp_uncached(double x)
{
    AutoUnsafeCallWithABI unsafe;
    return fdlibm::exp(x);
}

bool
js::math_exp(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    if (args.length() == 0) {
        args.rval().setNaN();
        return true;
    }

    double x;
    if (!ToNumber(cx, args[0], &x))
        return false;

    MathCache* mathCache = cx->caches().getMathCache(cx);
    if (!mathCache)
        return false;

    double z = math_exp_impl(mathCache, x);
    args.rval().setNumber(z);
    return true;
}

double
js::math_floor_impl(double x)
{
    AutoUnsafeCallWithABI unsafe;
    return fdlibm::floor(x);
}

bool
js::math_floor_handle(JSContext* cx, HandleValue v, MutableHandleValue r)
{
    double d;
    if (!ToNumber(cx, v, &d))
        return false;

    double z = math_floor_impl(d);
    r.setNumber(z);

    return true;
}

bool
js::math_floor(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    if (args.length() == 0) {
        args.rval().setNaN();
        return true;
    }

    return math_floor_handle(cx, args[0], args.rval());
}

bool
js::math_imul_handle(JSContext* cx, HandleValue lhs, HandleValue rhs, MutableHandleValue res)
{
    int32_t a = 0, b = 0;
    if (!lhs.isUndefined() && !ToInt32(cx, lhs, &a))
        return false;
    if (!rhs.isUndefined() && !ToInt32(cx, rhs, &b))
        return false;

    res.setInt32(WrappingMultiply(a, b));
    return true;
}

bool
js::math_imul(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    return math_imul_handle(cx, args.get(0), args.get(1), args.rval());
}

// Implements Math.fround (20.2.2.16) up to step 3
bool
js::RoundFloat32(JSContext* cx, HandleValue v, float* out)
{
    double d;
    bool success = ToNumber(cx, v, &d);
    *out = static_cast<float>(d);
    return success;
}

bool
js::RoundFloat32(JSContext* cx, HandleValue arg, MutableHandleValue res)
{
    float f;
    if (!RoundFloat32(cx, arg, &f))
        return false;

    res.setDouble(static_cast<double>(f));
    return true;
}

bool
js::math_fround(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    if (args.length() == 0) {
        args.rval().setNaN();
        return true;
    }

    return RoundFloat32(cx, args[0], args.rval());
}

double
js::math_log_impl(MathCache* cache, double x)
{
    AutoUnsafeCallWithABI unsafe;
    return cache->lookup(math_log_uncached, x, MathCache::Log);
}

double
js::math_log_uncached(double x)
{
    AutoUnsafeCallWithABI unsafe;
    return fdlibm::log(x);
}

bool
js::math_log_handle(JSContext* cx, HandleValue val, MutableHandleValue res)
{
    double in;
    if (!ToNumber(cx, val, &in))
        return false;

    MathCache* mathCache = cx->caches().getMathCache(cx);
    if (!mathCache)
        return false;

    double out = math_log_impl(mathCache, in);
    res.setNumber(out);
    return true;
}

bool
js::math_log(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    if (args.length() == 0) {
        args.rval().setNaN();
        return true;
    }

    return math_log_handle(cx, args[0], args.rval());
}

double
js::math_max_impl(double x, double y)
{
    AutoUnsafeCallWithABI unsafe;

    // Math.max(num, NaN) => NaN, Math.max(-0, +0) => +0
    if (x > y || IsNaN(x) || (x == y && IsNegative(y)))
        return x;
    return y;
}

bool
js::math_max(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    double maxval = NegativeInfinity<double>();
    for (unsigned i = 0; i < args.length(); i++) {
        double x;
        if (!ToNumber(cx, args[i], &x))
            return false;
        maxval = math_max_impl(x, maxval);
    }
    args.rval().setNumber(maxval);
    return true;
}

double
js::math_min_impl(double x, double y)
{
    AutoUnsafeCallWithABI unsafe;

    // Math.min(num, NaN) => NaN, Math.min(-0, +0) => -0
    if (x < y || IsNaN(x) || (x == y && IsNegativeZero(x)))
        return x;
    return y;
}

bool
js::math_min(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    double minval = PositiveInfinity<double>();
    for (unsigned i = 0; i < args.length(); i++) {
        double x;
        if (!ToNumber(cx, args[i], &x))
            return false;
        minval = math_min_impl(x, minval);
    }
    args.rval().setNumber(minval);
    return true;
}

bool
js::minmax_impl(JSContext* cx, bool max, HandleValue a, HandleValue b, MutableHandleValue res)
{
    double x, y;

    if (!ToNumber(cx, a, &x))
        return false;
    if (!ToNumber(cx, b, &y))
        return false;

    if (max)
        res.setNumber(math_max_impl(x, y));
    else
        res.setNumber(math_min_impl(x, y));

    return true;
}

double
js::powi(double x, int32_t y)
{
    AutoUnsafeCallWithABI unsafe;
    uint32_t n = Abs(y);
    double m = x;
    double p = 1;
    while (true) {
        if ((n & 1) != 0) p *= m;
        n >>= 1;
        if (n == 0) {
            if (y < 0) {
                // Unfortunately, we have to be careful when p has reached
                // infinity in the computation, because sometimes the higher
                // internal precision in the pow() implementation would have
                // given us a finite p. This happens very rarely.

                double result = 1.0 / p;
                return (result == 0 && IsInfinite(p))
                       ? pow(x, static_cast<double>(y))  // Avoid pow(double, int).
                       : result;
            }

            return p;
        }
        m *= m;
    }
}

double
js::ecmaPow(double x, double y)
{
    AutoUnsafeCallWithABI unsafe;

    /*
     * Use powi if the exponent is an integer-valued double. We don't have to
     * check for NaN since a comparison with NaN is always false.
     */
    int32_t yi;
    if (NumberEqualsInt32(y, &yi))
        return powi(x, yi);

    /*
     * Because C99 and ECMA specify different behavior for pow(),
     * we need to wrap the libm call to make it ECMA compliant.
     */
    if (!IsFinite(y) && (x == 1.0 || x == -1.0))
        return GenericNaN();

    /* pow(x, +-0) is always 1, even for x = NaN (MSVC gets this wrong). */
    if (y == 0)
        return 1;

    /*
     * Special case for square roots. Note that pow(x, 0.5) != sqrt(x)
     * when x = -0.0, so we have to guard for this.
     */
    if (IsFinite(x) && x != 0.0) {
        if (y == 0.5)
            return sqrt(x);
        if (y == -0.5)
            return 1.0 / sqrt(x);
    }
    return pow(x, y);
}

bool
js::math_pow_handle(JSContext* cx, HandleValue base, HandleValue power, MutableHandleValue result)
{
    double x;
    if (!ToNumber(cx, base, &x))
        return false;

    double y;
    if (!ToNumber(cx, power, &y))
        return false;

    double z = ecmaPow(x, y);
    result.setNumber(z);
    return true;
}

bool
js::math_pow(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    return math_pow_handle(cx, args.get(0), args.get(1), args.rval());
}

uint64_t
js::GenerateRandomSeed()
{
    uint64_t seed = 0;

#if defined(XP_WIN)
    MOZ_ALWAYS_TRUE(RtlGenRandom(&seed, sizeof(seed)));
#elif defined(HAVE_ARC4RANDOM)
    seed = (static_cast<uint64_t>(arc4random()) << 32) | arc4random();
#elif defined(XP_UNIX)
    bool done = false;
# if defined(__linux__)
    // Try the relatively new getrandom syscall first. It's the preferred way
    // on Linux as /dev/urandom may not work inside chroots and is harder to
    // sandbox (see bug 995069).
    int ret = syscall(SYS_getrandom, &seed, sizeof(seed), GRND_NONBLOCK);
    done = (ret == sizeof(seed));
# endif
    if (!done) {
        int fd = open("/dev/urandom", O_RDONLY);
        if (fd >= 0) {
            mozilla::Unused << read(fd, static_cast<void*>(&seed), sizeof(seed));
            close(fd);
        }
    }
#else
# error "Platform needs to implement GenerateRandomSeed()"
#endif

    // Also mix in PRMJ_Now() in case we couldn't read random bits from the OS.
    uint64_t timestamp = PRMJ_Now();
    return seed ^ timestamp ^ (timestamp << 32);
}

void
js::GenerateXorShift128PlusSeed(mozilla::Array<uint64_t, 2>& seed)
{
    // XorShift128PlusRNG must be initialized with a non-zero seed.
    do {
        seed[0] = GenerateRandomSeed();
        seed[1] = GenerateRandomSeed();
    } while (seed[0] == 0 && seed[1] == 0);
}

void
JSCompartment::ensureRandomNumberGenerator()
{
    if (randomNumberGenerator.isNothing()) {
        mozilla::Array<uint64_t, 2> seed;
        GenerateXorShift128PlusSeed(seed);
        randomNumberGenerator.emplace(seed[0], seed[1]);
    }
}

double
js::math_random_impl(JSContext* cx)
{
    JSCompartment* comp = cx->compartment();
    comp->ensureRandomNumberGenerator();
    return comp->randomNumberGenerator.ref().nextDouble();
}

bool
js::math_random(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    args.rval().setNumber(math_random_impl(cx));
    return true;
}

bool
js::math_round_handle(JSContext* cx, HandleValue arg, MutableHandleValue res)
{
    double d;
    if (!ToNumber(cx, arg, &d))
        return false;

    d = math_round_impl(d);
    res.setNumber(d);
    return true;
}

template<typename T>
T
js::GetBiggestNumberLessThan(T x)
{
    MOZ_ASSERT(!IsNegative(x));
    MOZ_ASSERT(IsFinite(x));
    typedef typename mozilla::FloatingPoint<T>::Bits Bits;
    Bits bits = mozilla::BitwiseCast<Bits>(x);
    MOZ_ASSERT(bits > 0, "will underflow");
    return mozilla::BitwiseCast<T>(bits - 1);
}

template double js::GetBiggestNumberLessThan<>(double x);
template float js::GetBiggestNumberLessThan<>(float x);

double
js::math_round_impl(double x)
{
    AutoUnsafeCallWithABI unsafe;

    int32_t ignored;
    if (NumberIsInt32(x, &ignored))
        return x;

    /* Some numbers are so big that adding 0.5 would give the wrong number. */
    if (ExponentComponent(x) >= int_fast16_t(FloatingPoint<double>::kExponentShift))
        return x;

    double add = (x >= 0) ? GetBiggestNumberLessThan(0.5) : 0.5;
    return js_copysign(fdlibm::floor(x + add), x);
}

float
js::math_roundf_impl(float x)
{
    AutoUnsafeCallWithABI unsafe;

    int32_t ignored;
    if (NumberIsInt32(x, &ignored))
        return x;

    /* Some numbers are so big that adding 0.5 would give the wrong number. */
    if (ExponentComponent(x) >= int_fast16_t(FloatingPoint<float>::kExponentShift))
        return x;

    float add = (x >= 0) ? GetBiggestNumberLessThan(0.5f) : 0.5f;
    return js_copysign(fdlibm::floorf(x + add), x);
}

bool /* ES5 15.8.2.15. */
js::math_round(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    if (args.length() == 0) {
        args.rval().setNaN();
        return true;
    }

    return math_round_handle(cx, args[0], args.rval());
}

double
js::math_sin_impl(MathCache* cache, double x)
{
    AutoUnsafeCallWithABI unsafe;
    return cache->lookup(math_sin_uncached, x, MathCache::Sin);
}

double
js::math_sin_uncached(double x)
{
    AutoUnsafeCallWithABI unsafe;
#ifdef _WIN64
    // Workaround MSVC bug where sin(-0) is +0 instead of -0 on x64 on
    // CPUs without FMA3 (pre-Haswell). See bug 1076670.
    if (IsNegativeZero(x))
        return -0.0;
#endif
    return sin(x);
}

bool
js::math_sin_handle(JSContext* cx, HandleValue val, MutableHandleValue res)
{
    double in;
    if (!ToNumber(cx, val, &in))
        return false;

    MathCache* mathCache = cx->caches().getMathCache(cx);
    if (!mathCache)
        return false;

    double out = math_sin_impl(mathCache, in);
    res.setDouble(out);
    return true;
}

bool
js::math_sin(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    if (args.length() == 0) {
        args.rval().setNaN();
        return true;
    }

    return math_sin_handle(cx, args[0], args.rval());
}

void
js::math_sincos_uncached(double x, double *sin, double *cos)
{
    AutoUnsafeCallWithABI unsafe;
#if defined(HAVE_SINCOS)
    sincos(x, sin, cos);
#elif defined(HAVE___SINCOS)
    __sincos(x, sin, cos);
#else
    *sin = js::math_sin_uncached(x);
    *cos = js::math_cos_uncached(x);
#endif
}

void
js::math_sincos_impl(MathCache* mathCache, double x, double *sin, double *cos)
{
    AutoUnsafeCallWithABI unsafe;
    unsigned indexSin;
    unsigned indexCos;
    bool hasSin = mathCache->isCached(x, MathCache::Sin, sin, &indexSin);
    bool hasCos = mathCache->isCached(x, MathCache::Cos, cos, &indexCos);
    if (!(hasSin || hasCos)) {
        js::math_sincos_uncached(x, sin, cos);
        mathCache->store(MathCache::Sin, x, *sin, indexSin);
        mathCache->store(MathCache::Cos, x, *cos, indexCos);
        return;
    }

    if (!hasSin)
        *sin = js::math_sin_impl(mathCache, x);

    if (!hasCos)
        *cos = js::math_cos_impl(mathCache, x);
}

bool
js::math_sqrt_handle(JSContext* cx, HandleValue number, MutableHandleValue result)
{
    double x;
    if (!ToNumber(cx, number, &x))
        return false;

    MathCache* mathCache = cx->caches().getMathCache(cx);
    if (!mathCache)
        return false;

    double z = mathCache->lookup(sqrt, x, MathCache::Sqrt);
    result.setDouble(z);
    return true;
}

bool
js::math_sqrt(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    if (args.length() == 0) {
        args.rval().setNaN();
        return true;
    }

    return math_sqrt_handle(cx, args[0], args.rval());
}

double
js::math_tan_impl(MathCache* cache, double x)
{
    AutoUnsafeCallWithABI unsafe;
    return cache->lookup(tan, x, MathCache::Tan);
}

double
js::math_tan_uncached(double x)
{
    AutoUnsafeCallWithABI unsafe;
    return tan(x);
}

bool
js::math_tan(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    if (args.length() == 0) {
        args.rval().setNaN();
        return true;
    }

    double x;
    if (!ToNumber(cx, args[0], &x))
        return false;

    MathCache* mathCache = cx->caches().getMathCache(cx);
    if (!mathCache)
        return false;

    double z = math_tan_impl(mathCache, x);
    args.rval().setDouble(z);
    return true;
}

typedef double (*UnaryMathFunctionType)(MathCache* cache, double);

template <UnaryMathFunctionType F>
static bool math_function(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    if (args.length() == 0) {
        args.rval().setNumber(GenericNaN());
        return true;
    }

    double x;
    if (!ToNumber(cx, args[0], &x))
        return false;

    MathCache* mathCache = cx->caches().getMathCache(cx);
    if (!mathCache)
        return false;
    double z = F(mathCache, x);
    args.rval().setNumber(z);

    return true;
}

double
js::math_log10_impl(MathCache* cache, double x)
{
    AutoUnsafeCallWithABI unsafe;
    return cache->lookup(fdlibm::log10, x, MathCache::Log10);
}

double
js::math_log10_uncached(double x)
{
    AutoUnsafeCallWithABI unsafe;
    return fdlibm::log10(x);
}

bool
js::math_log10(JSContext* cx, unsigned argc, Value* vp)
{
    return math_function<math_log10_impl>(cx, argc, vp);
}

double
js::math_log2_impl(MathCache* cache, double x)
{
    AutoUnsafeCallWithABI unsafe;
    return cache->lookup(fdlibm::log2, x, MathCache::Log2);
}

double
js::math_log2_uncached(double x)
{
    AutoUnsafeCallWithABI unsafe;
    return fdlibm::log2(x);
}

bool
js::math_log2(JSContext* cx, unsigned argc, Value* vp)
{
    return math_function<math_log2_impl>(cx, argc, vp);
}

double
js::math_log1p_impl(MathCache* cache, double x)
{
    AutoUnsafeCallWithABI unsafe;
    return cache->lookup(fdlibm::log1p, x, MathCache::Log1p);
}

double
js::math_log1p_uncached(double x)
{
    AutoUnsafeCallWithABI unsafe;
    return fdlibm::log1p(x);
}

bool
js::math_log1p(JSContext* cx, unsigned argc, Value* vp)
{
    return math_function<math_log1p_impl>(cx, argc, vp);
}

double
js::math_expm1_impl(MathCache* cache, double x)
{
    AutoUnsafeCallWithABI unsafe;
    return cache->lookup(fdlibm::expm1, x, MathCache::Expm1);
}

double
js::math_expm1_uncached(double x)
{
    AutoUnsafeCallWithABI unsafe;
    return fdlibm::expm1(x);
}

bool
js::math_expm1(JSContext* cx, unsigned argc, Value* vp)
{
    return math_function<math_expm1_impl>(cx, argc, vp);
}

double
js::math_cosh_impl(MathCache* cache, double x)
{
    AutoUnsafeCallWithABI unsafe;
    return cache->lookup(fdlibm::cosh, x, MathCache::Cosh);
}

double
js::math_cosh_uncached(double x)
{
    AutoUnsafeCallWithABI unsafe;
    return fdlibm::cosh(x);
}

bool
js::math_cosh(JSContext* cx, unsigned argc, Value* vp)
{
    return math_function<math_cosh_impl>(cx, argc, vp);
}

double
js::math_sinh_impl(MathCache* cache, double x)
{
    AutoUnsafeCallWithABI unsafe;
    return cache->lookup(fdlibm::sinh, x, MathCache::Sinh);
}

double
js::math_sinh_uncached(double x)
{
    AutoUnsafeCallWithABI unsafe;
    return fdlibm::sinh(x);
}

bool
js::math_sinh(JSContext* cx, unsigned argc, Value* vp)
{
    return math_function<math_sinh_impl>(cx, argc, vp);
}

double
js::math_tanh_impl(MathCache* cache, double x)
{
    AutoUnsafeCallWithABI unsafe;
    return cache->lookup(fdlibm::tanh, x, MathCache::Tanh);
}

double
js::math_tanh_uncached(double x)
{
    AutoUnsafeCallWithABI unsafe;
    return fdlibm::tanh(x);
}

bool
js::math_tanh(JSContext* cx, unsigned argc, Value* vp)
{
    return math_function<math_tanh_impl>(cx, argc, vp);
}

double
js::math_acosh_impl(MathCache* cache, double x)
{
    AutoUnsafeCallWithABI unsafe;
    return cache->lookup(fdlibm::acosh, x, MathCache::Acosh);
}

double
js::math_acosh_uncached(double x)
{
    AutoUnsafeCallWithABI unsafe;
    return fdlibm::acosh(x);
}

bool
js::math_acosh(JSContext* cx, unsigned argc, Value* vp)
{
    return math_function<math_acosh_impl>(cx, argc, vp);
}

double
js::math_asinh_impl(MathCache* cache, double x)
{
    AutoUnsafeCallWithABI unsafe;
    return cache->lookup(fdlibm::asinh, x, MathCache::Asinh);
}

double
js::math_asinh_uncached(double x)
{
    AutoUnsafeCallWithABI unsafe;
    return fdlibm::asinh(x);
}

bool
js::math_asinh(JSContext* cx, unsigned argc, Value* vp)
{
    return math_function<math_asinh_impl>(cx, argc, vp);
}

double
js::math_atanh_impl(MathCache* cache, double x)
{
    AutoUnsafeCallWithABI unsafe;
    return cache->lookup(fdlibm::atanh, x, MathCache::Atanh);
}

double
js::math_atanh_uncached(double x)
{
    AutoUnsafeCallWithABI unsafe;
    return fdlibm::atanh(x);
}

bool
js::math_atanh(JSContext* cx, unsigned argc, Value* vp)
{
    return math_function<math_atanh_impl>(cx, argc, vp);
}

/* Consistency wrapper for platform deviations in hypot() */
double
js::ecmaHypot(double x, double y)
{
    AutoUnsafeCallWithABI unsafe;
    return fdlibm::hypot(x, y);
}

static inline
void
hypot_step(double& scale, double& sumsq, double x)
{
    double xabs = mozilla::Abs(x);
    if (scale < xabs) {
        sumsq = 1 + sumsq * (scale / xabs) * (scale / xabs);
        scale = xabs;
    } else if (scale != 0) {
        sumsq += (xabs / scale) * (xabs / scale);
    }
}

double
js::hypot4(double x, double y, double z, double w)
{
    AutoUnsafeCallWithABI unsafe;

    /* Check for infinity or NaNs so that we can return immediatelly.
     * Does not need to be WIN_XP specific as ecmaHypot
     */
    if (mozilla::IsInfinite(x) || mozilla::IsInfinite(y) ||
            mozilla::IsInfinite(z) || mozilla::IsInfinite(w))
        return mozilla::PositiveInfinity<double>();

    if (mozilla::IsNaN(x) || mozilla::IsNaN(y) || mozilla::IsNaN(z) ||
            mozilla::IsNaN(w))
        return GenericNaN();

    double scale = 0;
    double sumsq = 1;

    hypot_step(scale, sumsq, x);
    hypot_step(scale, sumsq, y);
    hypot_step(scale, sumsq, z);
    hypot_step(scale, sumsq, w);

    return scale * sqrt(sumsq);
}

double
js::hypot3(double x, double y, double z)
{
    AutoUnsafeCallWithABI unsafe;
    return hypot4(x, y, z, 0.0);
}

bool
js::math_hypot(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return math_hypot_handle(cx, args, args.rval());
}

bool
js::math_hypot_handle(JSContext* cx, HandleValueArray args, MutableHandleValue res)
{
    // IonMonkey calls the system hypot function directly if two arguments are
    // given. Do that here as well to get the same results.
    if (args.length() == 2) {
        double x, y;
        if (!ToNumber(cx, args[0], &x))
            return false;
        if (!ToNumber(cx, args[1], &y))
            return false;

        double result = ecmaHypot(x, y);
        res.setNumber(result);
        return true;
    }

    bool isInfinite = false;
    bool isNaN = false;

    double scale = 0;
    double sumsq = 1;

    for (unsigned i = 0; i < args.length(); i++) {
        double x;
        if (!ToNumber(cx, args[i], &x))
            return false;

        isInfinite |= mozilla::IsInfinite(x);
        isNaN |= mozilla::IsNaN(x);
        if (isInfinite || isNaN)
            continue;

        hypot_step(scale, sumsq, x);
    }

    double result = isInfinite ? PositiveInfinity<double>() :
                    isNaN ? GenericNaN() :
                    scale * sqrt(sumsq);
    res.setNumber(result);
    return true;
}

double
js::math_trunc_impl(MathCache* cache, double x)
{
    AutoUnsafeCallWithABI unsafe;
    return cache->lookup(fdlibm::trunc, x, MathCache::Trunc);
}

double
js::math_trunc_uncached(double x)
{
    AutoUnsafeCallWithABI unsafe;
    return fdlibm::trunc(x);
}

bool
js::math_trunc(JSContext* cx, unsigned argc, Value* vp)
{
    return math_function<math_trunc_impl>(cx, argc, vp);
}

static double sign(double x)
{
    if (mozilla::IsNaN(x))
        return GenericNaN();

    return x == 0 ? x : x < 0 ? -1 : 1;
}

double
js::math_sign_impl(MathCache* cache, double x)
{
    AutoUnsafeCallWithABI unsafe;
    return cache->lookup(sign, x, MathCache::Sign);
}

double
js::math_sign_uncached(double x)
{
    AutoUnsafeCallWithABI unsafe;
    return sign(x);
}

bool
js::math_sign(JSContext* cx, unsigned argc, Value* vp)
{
    return math_function<math_sign_impl>(cx, argc, vp);
}

double
js::math_cbrt_impl(MathCache* cache, double x)
{
    AutoUnsafeCallWithABI unsafe;
    return cache->lookup(fdlibm::cbrt, x, MathCache::Cbrt);
}

double
js::math_cbrt_uncached(double x)
{
    AutoUnsafeCallWithABI unsafe;
    return fdlibm::cbrt(x);
}

bool
js::math_cbrt(JSContext* cx, unsigned argc, Value* vp)
{
    return math_function<math_cbrt_impl>(cx, argc, vp);
}

static bool
math_toSource(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    args.rval().setString(cx->names().Math);
    return true;
}

static const JSFunctionSpec math_static_methods[] = {
    JS_FN(js_toSource_str,  math_toSource,        0, 0),
    JS_INLINABLE_FN("abs",    math_abs,             1, 0, MathAbs),
    JS_INLINABLE_FN("acos",   math_acos,            1, 0, MathACos),
    JS_INLINABLE_FN("asin",   math_asin,            1, 0, MathASin),
    JS_INLINABLE_FN("atan",   math_atan,            1, 0, MathATan),
    JS_INLINABLE_FN("atan2",  math_atan2,           2, 0, MathATan2),
    JS_INLINABLE_FN("ceil",   math_ceil,            1, 0, MathCeil),
    JS_INLINABLE_FN("clz32",  math_clz32,           1, 0, MathClz32),
    JS_INLINABLE_FN("cos",    math_cos,             1, 0, MathCos),
    JS_INLINABLE_FN("exp",    math_exp,             1, 0, MathExp),
    JS_INLINABLE_FN("floor",  math_floor,           1, 0, MathFloor),
    JS_INLINABLE_FN("imul",   math_imul,            2, 0, MathImul),
    JS_INLINABLE_FN("fround", math_fround,          1, 0, MathFRound),
    JS_INLINABLE_FN("log",    math_log,             1, 0, MathLog),
    JS_INLINABLE_FN("max",    math_max,             2, 0, MathMax),
    JS_INLINABLE_FN("min",    math_min,             2, 0, MathMin),
    JS_INLINABLE_FN("pow",    math_pow,             2, 0, MathPow),
    JS_INLINABLE_FN("random", math_random,          0, 0, MathRandom),
    JS_INLINABLE_FN("round",  math_round,           1, 0, MathRound),
    JS_INLINABLE_FN("sin",    math_sin,             1, 0, MathSin),
    JS_INLINABLE_FN("sqrt",   math_sqrt,            1, 0, MathSqrt),
    JS_INLINABLE_FN("tan",    math_tan,             1, 0, MathTan),
    JS_INLINABLE_FN("log10",  math_log10,           1, 0, MathLog10),
    JS_INLINABLE_FN("log2",   math_log2,            1, 0, MathLog2),
    JS_INLINABLE_FN("log1p",  math_log1p,           1, 0, MathLog1P),
    JS_INLINABLE_FN("expm1",  math_expm1,           1, 0, MathExpM1),
    JS_INLINABLE_FN("cosh",   math_cosh,            1, 0, MathCosH),
    JS_INLINABLE_FN("sinh",   math_sinh,            1, 0, MathSinH),
    JS_INLINABLE_FN("tanh",   math_tanh,            1, 0, MathTanH),
    JS_INLINABLE_FN("acosh",  math_acosh,           1, 0, MathACosH),
    JS_INLINABLE_FN("asinh",  math_asinh,           1, 0, MathASinH),
    JS_INLINABLE_FN("atanh",  math_atanh,           1, 0, MathATanH),
    JS_INLINABLE_FN("hypot",  math_hypot,           2, 0, MathHypot),
    JS_INLINABLE_FN("trunc",  math_trunc,           1, 0, MathTrunc),
    JS_INLINABLE_FN("sign",   math_sign,            1, 0, MathSign),
    JS_INLINABLE_FN("cbrt",   math_cbrt,            1, 0, MathCbrt),
    JS_FS_END
};

JSObject*
js::InitMathClass(JSContext* cx, HandleObject obj)
{
    Handle<GlobalObject*> global = obj.as<GlobalObject>();
    RootedObject proto(cx, GlobalObject::getOrCreateObjectPrototype(cx, global));
    if (!proto)
        return nullptr;
    RootedObject Math(cx, NewObjectWithGivenProto(cx, &MathClass, proto, SingletonObject));
    if (!Math)
        return nullptr;

    if (!JS_DefineProperty(cx, obj, js_Math_str, Math, JSPROP_RESOLVING))
        return nullptr;
    if (!JS_DefineFunctions(cx, Math, math_static_methods))
        return nullptr;
    if (!JS_DefineConstDoubles(cx, Math, math_constants))
        return nullptr;
    if (!DefineToStringTag(cx, Math, cx->names().Math))
        return nullptr;

    obj->as<GlobalObject>().setConstructor(JSProto_Math, ObjectValue(*Math));

    return Math;
}
