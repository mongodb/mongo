/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef wasm_utility_h
#define wasm_utility_h

#include "mozilla/Maybe.h"

#include <algorithm>
namespace js {
namespace wasm {

template <class Container1, class Container2>
static inline bool EqualContainers(const Container1& lhs,
                                   const Container2& rhs) {
  return std::equal(lhs.begin(), lhs.end(), rhs.begin(), rhs.end());
}

#define WASM_DECLARE_POD_VECTOR(Type, VectorName)                      \
  }                                                                    \
  }                                                                    \
  static_assert(std::is_trivially_copyable<js::wasm::Type>::value,     \
                "Must be trivially copyable");                         \
  static_assert(std::is_trivially_destructible<js::wasm::Type>::value, \
                "Must be trivially destructible");                     \
  namespace js {                                                       \
  namespace wasm {                                                     \
  typedef Vector<Type, 0, SystemAllocPolicy> VectorName;

using mozilla::MallocSizeOf;

template <class T>
static inline size_t SizeOfVectorElementExcludingThis(
    const T& elem, MallocSizeOf mallocSizeOf) {
  return elem.sizeOfExcludingThis(mallocSizeOf);
}

template <class T>
static inline size_t SizeOfVectorElementExcludingThis(
    const RefPtr<T>& elem, MallocSizeOf mallocSizeOf) {
  return elem->sizeOfExcludingThis(mallocSizeOf);
}

template <class T, size_t N>
static inline size_t SizeOfVectorExcludingThis(
    const mozilla::Vector<T, N, SystemAllocPolicy>& vec,
    MallocSizeOf mallocSizeOf) {
  size_t size = vec.sizeOfExcludingThis(mallocSizeOf);
  for (const T& t : vec) {
    size += SizeOfVectorElementExcludingThis(t, mallocSizeOf);
  }
  return size;
}

template <class T>
static inline size_t SizeOfMaybeExcludingThis(const mozilla::Maybe<T>& maybe,
                                              MallocSizeOf mallocSizeOf) {
  return maybe ? maybe->sizeOfExcludingThis(mallocSizeOf) : 0;
}

}  // namespace wasm
}  // namespace js

#endif  // wasm_utility_h
