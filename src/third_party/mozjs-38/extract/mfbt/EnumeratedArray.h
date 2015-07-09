/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* EnumeratedArray is like Array, but indexed by a typed enum. */

#ifndef mozilla_EnumeratedArray_h
#define mozilla_EnumeratedArray_h

#include "mozilla/Array.h"

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
 *   EnumeratedArray<AnimalSpecies, AnimalSpecies::Count, int> headCount;
 *
 *   headCount[AnimalSpecies::Cow] = 17;
 *   headCount[AnimalSpecies::Sheep] = 30;
 *
 */
template<typename IndexType,
         IndexType SizeAsEnumValue,
         typename ValueType>
class EnumeratedArray
{
public:
  static const size_t kSize = size_t(SizeAsEnumValue);

private:
  Array<ValueType, kSize> mArray;

public:
  EnumeratedArray() {}

  explicit EnumeratedArray(const EnumeratedArray& aOther)
  {
    for (size_t i = 0; i < kSize; i++) {
      mArray[i] = aOther.mArray[i];
    }
  }

  ValueType& operator[](IndexType aIndex)
  {
    return mArray[size_t(aIndex)];
  }

  const ValueType& operator[](IndexType aIndex) const
  {
    return mArray[size_t(aIndex)];
  }
};

} // namespace mozilla

#endif // mozilla_EnumeratedArray_h
