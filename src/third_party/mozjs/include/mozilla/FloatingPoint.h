/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Various predicates and operations on IEEE-754 floating point types. */

#ifndef mozilla_FloatingPoint_h
#define mozilla_FloatingPoint_h

#include "mozilla/Assertions.h"
#include "mozilla/Attributes.h"
#include "mozilla/Casting.h"
#include "mozilla/MathAlgorithms.h"
#include "mozilla/MemoryChecking.h"
#include "mozilla/Types.h"
#include "mozilla/TypeTraits.h"

#include <limits>
#include <stdint.h>

namespace mozilla {

/*
 * It's reasonable to ask why we have this header at all.  Don't isnan,
 * copysign, the built-in comparison operators, and the like solve these
 * problems?  Unfortunately, they don't.  We've found that various compilers
 * (MSVC, MSVC when compiling with PGO, and GCC on OS X, at least) miscompile
 * the standard methods in various situations, so we can't use them.  Some of
 * these compilers even have problems compiling seemingly reasonable bitwise
 * algorithms!  But with some care we've found algorithms that seem to not
 * trigger those compiler bugs.
 *
 * For the aforementioned reasons, be very wary of making changes to any of
 * these algorithms.  If you must make changes, keep a careful eye out for
 * compiler bustage, particularly PGO-specific bustage.
 */

struct FloatTypeTraits
{
  using Bits = uint32_t;

  static constexpr unsigned kExponentBias = 127;
  static constexpr unsigned kExponentShift = 23;

  static constexpr Bits kSignBit         = 0x80000000UL;
  static constexpr Bits kExponentBits    = 0x7F800000UL;
  static constexpr Bits kSignificandBits = 0x007FFFFFUL;
};

struct DoubleTypeTraits
{
  using Bits = uint64_t;

  static constexpr unsigned kExponentBias = 1023;
  static constexpr unsigned kExponentShift = 52;

  static constexpr Bits kSignBit         = 0x8000000000000000ULL;
  static constexpr Bits kExponentBits    = 0x7ff0000000000000ULL;
  static constexpr Bits kSignificandBits = 0x000fffffffffffffULL;
};

template<typename T> struct SelectTrait;
template<> struct SelectTrait<float> : public FloatTypeTraits {};
template<> struct SelectTrait<double> : public DoubleTypeTraits {};

/*
 *  This struct contains details regarding the encoding of floating-point
 *  numbers that can be useful for direct bit manipulation. As of now, the
 *  template parameter has to be float or double.
 *
 *  The nested typedef |Bits| is the unsigned integral type with the same size
 *  as T: uint32_t for float and uint64_t for double (static assertions
 *  double-check these assumptions).
 *
 *  kExponentBias is the offset that is subtracted from the exponent when
 *  computing the value, i.e. one plus the opposite of the mininum possible
 *  exponent.
 *  kExponentShift is the shift that one needs to apply to retrieve the
 *  exponent component of the value.
 *
 *  kSignBit contains a bits mask. Bit-and-ing with this mask will result in
 *  obtaining the sign bit.
 *  kExponentBits contains the mask needed for obtaining the exponent bits and
 *  kSignificandBits contains the mask needed for obtaining the significand
 *  bits.
 *
 *  Full details of how floating point number formats are encoded are beyond
 *  the scope of this comment. For more information, see
 *  http://en.wikipedia.org/wiki/IEEE_floating_point
 *  http://en.wikipedia.org/wiki/Floating_point#IEEE_754:_floating_point_in_modern_computers
 */
template<typename T>
struct FloatingPoint : public SelectTrait<T>
{
  using Base = SelectTrait<T>;
  using Bits = typename Base::Bits;

  static_assert((Base::kSignBit & Base::kExponentBits) == 0,
                "sign bit shouldn't overlap exponent bits");
  static_assert((Base::kSignBit & Base::kSignificandBits) == 0,
                "sign bit shouldn't overlap significand bits");
  static_assert((Base::kExponentBits & Base::kSignificandBits) == 0,
                "exponent bits shouldn't overlap significand bits");

  static_assert((Base::kSignBit | Base::kExponentBits | Base::kSignificandBits) ==
                ~Bits(0),
                "all bits accounted for");

  /*
   * These implementations assume float/double are 32/64-bit single/double
   * format number types compatible with the IEEE-754 standard.  C++ don't
   * require this to be the case.  But we required this in implementations of
   * these algorithms that preceded this header, so we shouldn't break anything
   * if we keep doing so.
   */
  static_assert(sizeof(T) == sizeof(Bits), "Bits must be same size as T");
};

/** Determines whether a float/double is NaN. */
template<typename T>
static MOZ_ALWAYS_INLINE bool
IsNaN(T aValue)
{
  /*
   * A float/double is NaN if all exponent bits are 1 and the significand
   * contains at least one non-zero bit.
   */
  typedef FloatingPoint<T> Traits;
  typedef typename Traits::Bits Bits;
  return (BitwiseCast<Bits>(aValue) & Traits::kExponentBits) == Traits::kExponentBits &&
         (BitwiseCast<Bits>(aValue) & Traits::kSignificandBits) != 0;
}

/** Determines whether a float/double is +Infinity or -Infinity. */
template<typename T>
static MOZ_ALWAYS_INLINE bool
IsInfinite(T aValue)
{
  /* Infinities have all exponent bits set to 1 and an all-0 significand. */
  typedef FloatingPoint<T> Traits;
  typedef typename Traits::Bits Bits;
  Bits bits = BitwiseCast<Bits>(aValue);
  return (bits & ~Traits::kSignBit) == Traits::kExponentBits;
}

/** Determines whether a float/double is not NaN or infinite. */
template<typename T>
static MOZ_ALWAYS_INLINE bool
IsFinite(T aValue)
{
  /*
   * NaN and Infinities are the only non-finite floats/doubles, and both have
   * all exponent bits set to 1.
   */
  typedef FloatingPoint<T> Traits;
  typedef typename Traits::Bits Bits;
  Bits bits = BitwiseCast<Bits>(aValue);
  return (bits & Traits::kExponentBits) != Traits::kExponentBits;
}

/**
 * Determines whether a float/double is negative or -0.  It is an error
 * to call this method on a float/double which is NaN.
 */
template<typename T>
static MOZ_ALWAYS_INLINE bool
IsNegative(T aValue)
{
  MOZ_ASSERT(!IsNaN(aValue), "NaN does not have a sign");

  /* The sign bit is set if the double is negative. */
  typedef FloatingPoint<T> Traits;
  typedef typename Traits::Bits Bits;
  Bits bits = BitwiseCast<Bits>(aValue);
  return (bits & Traits::kSignBit) != 0;
}

/** Determines whether a float/double represents -0. */
template<typename T>
static MOZ_ALWAYS_INLINE bool
IsNegativeZero(T aValue)
{
  /* Only the sign bit is set if the value is -0. */
  typedef FloatingPoint<T> Traits;
  typedef typename Traits::Bits Bits;
  Bits bits = BitwiseCast<Bits>(aValue);
  return bits == Traits::kSignBit;
}

/** Determines wether a float/double represents +0. */
template<typename T>
static MOZ_ALWAYS_INLINE bool
IsPositiveZero(T aValue)
{
  /* All bits are zero if the value is +0. */
  typedef FloatingPoint<T> Traits;
  typedef typename Traits::Bits Bits;
  Bits bits = BitwiseCast<Bits>(aValue);
  return bits == 0;
}

/**
 * Returns 0 if a float/double is NaN or infinite;
 * otherwise, the float/double is returned.
 */
template<typename T>
static MOZ_ALWAYS_INLINE T
ToZeroIfNonfinite(T aValue)
{
  return IsFinite(aValue) ? aValue : 0;
}

/**
 * Returns the exponent portion of the float/double.
 *
 * Zero is not special-cased, so ExponentComponent(0.0) is
 * -int_fast16_t(Traits::kExponentBias).
 */
template<typename T>
static MOZ_ALWAYS_INLINE int_fast16_t
ExponentComponent(T aValue)
{
  /*
   * The exponent component of a float/double is an unsigned number, biased
   * from its actual value.  Subtract the bias to retrieve the actual exponent.
   */
  typedef FloatingPoint<T> Traits;
  typedef typename Traits::Bits Bits;
  Bits bits = BitwiseCast<Bits>(aValue);
  return int_fast16_t((bits & Traits::kExponentBits) >> Traits::kExponentShift) -
         int_fast16_t(Traits::kExponentBias);
}

/** Returns +Infinity. */
template<typename T>
static MOZ_ALWAYS_INLINE T
PositiveInfinity()
{
  /*
   * Positive infinity has all exponent bits set, sign bit set to 0, and no
   * significand.
   */
  typedef FloatingPoint<T> Traits;
  return BitwiseCast<T>(Traits::kExponentBits);
}

/** Returns -Infinity. */
template<typename T>
static MOZ_ALWAYS_INLINE T
NegativeInfinity()
{
  /*
   * Negative infinity has all exponent bits set, sign bit set to 1, and no
   * significand.
   */
  typedef FloatingPoint<T> Traits;
  return BitwiseCast<T>(Traits::kSignBit | Traits::kExponentBits);
}

/**
 * Computes the bit pattern for a NaN with the specified sign bit and
 * significand bits.
 */
template<typename T,
         int SignBit,
         typename FloatingPoint<T>::Bits Significand>
struct SpecificNaNBits
{
  using Traits = FloatingPoint<T>;

  static_assert(SignBit == 0 || SignBit == 1, "bad sign bit");
  static_assert((Significand & ~Traits::kSignificandBits) == 0,
                "significand must only have significand bits set");
  static_assert(Significand & Traits::kSignificandBits,
                "significand must be nonzero");

  static constexpr typename Traits::Bits value =
    (SignBit * Traits::kSignBit) | Traits::kExponentBits | Significand;
};

/**
 * Constructs a NaN value with the specified sign bit and significand bits.
 *
 * There is also a variant that returns the value directly.  In most cases, the
 * two variants should be identical.  However, in the specific case of x86
 * chips, the behavior differs: returning floating-point values directly is done
 * through the x87 stack, and x87 loads and stores turn signaling NaNs into
 * quiet NaNs... silently.  Returning floating-point values via outparam,
 * however, is done entirely within the SSE registers when SSE2 floating-point
 * is enabled in the compiler, which has semantics-preserving behavior you would
 * expect.
 *
 * If preserving the distinction between signaling NaNs and quiet NaNs is
 * important to you, you should use the outparam version.  In all other cases,
 * you should use the direct return version.
 */
template<typename T>
static MOZ_ALWAYS_INLINE void
SpecificNaN(int signbit, typename FloatingPoint<T>::Bits significand, T* result)
{
  typedef FloatingPoint<T> Traits;
  MOZ_ASSERT(signbit == 0 || signbit == 1);
  MOZ_ASSERT((significand & ~Traits::kSignificandBits) == 0);
  MOZ_ASSERT(significand & Traits::kSignificandBits);

  BitwiseCast<T>((signbit ? Traits::kSignBit : 0) |
                  Traits::kExponentBits |
                  significand,
                  result);
  MOZ_ASSERT(IsNaN(*result));
}

template<typename T>
static MOZ_ALWAYS_INLINE T
SpecificNaN(int signbit, typename FloatingPoint<T>::Bits significand)
{
  T t;
  SpecificNaN(signbit, significand, &t);
  return t;
}

/** Computes the smallest non-zero positive float/double value. */
template<typename T>
static MOZ_ALWAYS_INLINE T
MinNumberValue()
{
  typedef FloatingPoint<T> Traits;
  typedef typename Traits::Bits Bits;
  return BitwiseCast<T>(Bits(1));
}

namespace detail {

template<typename Float, typename SignedInteger>
inline bool
NumberEqualsSignedInteger(Float aValue, SignedInteger* aInteger)
{
  static_assert(IsSame<Float, float>::value || IsSame<Float, double>::value,
                "Float must be an IEEE-754 floating point type");
  static_assert(IsSigned<SignedInteger>::value,
                "this algorithm only works for signed types: a different one "
                "will be required for unsigned types");
  static_assert(sizeof(SignedInteger) >= sizeof(int),
                "this function *might* require some finessing for signed types "
                "subject to integral promotion before it can be used on them");

  MOZ_MAKE_MEM_UNDEFINED(aInteger, sizeof(*aInteger));

  // NaNs and infinities are not integers.
  if (!IsFinite(aValue)) {
    return false;
  }

  // Otherwise do direct comparisons against the minimum/maximum |SignedInteger|
  // values that can be encoded in |Float|.

  constexpr SignedInteger MaxIntValue =
    std::numeric_limits<SignedInteger>::max(); // e.g. INT32_MAX
  constexpr SignedInteger MinValue =
    std::numeric_limits<SignedInteger>::min(); // e.g. INT32_MIN

  static_assert(IsPowerOfTwo(Abs(MinValue)),
                "MinValue should be is a small power of two, thus exactly "
                "representable in float/double both");

  constexpr unsigned SignedIntegerWidth = CHAR_BIT * sizeof(SignedInteger);
  constexpr unsigned ExponentShift = FloatingPoint<Float>::kExponentShift;

  // Careful!  |MaxIntValue| may not be the maximum |SignedInteger| value that
  // can be encoded in |Float|.  Its |SignedIntegerWidth - 1| bits of precision
  // may exceed |Float|'s |ExponentShift + 1| bits of precision.  If necessary,
  // compute the maximum |SignedInteger| that fits in |Float| from IEEE-754
  // first principles.  (|MinValue| doesn't have this problem because as a
  // [relatively] small power of two it's always representable in |Float|.)

  // Per C++11 [expr.const]p2, unevaluated subexpressions of logical AND/OR and
  // conditional expressions *may* contain non-constant expressions, without
  // making the enclosing expression not constexpr.  MSVC implements this -- but
  // it sometimes warns about undefined behavior in unevaluated subexpressions.
  // This bites us if we initialize |MaxValue| the obvious way including an
  // |uint64_t(1) << (SignedIntegerWidth - 2 - ExponentShift)| subexpression.
  // Pull that shift-amount out and give it a not-too-huge value when it's in an
  // unevaluated subexpression.  🙄
  constexpr unsigned PrecisionExceededShiftAmount =
    ExponentShift > SignedIntegerWidth - 1
    ? 0
    : SignedIntegerWidth - 2 - ExponentShift;

  constexpr SignedInteger MaxValue =
   ExponentShift > SignedIntegerWidth - 1
    ? MaxIntValue
    : SignedInteger((uint64_t(1) << (SignedIntegerWidth - 1)) -
                    (uint64_t(1) << PrecisionExceededShiftAmount));

  if (static_cast<Float>(MinValue) <= aValue &&
      aValue <= static_cast<Float>(MaxValue))
  {
    auto possible = static_cast<SignedInteger>(aValue);
    if (static_cast<Float>(possible) == aValue) {
      *aInteger = possible;
      return true;
    }
  }

  return false;
}

template<typename Float, typename SignedInteger>
inline bool
NumberIsSignedInteger(Float aValue, SignedInteger* aInteger)
{
  static_assert(IsSame<Float, float>::value || IsSame<Float, double>::value,
                "Float must be an IEEE-754 floating point type");
  static_assert(IsSigned<SignedInteger>::value,
                "this algorithm only works for signed types: a different one "
                "will be required for unsigned types");
  static_assert(sizeof(SignedInteger) >= sizeof(int),
                "this function *might* require some finessing for signed types "
                "subject to integral promotion before it can be used on them");

  MOZ_MAKE_MEM_UNDEFINED(aInteger, sizeof(*aInteger));

  if (IsNegativeZero(aValue)) {
    return false;
  }

  return NumberEqualsSignedInteger(aValue, aInteger);
}

} // namespace detail

/**
 * If |aValue| is identical to some |int32_t| value, set |*aInt32| to that value
 * and return true.  Otherwise return false, leaving |*aInt32| in an
 * indeterminate state.
 *
 * This method returns false for negative zero.  If you want to consider -0 to
 * be 0, use NumberEqualsInt32 below.
 */
template<typename T>
static MOZ_ALWAYS_INLINE bool
NumberIsInt32(T aValue, int32_t* aInt32)
{
  return detail::NumberIsSignedInteger(aValue, aInt32);
}

/**
 * If |aValue| is equal to some int32_t value (where -0 and +0 are considered
 * equal), set |*aInt32| to that value and return true.  Otherwise return false,
 * leaving |*aInt32| in an indeterminate state.
 *
 * |NumberEqualsInt32(-0.0, ...)| will return true.  To test whether a value can
 * be losslessly converted to |int32_t| and back, use NumberIsInt32 above.
 */
template<typename T>
static MOZ_ALWAYS_INLINE bool
NumberEqualsInt32(T aValue, int32_t* aInt32)
{
  return detail::NumberEqualsSignedInteger(aValue, aInt32);
}

/**
 * Computes a NaN value.  Do not use this method if you depend upon a particular
 * NaN value being returned.
 */
template<typename T>
static MOZ_ALWAYS_INLINE T
UnspecifiedNaN()
{
  /*
   * If we can use any quiet NaN, we might as well use the all-ones NaN,
   * since it's cheap to materialize on common platforms (such as x64, where
   * this value can be represented in a 32-bit signed immediate field, allowing
   * it to be stored to memory in a single instruction).
   */
  typedef FloatingPoint<T> Traits;
  return SpecificNaN<T>(1, Traits::kSignificandBits);
}

/**
 * Compare two doubles for equality, *without* equating -0 to +0, and equating
 * any NaN value to any other NaN value.  (The normal equality operators equate
 * -0 with +0, and they equate NaN to no other value.)
 */
template<typename T>
static inline bool
NumbersAreIdentical(T aValue1, T aValue2)
{
  typedef FloatingPoint<T> Traits;
  typedef typename Traits::Bits Bits;
  if (IsNaN(aValue1)) {
    return IsNaN(aValue2);
  }
  return BitwiseCast<Bits>(aValue1) == BitwiseCast<Bits>(aValue2);
}

namespace detail {

template<typename T>
struct FuzzyEqualsEpsilon;

template<>
struct FuzzyEqualsEpsilon<float>
{
  // A number near 1e-5 that is exactly representable in a float.
  static float value() { return 1.0f / (1 << 17); }
};

template<>
struct FuzzyEqualsEpsilon<double>
{
  // A number near 1e-12 that is exactly representable in a double.
  static double value() { return 1.0 / (1LL << 40); }
};

} // namespace detail

/**
 * Compare two floating point values for equality, modulo rounding error. That
 * is, the two values are considered equal if they are both not NaN and if they
 * are less than or equal to aEpsilon apart. The default value of aEpsilon is
 * near 1e-5.
 *
 * For most scenarios you will want to use FuzzyEqualsMultiplicative instead,
 * as it is more reasonable over the entire range of floating point numbers.
 * This additive version should only be used if you know the range of the
 * numbers you are dealing with is bounded and stays around the same order of
 * magnitude.
 */
template<typename T>
static MOZ_ALWAYS_INLINE bool
FuzzyEqualsAdditive(T aValue1, T aValue2,
                    T aEpsilon = detail::FuzzyEqualsEpsilon<T>::value())
{
  static_assert(IsFloatingPoint<T>::value, "floating point type required");
  return Abs(aValue1 - aValue2) <= aEpsilon;
}

/**
 * Compare two floating point values for equality, allowing for rounding error
 * relative to the magnitude of the values. That is, the two values are
 * considered equal if they are both not NaN and they are less than or equal to
 * some aEpsilon apart, where the aEpsilon is scaled by the smaller of the two
 * argument values.
 *
 * In most cases you will want to use this rather than FuzzyEqualsAdditive, as
 * this function effectively masks out differences in the bottom few bits of
 * the floating point numbers being compared, regardless of what order of
 * magnitude those numbers are at.
 */
template<typename T>
static MOZ_ALWAYS_INLINE bool
FuzzyEqualsMultiplicative(T aValue1, T aValue2,
                          T aEpsilon = detail::FuzzyEqualsEpsilon<T>::value())
{
  static_assert(IsFloatingPoint<T>::value, "floating point type required");
  // can't use std::min because of bug 965340
  T smaller = Abs(aValue1) < Abs(aValue2) ? Abs(aValue1) : Abs(aValue2);
  return Abs(aValue1 - aValue2) <= aEpsilon * smaller;
}

/**
 * Returns true if the given value can be losslessly represented as an IEEE-754
 * single format number, false otherwise.  All NaN values are considered
 * representable (notwithstanding that the exact bit pattern of a double format
 * NaN value can't be exactly represented in single format).
 *
 * This function isn't inlined to avoid buggy optimizations by MSVC.
 */
MOZ_MUST_USE
extern MFBT_API bool
IsFloat32Representable(double aFloat32);

} /* namespace mozilla */

#endif /* mozilla_FloatingPoint_h */
