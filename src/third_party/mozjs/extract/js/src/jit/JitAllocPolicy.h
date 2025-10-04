/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_JitAllocPolicy_h
#define jit_JitAllocPolicy_h

#include "mozilla/Assertions.h"
#include "mozilla/Attributes.h"
#include "mozilla/Likely.h"
#include "mozilla/OperatorNewExtensions.h"
#include "mozilla/TemplateLib.h"

#include <algorithm>
#include <stddef.h>
#include <string.h>
#include <type_traits>
#include <utility>

#include "ds/LifoAlloc.h"
#include "jit/InlineList.h"
#include "js/Utility.h"

namespace js {
namespace jit {

class TempAllocator {
  LifoAllocScope lifoScope_;

 public:
  // Most infallible JIT allocations are small, so we use a ballast of 16
  // KiB. And with a ballast of 16 KiB, a chunk size of 32 KiB works well,
  // because TempAllocators with a peak allocation size of less than 16 KiB
  // (which is most of them) only have to allocate a single chunk.
  static const size_t BallastSize;             // 16 KiB
  static const size_t PreferredLifoChunkSize;  // 32 KiB

  explicit TempAllocator(LifoAlloc* lifoAlloc) : lifoScope_(lifoAlloc) {
    lifoAlloc->setAsInfallibleByDefault();
  }

  void* allocateInfallible(size_t bytes) {
    return lifoScope_.alloc().allocInfallible(bytes);
  }

  [[nodiscard]] void* allocate(size_t bytes) {
    LifoAlloc::AutoFallibleScope fallibleAllocator(lifoAlloc());
    return lifoScope_.alloc().allocEnsureUnused(bytes, BallastSize);
  }

  template <typename T>
  [[nodiscard]] T* allocateArray(size_t n) {
    LifoAlloc::AutoFallibleScope fallibleAllocator(lifoAlloc());
    size_t bytes;
    if (MOZ_UNLIKELY(!CalculateAllocSize<T>(n, &bytes))) {
      return nullptr;
    }
    return static_cast<T*>(
        lifoScope_.alloc().allocEnsureUnused(bytes, BallastSize));
  }

  // View this allocator as a fallible allocator.
  struct Fallible {
    TempAllocator& alloc;
  };
  Fallible fallible() { return {*this}; }

  LifoAlloc* lifoAlloc() { return &lifoScope_.alloc(); }

  [[nodiscard]] bool ensureBallast() {
    JS_OOM_POSSIBLY_FAIL_BOOL();
    return lifoScope_.alloc().ensureUnusedApproximate(BallastSize);
  }
};

class JitAllocPolicy {
  TempAllocator& alloc_;

 public:
  MOZ_IMPLICIT JitAllocPolicy(TempAllocator& alloc) : alloc_(alloc) {}
  template <typename T>
  T* maybe_pod_malloc(size_t numElems) {
    size_t bytes;
    if (MOZ_UNLIKELY(!CalculateAllocSize<T>(numElems, &bytes))) {
      return nullptr;
    }
    return static_cast<T*>(alloc_.allocate(bytes));
  }
  template <typename T>
  T* maybe_pod_calloc(size_t numElems) {
    T* p = maybe_pod_malloc<T>(numElems);
    if (MOZ_LIKELY(p)) {
      memset(p, 0, numElems * sizeof(T));
    }
    return p;
  }
  template <typename T>
  T* maybe_pod_realloc(T* p, size_t oldSize, size_t newSize) {
    T* n = pod_malloc<T>(newSize);
    if (MOZ_UNLIKELY(!n)) {
      return n;
    }
    MOZ_ASSERT(!(oldSize & mozilla::tl::MulOverflowMask<sizeof(T)>::value));
    memcpy(n, p, std::min(oldSize * sizeof(T), newSize * sizeof(T)));
    return n;
  }
  template <typename T>
  T* pod_malloc(size_t numElems) {
    return maybe_pod_malloc<T>(numElems);
  }
  template <typename T>
  T* pod_calloc(size_t numElems) {
    return maybe_pod_calloc<T>(numElems);
  }
  template <typename T>
  T* pod_realloc(T* ptr, size_t oldSize, size_t newSize) {
    return maybe_pod_realloc<T>(ptr, oldSize, newSize);
  }
  template <typename T>
  void free_(T* p, size_t numElems = 0) {}
  void reportAllocOverflow() const {}
  [[nodiscard]] bool checkSimulatedOOM() const {
    return !js::oom::ShouldFailWithOOM();
  }
};

struct TempObject {
  inline void* operator new(size_t nbytes,
                            TempAllocator::Fallible view) noexcept(true) {
    return view.alloc.allocate(nbytes);
  }
  inline void* operator new(size_t nbytes, TempAllocator& alloc) {
    return alloc.allocateInfallible(nbytes);
  }
  template <class T>
  inline void* operator new(size_t nbytes, T* pos) {
    static_assert(std::is_convertible_v<T*, TempObject*>,
                  "Placement new argument type must inherit from TempObject");
    return pos;
  }
  template <class T>
  inline void* operator new(size_t nbytes, mozilla::NotNullTag, T* pos) {
    static_assert(std::is_convertible_v<T*, TempObject*>,
                  "Placement new argument type must inherit from TempObject");
    MOZ_ASSERT(pos);
    return pos;
  }
};

template <typename T>
class TempObjectPool {
  TempAllocator* alloc_;
  InlineForwardList<T> freed_;

 public:
  TempObjectPool() : alloc_(nullptr) {}
  void setAllocator(TempAllocator& alloc) {
    MOZ_ASSERT(freed_.empty());
    alloc_ = &alloc;
  }
  template <typename... Args>
  T* allocate(Args&&... args) {
    MOZ_ASSERT(alloc_);
    if (freed_.empty()) {
      return new (alloc_->fallible()) T(std::forward<Args>(args)...);
    }
    T* res = freed_.popFront();
    return new (res) T(std::forward<Args>(args)...);
  }
  void free(T* obj) { freed_.pushFront(obj); }
  void clear() { freed_.clear(); }
};

}  // namespace jit
}  // namespace js

#endif /* jit_JitAllocPolicy_h */
