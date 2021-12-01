/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Provides saturation arithmetics for scalar types. */

#ifndef mozilla_Saturate_h
#define mozilla_Saturate_h

#include "mozilla/Attributes.h"
#include "mozilla/Move.h"
#include "mozilla/TypeTraits.h"

#include <limits>

namespace mozilla {
namespace detail {

/**
 * |SaturateOp<T>| wraps scalar values for saturation arithmetics. Usage:
 *
 *    uint32_t value = 1;
 *
 *    ++SaturateOp<uint32_t>(value); // value is 2
 *    --SaturateOp<uint32_t>(value); // value is 1
 *    --SaturateOp<uint32_t>(value); // value is 0
 *    --SaturateOp<uint32_t>(value); // value is still 0
 *
 * Please add new operators when required.
 *
 * |SaturateOp<T>| will saturate at the minimum and maximum values of
 * type T. If you need other bounds, implement a clamped-type class and
 * specialize the type traits accordingly.
 */
template <typename T>
class SaturateOp
{
public:
  explicit SaturateOp(T& aValue)
    : mValue(aValue)
  {
    // We should actually check for |std::is_scalar<T>::value| to be
    // true, but this type trait is not available everywhere. Relax
    // this assertion if you want to use floating point values as well.
    static_assert(IsIntegral<T>::value,
                  "Integral type required in instantiation");
  }

  // Add and subtract operators

  T operator+(const T& aRhs) const
  {
    return T(mValue) += aRhs;
  }

  T operator-(const T& aRhs) const
  {
    return T(mValue) -= aRhs;
  }

  // Compound operators

  const T& operator+=(const T& aRhs) const
  {
    const T min = std::numeric_limits<T>::min();
    const T max = std::numeric_limits<T>::max();

    if (aRhs > static_cast<T>(0)) {
      mValue = (max - aRhs) < mValue ? max : mValue + aRhs;
    } else {
      mValue = (min - aRhs) > mValue ? min : mValue + aRhs;
    }
    return mValue;
  }

  const T& operator-=(const T& aRhs) const
  {
    const T min = std::numeric_limits<T>::min();
    const T max = std::numeric_limits<T>::max();

    if (aRhs > static_cast<T>(0)) {
      mValue = (min + aRhs) > mValue ? min : mValue - aRhs;
    } else {
      mValue = (max + aRhs) < mValue ? max : mValue - aRhs;
    }
    return mValue;
  }

  // Increment and decrement operators

  const T& operator++() const // prefix
  {
    return operator+=(static_cast<T>(1));
  }

  T operator++(int) const // postfix
  {
    const T value(mValue);
    operator++();
    return value;
  }

  const T& operator--() const // prefix
  {
    return operator-=(static_cast<T>(1));
  }

  T operator--(int) const // postfix
  {
    const T value(mValue);
    operator--();
    return value;
  }

private:
  SaturateOp(const SaturateOp<T>&) = delete;
  SaturateOp(SaturateOp<T>&&) = delete;
  SaturateOp& operator=(const SaturateOp<T>&) = delete;
  SaturateOp& operator=(SaturateOp<T>&&) = delete;

  T& mValue;
};

/**
 * |Saturate<T>| is a value type for saturation arithmetics. It's
 * build on top of |SaturateOp<T>|.
 */
template <typename T>
class Saturate
{
public:
  Saturate() = default;
  MOZ_IMPLICIT Saturate(const Saturate<T>&) = default;

  MOZ_IMPLICIT Saturate(Saturate<T>&& aValue)
  {
    mValue = Move(aValue.mValue);
  }

  explicit Saturate(const T& aValue)
    : mValue(aValue)
  { }

  const T& value() const
  {
    return mValue;
  }

  // Compare operators

  bool operator==(const Saturate<T>& aRhs) const
  {
    return mValue == aRhs.mValue;
  }

  bool operator!=(const Saturate<T>& aRhs) const
  {
    return !operator==(aRhs);
  }

  bool operator==(const T& aRhs) const
  {
    return mValue == aRhs;
  }

  bool operator!=(const T& aRhs) const
  {
    return !operator==(aRhs);
  }

  // Assignment operators

  Saturate<T>& operator=(const Saturate<T>&) = default;

  Saturate<T>& operator=(Saturate<T>&& aRhs)
  {
    mValue = Move(aRhs.mValue);
    return *this;
  }

  // Add and subtract operators

  Saturate<T> operator+(const Saturate<T>& aRhs) const
  {
    Saturate<T> lhs(mValue);
    return lhs += aRhs.mValue;
  }

  Saturate<T> operator+(const T& aRhs) const
  {
    Saturate<T> lhs(mValue);
    return lhs += aRhs;
  }

  Saturate<T> operator-(const Saturate<T>& aRhs) const
  {
    Saturate<T> lhs(mValue);
    return lhs -= aRhs.mValue;
  }

  Saturate<T> operator-(const T& aRhs) const
  {
    Saturate<T> lhs(mValue);
    return lhs -= aRhs;
  }

  // Compound operators

  Saturate<T>& operator+=(const Saturate<T>& aRhs)
  {
    SaturateOp<T>(mValue) += aRhs.mValue;
    return *this;
  }

  Saturate<T>& operator+=(const T& aRhs)
  {
    SaturateOp<T>(mValue) += aRhs;
    return *this;
  }

  Saturate<T>& operator-=(const Saturate<T>& aRhs)
  {
    SaturateOp<T>(mValue) -= aRhs.mValue;
    return *this;
  }

  Saturate<T>& operator-=(const T& aRhs)
  {
    SaturateOp<T>(mValue) -= aRhs;
    return *this;
  }

  // Increment and decrement operators

  Saturate<T>& operator++() // prefix
  {
    ++SaturateOp<T>(mValue);
    return *this;
  }

  Saturate<T> operator++(int) // postfix
  {
    return Saturate<T>(SaturateOp<T>(mValue)++);
  }

  Saturate<T>& operator--() // prefix
  {
    --SaturateOp<T>(mValue);
    return *this;
  }

  Saturate<T> operator--(int) // postfix
  {
    return Saturate<T>(SaturateOp<T>(mValue)--);
  }

private:
  T mValue;
};

} // namespace detail

typedef detail::Saturate<int8_t> SaturateInt8;
typedef detail::Saturate<int16_t> SaturateInt16;
typedef detail::Saturate<int32_t> SaturateInt32;
typedef detail::Saturate<uint8_t> SaturateUint8;
typedef detail::Saturate<uint16_t> SaturateUint16;
typedef detail::Saturate<uint32_t> SaturateUint32;

} // namespace mozilla

template<typename LhsT, typename RhsT>
bool
operator==(LhsT aLhs, const mozilla::detail::Saturate<RhsT>& aRhs)
{
  return aRhs.operator==(static_cast<RhsT>(aLhs));
}

template<typename LhsT, typename RhsT>
bool
operator!=(LhsT aLhs, const mozilla::detail::Saturate<RhsT>& aRhs)
{
  return !(aLhs == aRhs);
}

#endif // mozilla_Saturate_h
