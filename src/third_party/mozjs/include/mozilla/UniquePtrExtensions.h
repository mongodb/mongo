/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Useful extensions to UniquePtr. */

#ifndef mozilla_UniquePtrExtensions_h
#define mozilla_UniquePtrExtensions_h

#include "mozilla/fallible.h"
#include "mozilla/UniquePtr.h"

namespace mozilla {

/**
 * MakeUniqueFallible works exactly like MakeUnique, except that the memory
 * allocation performed is done fallibly, i.e. it can return nullptr.
 */
template<typename T, typename... Args>
typename detail::UniqueSelector<T>::SingleObject
MakeUniqueFallible(Args&&... aArgs)
{
  return UniquePtr<T>(new (fallible) T(Forward<Args>(aArgs)...));
}

template<typename T>
typename detail::UniqueSelector<T>::UnknownBound
MakeUniqueFallible(decltype(sizeof(int)) aN)
{
  typedef typename RemoveExtent<T>::Type ArrayType;
  return UniquePtr<T>(new (fallible) ArrayType[aN]());
}

template<typename T, typename... Args>
typename detail::UniqueSelector<T>::KnownBound
MakeUniqueFallible(Args&&... aArgs) = delete;

namespace detail {

template<typename T>
struct FreePolicy
{
  void operator()(const void* ptr) {
    free(const_cast<void*>(ptr));
  }
};

} // namespace detail

template<typename T>
using UniqueFreePtr = UniquePtr<T, detail::FreePolicy<T>>;

} // namespace mozilla

#endif // mozilla_UniquePtrExtensions_h
