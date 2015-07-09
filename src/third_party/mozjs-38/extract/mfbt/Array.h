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

#include <stddef.h>

namespace mozilla {

template<typename T, size_t Length>
class Array
{
  T mArr[Length];

public:
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
