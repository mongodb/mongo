/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_OffthreadSnapshot_h
#define jit_OffthreadSnapshot_h

#include "gc/Policy.h"
#include "gc/Tracer.h"

// Wrapper for GC things stored in snapshots for offthread compilation.
// Asserts the GC pointer is not nursery-allocated.
// These pointers must be traced using TraceOffthreadGCPtr.
template <typename T>
class OffthreadGCPtr {
  // Note: no pre-barrier is needed because this is a constant. No post-barrier
  // is needed because the value is always tenured.
  const T ptr_;

 public:
  explicit OffthreadGCPtr(const T& ptr) : ptr_(ptr) {
    MOZ_ASSERT(JS::GCPolicy<T>::isTenured(ptr),
               "OffthreadSnapshot pointers must be tenured");
  }
  OffthreadGCPtr(const OffthreadGCPtr<T>& other) = default;

  operator T() const { return ptr_; }
  T operator->() const { return ptr_; }

 private:
  OffthreadGCPtr() = delete;
  void operator=(OffthreadGCPtr<T>& other) = delete;
};

template <typename T>
inline void TraceOffthreadGCPtr(JSTracer* trc, const OffthreadGCPtr<T>& thing,
                                const char* name) {
  T thingRaw = thing;
  js::TraceManuallyBarrieredEdge(trc, &thingRaw, name);
  MOZ_ASSERT(static_cast<T>(thing) == thingRaw, "Unexpected moving GC!");
}

#endif /* jit_OffthreadSnapshot_h */
