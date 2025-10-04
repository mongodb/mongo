/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_FixedList_h
#define jit_FixedList_h

#include "mozilla/Assertions.h"
#include "mozilla/Likely.h"

#include <stddef.h>

#include "jit/JitAllocPolicy.h"
#include "js/Utility.h"

namespace js {
namespace jit {

// List of a fixed length, but the length is unknown until runtime.
template <typename T>
class FixedList {
  T* list_;
  size_t length_;

 private:
  FixedList(const FixedList&);       // no copy definition.
  void operator=(const FixedList*);  // no assignment definition.

 public:
  FixedList() : list_(nullptr), length_(0) {}

  // Dynamic memory allocation requires the ability to report failure.
  [[nodiscard]] bool init(TempAllocator& alloc, size_t length) {
    if (length == 0) {
      return true;
    }

    list_ = alloc.allocateArray<T>(length);
    if (!list_) {
      return false;
    }

    length_ = length;
    return true;
  }

  size_t empty() const { return length_ == 0; }

  size_t length() const { return length_; }

  void shrink(size_t num) {
    MOZ_ASSERT(num < length_);
    length_ -= num;
  }

  [[nodiscard]] bool growBy(TempAllocator& alloc, size_t num) {
    size_t newlength = length_ + num;
    if (newlength < length_) {
      return false;
    }
    size_t bytes;
    if (MOZ_UNLIKELY(!CalculateAllocSize<T>(newlength, &bytes))) {
      return false;
    }
    T* list = (T*)alloc.allocate(bytes);
    if (MOZ_UNLIKELY(!list)) {
      return false;
    }

    for (size_t i = 0; i < length_; i++) {
      list[i] = list_[i];
    }

    length_ += num;
    list_ = list;
    return true;
  }

  T& operator[](size_t index) {
    MOZ_ASSERT(index < length_);
    return list_[index];
  }
  const T& operator[](size_t index) const {
    MOZ_ASSERT(index < length_);
    return list_[index];
  }

  T* data() { return list_; }

  T* begin() { return list_; }
  T* end() { return list_ + length_; }
};

}  // namespace jit
}  // namespace js

#endif /* jit_FixedList_h */
