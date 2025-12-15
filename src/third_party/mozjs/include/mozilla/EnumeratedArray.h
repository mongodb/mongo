/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* EnumeratedArray is like Array, but indexed by a typed enum. */

#ifndef mozilla_EnumeratedArray_h
#define mozilla_EnumeratedArray_h

#include <utility>

#include "mozilla/Array.h"
#include "EnumTypeTraits.h"

namespace mozilla {

/**
 * EnumeratedArray is a fixed-size array container for use when an
 * array is indexed by a specific enum class.
 *
 * This provides type safety by guarding at compile time against accidentally
 * indexing such arrays with unrelated values. This also removes the need
 * for manual casting when using a typed enum value to index arrays.
 *
 * Aside from the typing of indices, EnumeratedArray is similar to Array.
 *
 * Example:
 *
 *   enum class AnimalSpecies {
 *     Cow,
 *     Sheep,
 *     Count
 *   };
 *
 *   EnumeratedArray<AnimalSpecies, int, AnimalSpecies::Count> headCount;
 *
 *   headCount[AnimalSpecies::Cow] = 17;
 *   headCount[AnimalSpecies::Sheep] = 30;
 *
 * If the enum class has contiguous values and provides a specialization of
 * mozilla::MaxContiguousEnumValue then the size will be calculated as the max
 * value + 1.
 */
template <typename Enum, typename ValueType,
          size_t Size = ContiguousEnumSize<Enum>::value>
class EnumeratedArray {
 private:
  static_assert(UnderlyingValue(MinContiguousEnumValue<Enum>::value) == 0,
                "All indexes would need to be corrected if min != 0");

  using ArrayType = Array<ValueType, Size>;

  ArrayType mArray;

 public:
  constexpr EnumeratedArray() = default;

  template <typename... Args>
  MOZ_IMPLICIT constexpr EnumeratedArray(Args&&... aArgs)
      : mArray{std::forward<Args>(aArgs)...} {}

  constexpr ValueType& operator[](Enum aIndex) {
    return mArray[size_t(aIndex)];
  }

  constexpr const ValueType& operator[](Enum aIndex) const {
    return mArray[size_t(aIndex)];
  }

  using iterator = typename ArrayType::iterator;
  using const_iterator = typename ArrayType::const_iterator;
  using reverse_iterator = typename ArrayType::reverse_iterator;
  using const_reverse_iterator = typename ArrayType::const_reverse_iterator;

  // Methods for range-based for loops.
  iterator begin() { return mArray.begin(); }
  const_iterator begin() const { return mArray.begin(); }
  const_iterator cbegin() const { return mArray.cbegin(); }
  iterator end() { return mArray.end(); }
  const_iterator end() const { return mArray.end(); }
  const_iterator cend() const { return mArray.cend(); }

  // Method for std::size.
  constexpr size_t size() const { return mArray.size(); }

  // Methods for reverse iterating.
  reverse_iterator rbegin() { return mArray.rbegin(); }
  const_reverse_iterator rbegin() const { return mArray.rbegin(); }
  const_reverse_iterator crbegin() const { return mArray.crbegin(); }
  reverse_iterator rend() { return mArray.rend(); }
  const_reverse_iterator rend() const { return mArray.rend(); }
  const_reverse_iterator crend() const { return mArray.crend(); }
};

}  // namespace mozilla

#endif  // mozilla_EnumeratedArray_h
