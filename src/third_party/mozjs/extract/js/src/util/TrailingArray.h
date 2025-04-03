/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef util_TrailingArray_h
#define util_TrailingArray_h

#include "mozilla/Assertions.h"  // MOZ_ASSERT

#include <stddef.h>  // size_t
#include <stdint.h>  // uint32_t, uintptr_t

namespace js {

// This is a mixin class to use for types that have trailing arrays and use
// offsets to delimit them. It provides helper methods to do casting and
// initialization while avoiding C++ undefined behaviour.
class TrailingArray {
 protected:
  // Offsets are measured in bytes relative to 'this'.
  using Offset = uint32_t;

  // Test if offset is correctly aligned for type.
  template <typename T>
  static constexpr bool isAlignedOffset(Offset offset) {
    return offset % alignof(T) == 0;
  }
  template <size_t N>
  static constexpr bool isAlignedOffset(Offset offset) {
    return offset % N == 0;
  }

  // Translate an offset into a concrete pointer.
  template <typename T>
  T* offsetToPointer(Offset offset) {
    uintptr_t base = reinterpret_cast<uintptr_t>(this);
    return reinterpret_cast<T*>(base + offset);
  }
  template <typename T>
  const T* offsetToPointer(Offset offset) const {
    uintptr_t base = reinterpret_cast<uintptr_t>(this);
    return reinterpret_cast<const T*>(base + offset);
  }

  // Placement-new the elements of an array. This optimizes away for types with
  // trivial default initialization and plays nicely with compiler vectorization
  // passes.
  template <typename T>
  void initElements(Offset offset, size_t nelem) {
    MOZ_ASSERT(isAlignedOffset<T>(offset));

    // Address of first array element.
    uintptr_t elem = reinterpret_cast<uintptr_t>(this) + offset;

    for (size_t i = 0; i < nelem; ++i) {
      void* raw = reinterpret_cast<void*>(elem);
      new (raw) T;
      elem += sizeof(T);
    }
  }

  // Compute the length of an array from its start and end offset.
  template <typename T>
  size_t numElements(Offset start, Offset end) const {
    constexpr size_t ElemSize = sizeof(T);
    return numElements<ElemSize>(start, end);
  }
  template <size_t ElemSize>
  size_t numElements(Offset start, Offset end) const {
    MOZ_ASSERT(start <= end);
    MOZ_ASSERT((end - start) % ElemSize == 0);
    return (end - start) / ElemSize;
  }

  // Constructor is protected so a derived type is required.
  TrailingArray() = default;

 public:
  // Type has trailing data so isn't copyable or movable.
  TrailingArray(const TrailingArray&) = delete;
  TrailingArray& operator=(const TrailingArray&) = delete;
};

}  // namespace js

#endif  // util_TrailingArray_h
