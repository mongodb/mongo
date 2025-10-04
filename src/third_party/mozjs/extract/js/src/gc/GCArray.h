/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef js_GCArray_h
#define js_GCArray_h

#include "mozilla/Assertions.h"

#include "gc/Barrier.h"
#include "gc/Tracer.h"
#include "js/Value.h"

namespace js {

/*
 * A fixed size array of |T| for use with GC things.
 *
 * Must be allocated manually to allow space for the trailing elements
 * data. Call bytesRequired to get the required allocation size.
 *
 * Does not provide any barriers by default.
 */
template <typename T>
class GCArray {
  const uint32_t size_;
  T elements_[1];

 public:
  explicit GCArray(uint32_t size) : size_(size) {
    // The array contents following the first element are not initialized.
  }

  uint32_t size() const { return size_; }

  const T& operator[](uint32_t i) const {
    MOZ_ASSERT(i < size_);
    return elements_[i];
  }
  T& operator[](uint32_t i) {
    MOZ_ASSERT(i < size_);
    return elements_[i];
  }

  const T* begin() const { return elements_; }
  T* begin() { return elements_; }
  const T* end() const { return elements_ + size_; }
  T* end() { return elements_ + size_; }

  void trace(JSTracer* trc) {
    TraceRange(trc, size(), begin(), "array element");
  }

  static constexpr ptrdiff_t offsetOfElements() {
    return offsetof(GCArray, elements_);
  }

  static size_t bytesRequired(size_t size) {
    return offsetOfElements() + std::max(size, size_t(1)) * sizeof(T);
  }
};

/*
 * A fixed size array of GC things owned by a GC thing.
 *
 * Uses the appropriate barriers depending on whether the owner is in the
 * nursery or the tenured heap. If the owner cannot be allocated in the nursery
 * then this class is not required.
 */
template <typename T>
class GCOwnedArray {
  using StorageType = GCArray<PreAndPostBarrierWrapper<T>>;
  using TenuredInterface = StorageType;
  using NurseryInterface = GCArray<PreBarrierWrapper<T>>;

  StorageType array;

 public:
  explicit GCOwnedArray(uint32_t size) : array(size) {}

  uint32_t size() const { return array.size(); }
  const T& operator[](uint32_t i) const { return array[i].get(); }

  // Apply |f| to a view of the data with appropriate barriers given |owner|.
  template <typename F>
  void withOwner(gc::Cell* owner, F&& f) {
    if (gc::IsInsideNursery(owner)) {
      f(nurseryOwned());
    } else {
      f(tenuredOwned());
    }
  }

  // For convenience, special case setElement.
  void setElement(gc::Cell* owner, uint32_t i, const T& newValue) {
    withOwner(owner, [&](auto& self) { self[i] = newValue; });
  }

  void trace(JSTracer* trc) { array.trace(trc); }

  static constexpr ptrdiff_t offsetOfElements() {
    return offsetof(GCOwnedArray, array) + StorageType::offsetOfElements();
  }

  static size_t bytesRequired(size_t size) {
    return offsetof(GCOwnedArray, array) + StorageType::bytesRequired(size);
  }

 private:
  TenuredInterface& tenuredOwned() { return array; }
  NurseryInterface& nurseryOwned() {
    return reinterpret_cast<NurseryInterface&>(array);
  }
};

}  // namespace js

#endif  // js_GCArray_h
