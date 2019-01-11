/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Provides checked integers, detecting integer overflow and divide-by-0. */

#ifndef mozilla_CheckedInt_h
#define mozilla_CheckedInt_h

#include <stdint.h>
#include "mozilla/Assertions.h"
#include "mozilla/Attributes.h"
#include "mozilla/IntegerTypeTraits.h"

// Probe for builtin math overflow support.  Disabled for 32-bit builds for now
// since "gcc -m32" claims to support these but its implementation is buggy.
// https://gcc.gnu.org/bugzilla/show_bug.cgi?id=82274
#if defined(HAVE_64BIT_BUILD)
#if defined(__has_builtin)
#define MOZ_HAS_BUILTIN_OP_OVERFLOW (__has_builtin(__builtin_add_overflow))
#elif defined(__GNUC__)
// (clang also defines __GNUC__ but it supports __has_builtin since at least
//  v3.1 (released in 2012) so it won't get here.)
#define MOZ_HAS_BUILTIN_OP_OVERFLOW (__GNUC__ >= 5)
#else
#define MOZ_HAS_BUILTIN_OP_OVERFLOW (0)
#endif
#else
#define MOZ_HAS_BUILTIN_OP_OVERFLOW (0)
#endif

namespace mozilla {

template<typename T> class CheckedInt;

namespace detail {

/*
 * Step 1: manually record supported types
 *
 * What's nontrivial here is that there are different families of integer
 * types: basic integer types and stdint types. It is merrily undefined which
 * types from one family may be just typedefs for a type from another family.
 *
 * For example, on GCC 4.6, aside from the basic integer types, the only other
 * type that isn't just a typedef for some of them, is int8_t.
 */

struct UnsupportedType {};

template<typename IntegerType>
struct IsSupportedPass2
{
  static const bool value = false;
};

template<typename IntegerType>
struct IsSupported
{
  static const bool value = IsSupportedPass2<IntegerType>::value;
};

template<>
struct IsSupported<int8_t>
{ static const bool value = true; };

template<>
struct IsSupported<uint8_t>
{ static const bool value = true; };

template<>
struct IsSupported<int16_t>
{ static const bool value = true; };

template<>
struct IsSupported<uint16_t>
{ static const bool value = true; };

template<>
struct IsSupported<int32_t>
{ static const bool value = true; };

template<>
struct IsSupported<uint32_t>
{ static const bool value = true; };

template<>
struct IsSupported<int64_t>
{ static const bool value = true; };

template<>
struct IsSupported<uint64_t>
{ static const bool value = true; };


template<>
struct IsSupportedPass2<char>
{ static const bool value = true; };

template<>
struct IsSupportedPass2<signed char>
{ static const bool value = true; };

template<>
struct IsSupportedPass2<unsigned char>
{ static const bool value = true; };

template<>
struct IsSupportedPass2<short>
{ static const bool value = true; };

template<>
struct IsSupportedPass2<unsigned short>
{ static const bool value = true; };

template<>
struct IsSupportedPass2<int>
{ static const bool value = true; };

template<>
struct IsSupportedPass2<unsigned int>
{ static const bool value = true; };

template<>
struct IsSupportedPass2<long>
{ static const bool value = true; };

template<>
struct IsSupportedPass2<unsigned long>
{ static const bool value = true; };

template<>
struct IsSupportedPass2<long long>
{ static const bool value = true; };

template<>
struct IsSupportedPass2<unsigned long long>
{ static const bool value = true; };

/*
 * Step 2: Implement the actual validity checks.
 *
 * Ideas taken from IntegerLib, code different.
 */

template<typename IntegerType, size_t Size = sizeof(IntegerType)>
struct TwiceBiggerType
{
  typedef typename detail::StdintTypeForSizeAndSignedness<
                     sizeof(IntegerType) * 2,
                     IsSigned<IntegerType>::value
                   >::Type Type;
};

template<typename IntegerType>
struct TwiceBiggerType<IntegerType, 8>
{
  typedef UnsupportedType Type;
};

template<typename T>
inline bool
HasSignBit(T aX)
{
  // In C++, right bit shifts on negative values is undefined by the standard.
  // Notice that signed-to-unsigned conversions are always well-defined in the
  // standard, as the value congruent modulo 2**n as expected. By contrast,
  // unsigned-to-signed is only well-defined if the value is representable.
  return bool(typename MakeUnsigned<T>::Type(aX) >>
              PositionOfSignBit<T>::value);
}

// Bitwise ops may return a larger type, so it's good to use this inline
// helper guaranteeing that the result is really of type T.
template<typename T>
inline T
BinaryComplement(T aX)
{
  return ~aX;
}

template<typename T,
         typename U,
         bool IsTSigned = IsSigned<T>::value,
         bool IsUSigned = IsSigned<U>::value>
struct DoesRangeContainRange
{
};

template<typename T, typename U, bool Signedness>
struct DoesRangeContainRange<T, U, Signedness, Signedness>
{
  static const bool value = sizeof(T) >= sizeof(U);
};

template<typename T, typename U>
struct DoesRangeContainRange<T, U, true, false>
{
  static const bool value = sizeof(T) > sizeof(U);
};

template<typename T, typename U>
struct DoesRangeContainRange<T, U, false, true>
{
  static const bool value = false;
};

template<typename T,
         typename U,
         bool IsTSigned = IsSigned<T>::value,
         bool IsUSigned = IsSigned<U>::value,
         bool DoesTRangeContainURange = DoesRangeContainRange<T, U>::value>
struct IsInRangeImpl {};

template<typename T, typename U, bool IsTSigned, bool IsUSigned>
struct IsInRangeImpl<T, U, IsTSigned, IsUSigned, true>
{
  static bool constexpr run(U)
  {
    return true;
  }
};

template<typename T, typename U>
struct IsInRangeImpl<T, U, true, true, false>
{
  static bool constexpr run(U aX)
  {
    return aX <= MaxValue<T>::value && aX >= MinValue<T>::value;
  }
};

template<typename T, typename U>
struct IsInRangeImpl<T, U, false, false, false>
{
  static bool constexpr run(U aX)
  {
    return aX <= MaxValue<T>::value;
  }
};

template<typename T, typename U>
struct IsInRangeImpl<T, U, true, false, false>
{
  static bool constexpr run(U aX)
  {
    return sizeof(T) > sizeof(U) || aX <= U(MaxValue<T>::value);
  }
};

template<typename T, typename U>
struct IsInRangeImpl<T, U, false, true, false>
{
  static bool constexpr run(U aX)
  {
    return sizeof(T) >= sizeof(U)
           ? aX >= 0
           : aX >= 0 && aX <= U(MaxValue<T>::value);
  }
};

template<typename T, typename U>
inline constexpr bool
IsInRange(U aX)
{
  return IsInRangeImpl<T, U>::run(aX);
}

template<typename T>
inline bool
IsAddValid(T aX, T aY)
{
#if MOZ_HAS_BUILTIN_OP_OVERFLOW
  T dummy;
  return !__builtin_add_overflow(aX, aY, &dummy);
#else
  // Addition is valid if the sign of aX+aY is equal to either that of aX or
  // that of aY. Since the value of aX+aY is undefined if we have a signed
  // type, we compute it using the unsigned type of the same size.  Beware!
  // These bitwise operations can return a larger integer type, if T was a
  // small type like int8_t, so we explicitly cast to T.

  typename MakeUnsigned<T>::Type ux = aX;
  typename MakeUnsigned<T>::Type uy = aY;
  typename MakeUnsigned<T>::Type result = ux + uy;
  return IsSigned<T>::value
         ? HasSignBit(BinaryComplement(T((result ^ aX) & (result ^ aY))))
         : BinaryComplement(aX) >= aY;
#endif
}

template<typename T>
inline bool
IsSubValid(T aX, T aY)
{
#if MOZ_HAS_BUILTIN_OP_OVERFLOW
  T dummy;
  return !__builtin_sub_overflow(aX, aY, &dummy);
#else
  // Subtraction is valid if either aX and aY have same sign, or aX-aY and aX
  // have same sign. Since the value of aX-aY is undefined if we have a signed
  // type, we compute it using the unsigned type of the same size.
  typename MakeUnsigned<T>::Type ux = aX;
  typename MakeUnsigned<T>::Type uy = aY;
  typename MakeUnsigned<T>::Type result = ux - uy;

  return IsSigned<T>::value
         ? HasSignBit(BinaryComplement(T((result ^ aX) & (aX ^ aY))))
         : aX >= aY;
#endif
}

template<typename T,
         bool IsTSigned = IsSigned<T>::value,
         bool TwiceBiggerTypeIsSupported =
           IsSupported<typename TwiceBiggerType<T>::Type>::value>
struct IsMulValidImpl {};

template<typename T, bool IsTSigned>
struct IsMulValidImpl<T, IsTSigned, true>
{
  static bool run(T aX, T aY)
  {
    typedef typename TwiceBiggerType<T>::Type TwiceBiggerType;
    TwiceBiggerType product = TwiceBiggerType(aX) * TwiceBiggerType(aY);
    return IsInRange<T>(product);
  }
};

template<typename T>
struct IsMulValidImpl<T, true, false>
{
  static bool run(T aX, T aY)
  {
    const T max = MaxValue<T>::value;
    const T min = MinValue<T>::value;

    if (aX == 0 || aY == 0) {
      return true;
    }
    if (aX > 0) {
      return aY > 0
             ? aX <= max / aY
             : aY >= min / aX;
    }

    // If we reach this point, we know that aX < 0.
    return aY > 0
           ? aX >= min / aY
           : aY >= max / aX;
  }
};

template<typename T>
struct IsMulValidImpl<T, false, false>
{
  static bool run(T aX, T aY)
  {
    return aY == 0 ||  aX <= MaxValue<T>::value / aY;
  }
};

template<typename T>
inline bool
IsMulValid(T aX, T aY)
{
#if MOZ_HAS_BUILTIN_OP_OVERFLOW
  T dummy;
  return !__builtin_mul_overflow(aX, aY, &dummy);
#else
  return IsMulValidImpl<T>::run(aX, aY);
#endif
}

template<typename T>
inline bool
IsDivValid(T aX, T aY)
{
  // Keep in mind that in the signed case, min/-1 is invalid because
  // abs(min)>max.
  return aY != 0 &&
         !(IsSigned<T>::value && aX == MinValue<T>::value && aY == T(-1));
}

template<typename T, bool IsTSigned = IsSigned<T>::value>
struct IsModValidImpl;

template<typename T>
inline bool
IsModValid(T aX, T aY)
{
  return IsModValidImpl<T>::run(aX, aY);
}

/*
 * Mod is pretty simple.
 * For now, let's just use the ANSI C definition:
 * If aX or aY are negative, the results are implementation defined.
 *   Consider these invalid.
 * Undefined for aY=0.
 * The result will never exceed either aX or aY.
 *
 * Checking that aX>=0 is a warning when T is unsigned.
 */

template<typename T>
struct IsModValidImpl<T, false>
{
  static inline bool run(T aX, T aY)
  {
    return aY >= 1;
  }
};

template<typename T>
struct IsModValidImpl<T, true>
{
  static inline bool run(T aX, T aY)
  {
    if (aX < 0) {
      return false;
    }
    return aY >= 1;
  }
};

template<typename T, bool IsSigned = IsSigned<T>::value>
struct NegateImpl;

template<typename T>
struct NegateImpl<T, false>
{
  static CheckedInt<T> negate(const CheckedInt<T>& aVal)
  {
    // Handle negation separately for signed/unsigned, for simpler code and to
    // avoid an MSVC warning negating an unsigned value.
    return CheckedInt<T>(0, aVal.isValid() && aVal.mValue == 0);
  }
};

template<typename T>
struct NegateImpl<T, true>
{
  static CheckedInt<T> negate(const CheckedInt<T>& aVal)
  {
    // Watch out for the min-value, which (with twos-complement) can't be
    // negated as -min-value is then (max-value + 1).
    if (!aVal.isValid() || aVal.mValue == MinValue<T>::value) {
      return CheckedInt<T>(aVal.mValue, false);
    }
    return CheckedInt<T>(-aVal.mValue, true);
  }
};

} // namespace detail


/*
 * Step 3: Now define the CheckedInt class.
 */

/**
 * @class CheckedInt
 * @brief Integer wrapper class checking for integer overflow and other errors
 * @param T the integer type to wrap. Can be any type among the following:
 *            - any basic integer type such as |int|
 *            - any stdint type such as |int8_t|
 *
 * This class implements guarded integer arithmetic. Do a computation, check
 * that isValid() returns true, you then have a guarantee that no problem, such
 * as integer overflow, happened during this computation, and you can call
 * value() to get the plain integer value.
 *
 * The arithmetic operators in this class are guaranteed not to raise a signal
 * (e.g. in case of a division by zero).
 *
 * For example, suppose that you want to implement a function that computes
 * (aX+aY)/aZ, that doesn't crash if aZ==0, and that reports on error (divide by
 * zero or integer overflow). You could code it as follows:
   @code
   bool computeXPlusYOverZ(int aX, int aY, int aZ, int* aResult)
   {
     CheckedInt<int> checkedResult = (CheckedInt<int>(aX) + aY) / aZ;
     if (checkedResult.isValid()) {
       *aResult = checkedResult.value();
       return true;
     } else {
       return false;
     }
   }
   @endcode
 *
 * Implicit conversion from plain integers to checked integers is allowed. The
 * plain integer is checked to be in range before being casted to the
 * destination type. This means that the following lines all compile, and the
 * resulting CheckedInts are correctly detected as valid or invalid:
 * @code
   // 1 is of type int, is found to be in range for uint8_t, x is valid
   CheckedInt<uint8_t> x(1);
   // -1 is of type int, is found not to be in range for uint8_t, x is invalid
   CheckedInt<uint8_t> x(-1);
   // -1 is of type int, is found to be in range for int8_t, x is valid
   CheckedInt<int8_t> x(-1);
   // 1000 is of type int16_t, is found not to be in range for int8_t,
   // x is invalid
   CheckedInt<int8_t> x(int16_t(1000));
   // 3123456789 is of type uint32_t, is found not to be in range for int32_t,
   // x is invalid
   CheckedInt<int32_t> x(uint32_t(3123456789));
 * @endcode
 * Implicit conversion from
 * checked integers to plain integers is not allowed. As shown in the
 * above example, to get the value of a checked integer as a normal integer,
 * call value().
 *
 * Arithmetic operations between checked and plain integers is allowed; the
 * result type is the type of the checked integer.
 *
 * Checked integers of different types cannot be used in the same arithmetic
 * expression.
 *
 * There are convenience typedefs for all stdint types, of the following form
 * (these are just 2 examples):
   @code
   typedef CheckedInt<int32_t> CheckedInt32;
   typedef CheckedInt<uint16_t> CheckedUint16;
   @endcode
 */
template<typename T>
class CheckedInt
{
protected:
  T mValue;
  bool mIsValid;

  template<typename U>
  CheckedInt(U aValue, bool aIsValid) : mValue(aValue), mIsValid(aIsValid)
  {
    static_assert(detail::IsSupported<T>::value &&
                  detail::IsSupported<U>::value,
                  "This type is not supported by CheckedInt");
  }

  friend struct detail::NegateImpl<T>;

public:
  /**
   * Constructs a checked integer with given @a value. The checked integer is
   * initialized as valid or invalid depending on whether the @a value
   * is in range.
   *
   * This constructor is not explicit. Instead, the type of its argument is a
   * separate template parameter, ensuring that no conversion is performed
   * before this constructor is actually called. As explained in the above
   * documentation for class CheckedInt, this constructor checks that its
   * argument is valid.
   */
  template<typename U>
  MOZ_IMPLICIT constexpr CheckedInt(U aValue) MOZ_NO_ARITHMETIC_EXPR_IN_ARGUMENT
    : mValue(T(aValue)),
      mIsValid(detail::IsInRange<T>(aValue))
  {
    static_assert(detail::IsSupported<T>::value &&
                  detail::IsSupported<U>::value,
                  "This type is not supported by CheckedInt");
  }

  template<typename U>
  friend class CheckedInt;

  template<typename U>
  CheckedInt<U> toChecked() const
  {
    CheckedInt<U> ret(mValue);
    ret.mIsValid = ret.mIsValid && mIsValid;
    return ret;
  }

  /** Constructs a valid checked integer with initial value 0 */
  constexpr CheckedInt() : mValue(0), mIsValid(true)
  {
    static_assert(detail::IsSupported<T>::value,
                  "This type is not supported by CheckedInt");
  }

  /** @returns the actual value */
  T value() const
  {
    MOZ_ASSERT(mIsValid, "Invalid checked integer (division by zero or integer overflow)");
    return mValue;
  }

  /**
   * @returns true if the checked integer is valid, i.e. is not the result
   * of an invalid operation or of an operation involving an invalid checked
   * integer
   */
  bool isValid() const
  {
    return mIsValid;
  }

  template<typename U>
  friend CheckedInt<U> operator +(const CheckedInt<U>& aLhs,
                                  const CheckedInt<U>& aRhs);
  template<typename U>
  CheckedInt& operator +=(U aRhs);
  CheckedInt& operator +=(const CheckedInt<T>& aRhs);

  template<typename U>
  friend CheckedInt<U> operator -(const CheckedInt<U>& aLhs,
                                  const CheckedInt<U>& aRhs);
  template<typename U>
  CheckedInt& operator -=(U aRhs);
  CheckedInt& operator -=(const CheckedInt<T>& aRhs);

  template<typename U>
  friend CheckedInt<U> operator *(const CheckedInt<U>& aLhs,
                                  const CheckedInt<U>& aRhs);
  template<typename U>
  CheckedInt& operator *=(U aRhs);
  CheckedInt& operator *=(const CheckedInt<T>& aRhs);

  template<typename U>
  friend CheckedInt<U> operator /(const CheckedInt<U>& aLhs,
                                  const CheckedInt<U>& aRhs);
  template<typename U>
  CheckedInt& operator /=(U aRhs);
  CheckedInt& operator /=(const CheckedInt<T>& aRhs);

  template<typename U>
  friend CheckedInt<U> operator %(const CheckedInt<U>& aLhs,
                                  const CheckedInt<U>& aRhs);
  template<typename U>
  CheckedInt& operator %=(U aRhs);
  CheckedInt& operator %=(const CheckedInt<T>& aRhs);

  CheckedInt operator -() const
  {
    return detail::NegateImpl<T>::negate(*this);
  }

  /**
   * @returns true if the left and right hand sides are valid
   * and have the same value.
   *
   * Note that these semantics are the reason why we don't offer
   * a operator!=. Indeed, we'd want to have a!=b be equivalent to !(a==b)
   * but that would mean that whenever a or b is invalid, a!=b
   * is always true, which would be very confusing.
   *
   * For similar reasons, operators <, >, <=, >= would be very tricky to
   * specify, so we just avoid offering them.
   *
   * Notice that these == semantics are made more reasonable by these facts:
   *  1. a==b implies equality at the raw data level
   *     (the converse is false, as a==b is never true among invalids)
   *  2. This is similar to the behavior of IEEE floats, where a==b
   *     means that a and b have the same value *and* neither is NaN.
   */
  bool operator ==(const CheckedInt& aOther) const
  {
    return mIsValid && aOther.mIsValid && mValue == aOther.mValue;
  }

  /** prefix ++ */
  CheckedInt& operator++()
  {
    *this += 1;
    return *this;
  }

  /** postfix ++ */
  CheckedInt operator++(int)
  {
    CheckedInt tmp = *this;
    *this += 1;
    return tmp;
  }

  /** prefix -- */
  CheckedInt& operator--()
  {
    *this -= 1;
    return *this;
  }

  /** postfix -- */
  CheckedInt operator--(int)
  {
    CheckedInt tmp = *this;
    *this -= 1;
    return tmp;
  }

private:
  /**
   * The !=, <, <=, >, >= operators are disabled:
   * see the comment on operator==.
   */
  template<typename U> bool operator !=(U aOther) const = delete;
  template<typename U> bool operator < (U aOther) const = delete;
  template<typename U> bool operator <=(U aOther) const = delete;
  template<typename U> bool operator > (U aOther) const = delete;
  template<typename U> bool operator >=(U aOther) const = delete;
};

#define MOZ_CHECKEDINT_BASIC_BINARY_OPERATOR(NAME, OP)                        \
  template<typename T>                                                        \
  inline CheckedInt<T>                                                        \
  operator OP(const CheckedInt<T>& aLhs, const CheckedInt<T>& aRhs)           \
  {                                                                           \
    if (!detail::Is##NAME##Valid(aLhs.mValue, aRhs.mValue)) {                 \
      return CheckedInt<T>(0, false);                                         \
    }                                                                         \
    return CheckedInt<T>(aLhs.mValue OP aRhs.mValue,                          \
                         aLhs.mIsValid && aRhs.mIsValid);                     \
  }

#if MOZ_HAS_BUILTIN_OP_OVERFLOW
#define MOZ_CHECKEDINT_BASIC_BINARY_OPERATOR2(NAME, OP, FUN)                  \
  template<typename T>                                                        \
  inline CheckedInt<T>                                                        \
  operator OP(const CheckedInt<T>& aLhs, const CheckedInt<T>& aRhs)           \
  {                                                                           \
    T result;                                                                 \
    if (FUN(aLhs.mValue, aRhs.mValue, &result)) {                             \
      return CheckedInt<T>(0, false);                                         \
    }                                                                         \
    return CheckedInt<T>(result, aLhs.mIsValid && aRhs.mIsValid);             \
  }
MOZ_CHECKEDINT_BASIC_BINARY_OPERATOR2(Add, +, __builtin_add_overflow)
MOZ_CHECKEDINT_BASIC_BINARY_OPERATOR2(Sub, -, __builtin_sub_overflow)
MOZ_CHECKEDINT_BASIC_BINARY_OPERATOR2(Mul, *, __builtin_mul_overflow)
#undef MOZ_CHECKEDINT_BASIC_BINARY_OPERATOR2
#else
MOZ_CHECKEDINT_BASIC_BINARY_OPERATOR(Add, +)
MOZ_CHECKEDINT_BASIC_BINARY_OPERATOR(Sub, -)
MOZ_CHECKEDINT_BASIC_BINARY_OPERATOR(Mul, *)
#endif

MOZ_CHECKEDINT_BASIC_BINARY_OPERATOR(Div, /)
MOZ_CHECKEDINT_BASIC_BINARY_OPERATOR(Mod, %)
#undef MOZ_CHECKEDINT_BASIC_BINARY_OPERATOR

// Implement castToCheckedInt<T>(x), making sure that
//  - it allows x to be either a CheckedInt<T> or any integer type
//    that can be casted to T
//  - if x is already a CheckedInt<T>, we just return a reference to it,
//    instead of copying it (optimization)

namespace detail {

template<typename T, typename U>
struct CastToCheckedIntImpl
{
  typedef CheckedInt<T> ReturnType;
  static CheckedInt<T> run(U aU) { return aU; }
};

template<typename T>
struct CastToCheckedIntImpl<T, CheckedInt<T> >
{
  typedef const CheckedInt<T>& ReturnType;
  static const CheckedInt<T>& run(const CheckedInt<T>& aU) { return aU; }
};

} // namespace detail

template<typename T, typename U>
inline typename detail::CastToCheckedIntImpl<T, U>::ReturnType
castToCheckedInt(U aU)
{
  static_assert(detail::IsSupported<T>::value &&
                detail::IsSupported<U>::value,
                "This type is not supported by CheckedInt");
  return detail::CastToCheckedIntImpl<T, U>::run(aU);
}

#define MOZ_CHECKEDINT_CONVENIENCE_BINARY_OPERATORS(OP, COMPOUND_OP)            \
  template<typename T>                                                          \
  template<typename U>                                                          \
  CheckedInt<T>& CheckedInt<T>::operator COMPOUND_OP(U aRhs)                    \
  {                                                                             \
    *this = *this OP castToCheckedInt<T>(aRhs);                                 \
    return *this;                                                               \
  }                                                                             \
  template<typename T>                                                          \
  CheckedInt<T>& CheckedInt<T>::operator COMPOUND_OP(const CheckedInt<T>& aRhs) \
  {                                                                             \
    *this = *this OP aRhs;                                                      \
    return *this;                                                               \
  }                                                                             \
  template<typename T, typename U>                                              \
  inline CheckedInt<T> operator OP(const CheckedInt<T>& aLhs, U aRhs)           \
  {                                                                             \
    return aLhs OP castToCheckedInt<T>(aRhs);                                   \
  }                                                                             \
  template<typename T, typename U>                                              \
  inline CheckedInt<T> operator OP(U aLhs, const CheckedInt<T>& aRhs)           \
  {                                                                             \
    return castToCheckedInt<T>(aLhs) OP aRhs;                                   \
  }

MOZ_CHECKEDINT_CONVENIENCE_BINARY_OPERATORS(+, +=)
MOZ_CHECKEDINT_CONVENIENCE_BINARY_OPERATORS(*, *=)
MOZ_CHECKEDINT_CONVENIENCE_BINARY_OPERATORS(-, -=)
MOZ_CHECKEDINT_CONVENIENCE_BINARY_OPERATORS(/, /=)
MOZ_CHECKEDINT_CONVENIENCE_BINARY_OPERATORS(%, %=)

#undef MOZ_CHECKEDINT_CONVENIENCE_BINARY_OPERATORS

template<typename T, typename U>
inline bool
operator ==(const CheckedInt<T>& aLhs, U aRhs)
{
  return aLhs == castToCheckedInt<T>(aRhs);
}

template<typename T, typename U>
inline bool
operator ==(U aLhs, const CheckedInt<T>& aRhs)
{
  return castToCheckedInt<T>(aLhs) == aRhs;
}

// Convenience typedefs.
typedef CheckedInt<int8_t>   CheckedInt8;
typedef CheckedInt<uint8_t>  CheckedUint8;
typedef CheckedInt<int16_t>  CheckedInt16;
typedef CheckedInt<uint16_t> CheckedUint16;
typedef CheckedInt<int32_t>  CheckedInt32;
typedef CheckedInt<uint32_t> CheckedUint32;
typedef CheckedInt<int64_t>  CheckedInt64;
typedef CheckedInt<uint64_t> CheckedUint64;

} // namespace mozilla

#endif /* mozilla_CheckedInt_h */
