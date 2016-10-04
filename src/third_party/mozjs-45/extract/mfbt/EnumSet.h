/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* A set abstraction for enumeration values. */

#ifndef mozilla_EnumSet_h
#define mozilla_EnumSet_h

#include "mozilla/Assertions.h"
#include "mozilla/Attributes.h"

#include <stdint.h>

namespace mozilla {

/**
 * EnumSet<T> is a set of values defined by an enumeration. It is implemented
 * using a 32 bit mask for each value so it will only work for enums with an int
 * representation less than 32. It works both for enum and enum class types.
 */
template<typename T>
class EnumSet
{
public:
  EnumSet()
    : mBitField(0)
  { }

  MOZ_IMPLICIT EnumSet(T aEnum)
    : mBitField(bitFor(aEnum))
  { }

  EnumSet(T aEnum1, T aEnum2)
    : mBitField(bitFor(aEnum1) |
                bitFor(aEnum2))
  { }

  EnumSet(T aEnum1, T aEnum2, T aEnum3)
    : mBitField(bitFor(aEnum1) |
                bitFor(aEnum2) |
                bitFor(aEnum3))
  { }

  EnumSet(T aEnum1, T aEnum2, T aEnum3, T aEnum4)
   : mBitField(bitFor(aEnum1) |
               bitFor(aEnum2) |
               bitFor(aEnum3) |
               bitFor(aEnum4))
  { }

  EnumSet(const EnumSet& aEnumSet)
   : mBitField(aEnumSet.mBitField)
  { }

  /**
   * Add an element
   */
  void operator+=(T aEnum)
  {
    mBitField |= bitFor(aEnum);
  }

  /**
   * Add an element
   */
  EnumSet<T> operator+(T aEnum) const
  {
    EnumSet<T> result(*this);
    result += aEnum;
    return result;
  }

  /**
   * Union
   */
  void operator+=(const EnumSet<T> aEnumSet)
  {
    mBitField |= aEnumSet.mBitField;
  }

  /**
   * Union
   */
  EnumSet<T> operator+(const EnumSet<T> aEnumSet) const
  {
    EnumSet<T> result(*this);
    result += aEnumSet;
    return result;
  }

  /**
   * Remove an element
   */
  void operator-=(T aEnum)
  {
    mBitField &= ~(bitFor(aEnum));
  }

  /**
   * Remove an element
   */
  EnumSet<T> operator-(T aEnum) const
  {
    EnumSet<T> result(*this);
    result -= aEnum;
    return result;
  }

  /**
   * Remove a set of elements
   */
  void operator-=(const EnumSet<T> aEnumSet)
  {
    mBitField &= ~(aEnumSet.mBitField);
  }

  /**
   * Remove a set of elements
   */
  EnumSet<T> operator-(const EnumSet<T> aEnumSet) const
  {
    EnumSet<T> result(*this);
    result -= aEnumSet;
    return result;
  }

  /**
   * Clear
   */
  void clear()
  {
    mBitField = 0;
  }

  /**
   * Intersection
   */
  void operator&=(const EnumSet<T> aEnumSet)
  {
    mBitField &= aEnumSet.mBitField;
  }

  /**
   * Intersection
   */
  EnumSet<T> operator&(const EnumSet<T> aEnumSet) const
  {
    EnumSet<T> result(*this);
    result &= aEnumSet;
    return result;
  }

  /**
   * Equality
   */
  bool operator==(const EnumSet<T> aEnumSet) const
  {
    return mBitField == aEnumSet.mBitField;
  }

  /**
   * Test is an element is contained in the set.
   */
  bool contains(T aEnum) const
  {
    return mBitField & bitFor(aEnum);
  }

  /**
   * Return the number of elements in the set.
   */
  uint8_t size()
  {
    uint8_t count = 0;
    for (uint32_t bitField = mBitField; bitField; bitField >>= 1) {
      if (bitField & 1) {
        count++;
      }
    }
    return count;
  }

  bool isEmpty() const
  {
    return mBitField == 0;
  }

  uint32_t serialize() const
  {
    return mBitField;
  }

  void deserialize(uint32_t aValue)
  {
    mBitField = aValue;
  }

private:
  static uint32_t bitFor(T aEnum)
  {
    uint32_t bitNumber = (uint32_t)aEnum;
    MOZ_ASSERT(bitNumber < 32);
    return 1U << bitNumber;
  }

  uint32_t mBitField;
};

} // namespace mozilla

#endif /* mozilla_EnumSet_h_*/
