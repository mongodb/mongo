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
#include "mozilla/Types.h"

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
  typedef uint32_t Bits;

  static const unsigned kExponentBias = 127;
  static const unsigned kExponentShift = 23;

  static const Bits kSignBit         = 0x80000000UL;
  static const Bits kExponentBits    = 0x7F800000UL;
  static const Bits kSignificandBits = 0x007FFFFFUL;
};

struct DoubleTypeTraits
{
  typedef uint64_t Bits;

  static const unsigned kExponentBias = 1023;
  static const unsigned kExponentShift = 52;

  static const Bits kSignBit         = 0x8000000000000000ULL;
  static const Bits kExponentBits    = 0x7ff0000000000000ULL;
  static const Bits kSignificandBits = 0x000fffffffffffffULL;
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
  typedef SelectTrait<T> Base;
  typedef typename Base::Bits Bits;

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
static MOZ_ALWAYS_INLINE MOZ_CONSTEXPR bool
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


/** Constructs a NaN value with the specified sign bit and significand bits. */
template<typename T>
static MOZ_ALWAYS_INLINE T
SpecificNaN(int signbit, typename FloatingPoint<T>::Bits significand)
{
  typedef FloatingPoint<T> Traits;
  MOZ_ASSERT(signbit == 0 || signbit == 1);
  MOZ_ASSERT((significand & ~Traits::kSignificandBits) == 0);
  MOZ_ASSERT(significand & Traits::kSignificandBits);

  T t = BitwiseCast<T>((signbit ? Traits::kSignBit : 0) |
                       Traits::kExponentBits |
                       significand);
  MOZ_ASSERT(IsNaN(t));
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

/**
 * If aValue is equal to some int32_t value, set *aInt32 to that value and
 * return true; otherwise return false.
 *
 * Note that negative zero is "equal" to zero here. To test whether a value can
 * be losslessly converted to int32_t and back, use NumberIsInt32 instead.
 */
template<typename T>
static MOZ_ALWAYS_INLINE bool
NumberEqualsInt32(T aValue, int32_t* aInt32)
{
  /*
   * XXX Casting a floating-point value that doesn't truncate to int32_t, to
   *     int32_t, induces undefined behavior.  We should definitely fix this
   *     (bug 744965), but as apparently it "works" in practice, it's not a
   *     pressing concern now.
   */
  return aValue == (*aInt32 = int32_t(aValue));
}

/**
 * If d can be converted to int32_t and back to an identical double value,
 * set *aInt32 to that value and return true; otherwise return false.
 *
 * The difference between this and NumberEqualsInt32 is that this method returns
 * false for negative zero.
 */
template<typename T>
static MOZ_ALWAYS_INLINE bool
NumberIsInt32(T aValue, int32_t* aInt32)
{
  return !IsNegativeZero(aValue) && NumberEqualsInt32(aValue, aInt32);
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

  // Short-circuit the common case in order to avoid the expensive operations
  // below.
  if (aValue1 == aValue2) {
    return true;
  }

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
MOZ_WARN_UNUSED_RESULT
extern MFBT_API bool
IsFloat32Representable(double aFloat32);

} /* namespace mozilla */

#endif /* mozilla_FloatingPoint_h */
