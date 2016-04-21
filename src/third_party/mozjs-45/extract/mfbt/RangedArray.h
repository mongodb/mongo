/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * A compile-time constant-length array, with bounds-checking assertions -- but
 * unlike mozilla::Array, with indexes biased by a constant.
 *
 * Thus where mozilla::Array<int, 3> is a three-element array indexed by [0, 3),
 * mozilla::RangedArray<int, 8, 3> is a three-element array indexed by [8, 11).
 */

#ifndef mozilla_RangedArray_h
#define mozilla_RangedArray_h

#include "mozilla/Array.h"

namespace mozilla {

template<typename T, size_t MinIndex, size_t Length>
class RangedArray
{
private:
  typedef Array<T, Length> ArrayType;
  ArrayType mArr;

public:
  T& operator[](size_t aIndex)
  {
    MOZ_ASSERT(aIndex == MinIndex || aIndex > MinIndex);
    return mArr[aIndex - MinIndex];
  }

  const T& operator[](size_t aIndex) const
  {
    MOZ_ASSERT(aIndex == MinIndex || aIndex > MinIndex);
    return mArr[aIndex - MinIndex];
  }

  typedef typename ArrayType::iterator               iterator;
  typedef typename ArrayType::const_iterator         const_iterator;
  typedef typename ArrayType::reverse_iterator       reverse_iterator;
  typedef typename ArrayType::const_reverse_iterator const_reverse_iterator;

  // Methods for range-based for loops.
  iterator begin() { return mArr.begin(); }
  const_iterator begin() const { return mArr.begin(); }
  const_iterator cbegin() const { return mArr.cbegin(); }
  iterator end() { return mArr.end(); }
  const_iterator end() const { return mArr.end(); }
  const_iterator cend() const { return mArr.cend(); }

  // Methods for reverse iterating.
  reverse_iterator rbegin() { return mArr.rbegin(); }
  const_reverse_iterator rbegin() const { return mArr.rbegin(); }
  const_reverse_iterator crbegin() const { return mArr.crbegin(); }
  reverse_iterator rend() { return mArr.rend(); }
  const_reverse_iterator rend() const { return mArr.rend(); }
  const_reverse_iterator crend() const { return mArr.crend(); }
};

} // namespace mozilla

#endif // mozilla_RangedArray_h
