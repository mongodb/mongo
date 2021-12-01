/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* A compile-time constant-length array with bounds-checking assertions. */

#ifndef mozilla_Array_h
#define mozilla_Array_h

#include "mozilla/Assertions.h"
#include "mozilla/Attributes.h"
#include "mozilla/Move.h"
#include "mozilla/ReverseIterator.h"

#include <stddef.h>

namespace mozilla {

template<typename T, size_t Length>
class Array
{
  T mArr[Length];

public:
  Array() {}

  template <typename... Args>
  MOZ_IMPLICIT Array(Args&&... aArgs)
    : mArr{mozilla::Forward<Args>(aArgs)...}
  {
    static_assert(sizeof...(aArgs) == Length,
                  "The number of arguments should be equal to the template parameter Length");
  }

  T& operator[](size_t aIndex)
  {
    MOZ_ASSERT(aIndex < Length);
    return mArr[aIndex];
  }

  const T& operator[](size_t aIndex) const
  {
    MOZ_ASSERT(aIndex < Length);
    return mArr[aIndex];
  }

  bool operator==(const Array<T, Length>& aOther) const
  {
    for (size_t i = 0; i < Length; i++) {
      if (mArr[i] != aOther[i]) {
        return false;
      }
    }
    return true;
  }

  typedef T*                        iterator;
  typedef const T*                  const_iterator;
  typedef ReverseIterator<T*>       reverse_iterator;
  typedef ReverseIterator<const T*> const_reverse_iterator;

  // Methods for range-based for loops.
  iterator begin() { return mArr; }
  const_iterator begin() const { return mArr; }
  const_iterator cbegin() const { return begin(); }
  iterator end() { return mArr + Length; }
  const_iterator end() const { return mArr + Length; }
  const_iterator cend() const { return end(); }

  // Methods for reverse iterating.
  reverse_iterator rbegin() { return reverse_iterator(end()); }
  const_reverse_iterator rbegin() const { return const_reverse_iterator(end()); }
  const_reverse_iterator crbegin() const { return rbegin(); }
  reverse_iterator rend() { return reverse_iterator(begin()); }
  const_reverse_iterator rend() const { return const_reverse_iterator(begin()); }
  const_reverse_iterator crend() const { return rend(); }
};

template<typename T>
class Array<T, 0>
{
public:
  T& operator[](size_t aIndex)
  {
    MOZ_CRASH("indexing into zero-length array");
  }

  const T& operator[](size_t aIndex) const
  {
    MOZ_CRASH("indexing into zero-length array");
  }
};

}  /* namespace mozilla */

#endif /* mozilla_Array_h */
