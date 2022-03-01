/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef ds_FixedLengthVector_h
#define ds_FixedLengthVector_h

#include "mozilla/Assertions.h"             // MOZ_ASSERT
#include "mozilla/OperatorNewExtensions.h"  // mozilla::KnownNotNull

#include <stddef.h>  // size_t

#include "js/Utility.h"    // js_free
#include "vm/JSContext.h"  // JSContext

namespace js {

// A dynamically-allocated fixed-length vector with bounds checking assertions.
template <typename T>
class FixedLengthVector {
  // The pointer to the storage.
  T* data_ = nullptr;

  // The size of the storage.
  size_t length_ = 0;

 public:
  FixedLengthVector() = default;

  FixedLengthVector(FixedLengthVector&) = delete;
  FixedLengthVector(FixedLengthVector&&) = default;

  ~FixedLengthVector() {
    if (initialized()) {
      js_free(data_);
    }
  }

  size_t length() const { return length_; }

  bool initialized() const { return !!data_; }

  // Allocate the storage with the given size, wihtout calling constructor.
  //
  // If the allocation fails, this returns false and sets the
  // pending exception on the given context.
  [[nodiscard]] bool allocateUninitialized(JSContext* cx, size_t length) {
    MOZ_ASSERT(!initialized());

    length_ = length;
    data_ = cx->pod_malloc<T>(length);
    if (MOZ_UNLIKELY(!data_)) {
      return false;
    }

    return true;
  }

  // Allocate the storage with the given size and call default constructor.
  //
  // If the allocation fails, this returns false and sets the
  // pending exception on the given context.
  [[nodiscard]] bool allocate(JSContext* cx, size_t length) {
    if (!allocateUninitialized(cx, length)) {
      return false;
    }

    for (size_t i = 0; i < length; i++) {
      new (mozilla::KnownNotNull, &data_[i]) T();
    }
    return true;
  }

  T* begin() {
    MOZ_ASSERT(initialized());
    return data_;
  }

  const T* begin() const {
    MOZ_ASSERT(initialized());
    return data_;
  }

  T* end() {
    MOZ_ASSERT(initialized());
    return data_ + length_;
  }

  const T* end() const {
    MOZ_ASSERT(initialized());
    return data_ + length_;
  }

  T& operator[](size_t index) {
    MOZ_ASSERT(initialized());
    MOZ_ASSERT(index < length_);
    return begin()[index];
  }

  const T& operator[](size_t index) const {
    MOZ_ASSERT(initialized());
    MOZ_ASSERT(index < length_);
    return begin()[index];
  }
};

}  // namespace js

#endif  // ds_FixedLengthVector_h
