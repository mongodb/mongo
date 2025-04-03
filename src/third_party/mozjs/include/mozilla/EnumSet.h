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
#include "mozilla/MathAlgorithms.h"

#include <initializer_list>
#include <type_traits>

#include <stdint.h>

namespace mozilla {

/**
 * EnumSet<T, U> is a set of values defined by an enumeration. It is implemented
 * using a bit mask with the size of U for each value. It works both for enum
 * and enum class types. EnumSet also works with U being a BitSet.
 */
template <typename T, typename Serialized = typename std::make_unsigned<
                          typename std::underlying_type<T>::type>::type>
class EnumSet {
 public:
  using valueType = T;
  using serializedType = Serialized;

  constexpr EnumSet() : mBitField() {}

  constexpr MOZ_IMPLICIT EnumSet(T aEnum) : mBitField(bitFor(aEnum)) {}

  constexpr EnumSet(T aEnum1, T aEnum2)
      : mBitField(bitFor(aEnum1) | bitFor(aEnum2)) {}

  constexpr EnumSet(T aEnum1, T aEnum2, T aEnum3)
      : mBitField(bitFor(aEnum1) | bitFor(aEnum2) | bitFor(aEnum3)) {}

  constexpr EnumSet(T aEnum1, T aEnum2, T aEnum3, T aEnum4)
      : mBitField(bitFor(aEnum1) | bitFor(aEnum2) | bitFor(aEnum3) |
                  bitFor(aEnum4)) {}

  constexpr MOZ_IMPLICIT EnumSet(std::initializer_list<T> list) : mBitField() {
    for (auto value : list) {
      (*this) += value;
    }
  }

#ifdef DEBUG
  constexpr EnumSet(const EnumSet& aEnumSet) : mBitField(aEnumSet.mBitField) {}

  constexpr EnumSet& operator=(const EnumSet& aEnumSet) {
    mBitField = aEnumSet.mBitField;
    incVersion();
    return *this;
  }
#endif

  /**
   * Add an element
   */
  constexpr void operator+=(T aEnum) {
    incVersion();
    mBitField |= bitFor(aEnum);
  }

  /**
   * Add an element
   */
  constexpr EnumSet operator+(T aEnum) const {
    EnumSet result(*this);
    result += aEnum;
    return result;
  }

  /**
   * Union
   */
  void operator+=(const EnumSet& aEnumSet) {
    incVersion();
    mBitField |= aEnumSet.mBitField;
  }

  /**
   * Union
   */
  EnumSet operator+(const EnumSet& aEnumSet) const {
    EnumSet result(*this);
    result += aEnumSet;
    return result;
  }

  /**
   * Remove an element
   */
  void operator-=(T aEnum) {
    incVersion();
    mBitField &= ~(bitFor(aEnum));
  }

  /**
   * Remove an element
   */
  EnumSet operator-(T aEnum) const {
    EnumSet result(*this);
    result -= aEnum;
    return result;
  }

  /**
   * Remove a set of elements
   */
  void operator-=(const EnumSet& aEnumSet) {
    incVersion();
    mBitField &= ~(aEnumSet.mBitField);
  }

  /**
   * Remove a set of elements
   */
  EnumSet operator-(const EnumSet& aEnumSet) const {
    EnumSet result(*this);
    result -= aEnumSet;
    return result;
  }

  /**
   * Clear
   */
  void clear() {
    incVersion();
    mBitField = Serialized();
  }

  /**
   * Intersection
   */
  void operator&=(const EnumSet& aEnumSet) {
    incVersion();
    mBitField &= aEnumSet.mBitField;
  }

  /**
   * Intersection
   */
  EnumSet operator&(const EnumSet& aEnumSet) const {
    EnumSet result(*this);
    result &= aEnumSet;
    return result;
  }

  /**
   * Equality
   */
  bool operator==(const EnumSet& aEnumSet) const {
    return mBitField == aEnumSet.mBitField;
  }

  /**
   * Equality
   */
  bool operator==(T aEnum) const { return mBitField == bitFor(aEnum); }

  /**
   * Not equal
   */
  bool operator!=(const EnumSet& aEnumSet) const {
    return !operator==(aEnumSet);
  }

  /**
   * Not equal
   */
  bool operator!=(T aEnum) const { return !operator==(aEnum); }

  /**
   * Test is an element is contained in the set.
   */
  bool contains(T aEnum) const {
    return static_cast<bool>(mBitField & bitFor(aEnum));
  }

  /**
   * Test if a set is contained in the set.
   */
  bool contains(const EnumSet& aEnumSet) const {
    return (mBitField & aEnumSet.mBitField) == aEnumSet.mBitField;
  }

  /**
   * Return the number of elements in the set.
   */
  size_t size() const {
    if constexpr (std::is_unsigned_v<Serialized>) {
      if constexpr (kMaxBits > 32) {
        return CountPopulation64(mBitField);
      } else {
        return CountPopulation32(mBitField);
      }
    } else {
      return mBitField.Count();
    }
  }

  bool isEmpty() const {
    if constexpr (std::is_unsigned_v<Serialized>) {
      return mBitField == 0;
    } else {
      return mBitField.IsEmpty();
    }
  }

  Serialized serialize() const { return mBitField; }

  void deserialize(Serialized aValue) {
    incVersion();
    mBitField = aValue;
  }

  class ConstIterator {
    const EnumSet* mSet;
    uint32_t mPos;
#ifdef DEBUG
    uint64_t mVersion;
#endif

    void checkVersion() const {
      // Check that the set has not been modified while being iterated.
      MOZ_ASSERT_IF(mSet, mSet->mVersion == mVersion);
    }

   public:
    ConstIterator(const EnumSet& aSet, uint32_t aPos)
        : mSet(&aSet), mPos(aPos) {
#ifdef DEBUG
      mVersion = mSet->mVersion;
#endif
      MOZ_ASSERT(aPos <= kMaxBits);
      if (aPos != kMaxBits && !mSet->contains(T(mPos))) {
        ++*this;
      }
    }

    ConstIterator(const ConstIterator& aOther)
        : mSet(aOther.mSet), mPos(aOther.mPos) {
#ifdef DEBUG
      mVersion = aOther.mVersion;
      checkVersion();
#endif
    }

    ConstIterator(ConstIterator&& aOther)
        : mSet(aOther.mSet), mPos(aOther.mPos) {
#ifdef DEBUG
      mVersion = aOther.mVersion;
      checkVersion();
#endif
      aOther.mSet = nullptr;
    }

    ~ConstIterator() { checkVersion(); }

    bool operator==(const ConstIterator& other) const {
      MOZ_ASSERT(mSet == other.mSet);
      checkVersion();
      return mPos == other.mPos;
    }

    bool operator!=(const ConstIterator& other) const {
      return !(*this == other);
    }

    T operator*() const {
      MOZ_ASSERT(mSet);
      MOZ_ASSERT(mPos < kMaxBits);
      MOZ_ASSERT(mSet->contains(T(mPos)));
      checkVersion();
      return T(mPos);
    }

    ConstIterator& operator++() {
      MOZ_ASSERT(mSet);
      MOZ_ASSERT(mPos < kMaxBits);
      checkVersion();
      do {
        mPos++;
      } while (mPos < kMaxBits && !mSet->contains(T(mPos)));
      return *this;
    }
  };

  ConstIterator begin() const { return ConstIterator(*this, 0); }

  ConstIterator end() const { return ConstIterator(*this, kMaxBits); }

 private:
  constexpr static Serialized bitFor(T aEnum) {
    auto bitNumber = static_cast<size_t>(aEnum);
    MOZ_DIAGNOSTIC_ASSERT(bitNumber < kMaxBits);
    if constexpr (std::is_unsigned_v<Serialized>) {
      return static_cast<Serialized>(Serialized{1} << bitNumber);
    } else {
      Serialized bitField;
      bitField[bitNumber] = true;
      return bitField;
    }
  }

  constexpr void incVersion() {
#ifdef DEBUG
    mVersion++;
#endif
  }

  static constexpr size_t MaxBits() {
    if constexpr (std::is_unsigned_v<Serialized>) {
      return sizeof(Serialized) * 8;
    } else {
      return Serialized::Size();
    }
  }

  static constexpr size_t kMaxBits = MaxBits();

  Serialized mBitField;

#ifdef DEBUG
  uint64_t mVersion = 0;
#endif
};

}  // namespace mozilla

#endif /* mozilla_EnumSet_h_*/
