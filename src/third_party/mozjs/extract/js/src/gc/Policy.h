/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* JS Garbage Collector. */

#ifndef gc_Policy_h
#define gc_Policy_h

#include <type_traits>

#include "gc/Barrier.h"
#include "gc/Tracer.h"
#include "js/GCPolicyAPI.h"

namespace js {

// Define the GCPolicy for all internal pointers.
template <typename T>
struct InternalGCPointerPolicy : public JS::GCPointerPolicy<T> {
  using Type = std::remove_pointer_t<T>;

#define IS_BASE_OF_OR(_1, BaseType, _2, _3) std::is_base_of_v<BaseType, Type> ||
  static_assert(
      JS_FOR_EACH_TRACEKIND(IS_BASE_OF_OR) false,
      "InternalGCPointerPolicy must only be used for GC thing pointers");
#undef IS_BASE_OF_OR

  static void trace(JSTracer* trc, T* vp, const char* name) {
    // It's not safe to trace unbarriered pointers except as part of root
    // marking. If you get an assertion here you probably need to add a barrier,
    // e.g. HeapPtr<T>.
    TraceNullableRoot(trc, vp, name);
  }
};

}  // namespace js

namespace JS {

// Internally, all pointer types are treated as pointers to GC things by
// default.
template <typename T>
struct GCPolicy<T*> : public js::InternalGCPointerPolicy<T*> {};
template <typename T>
struct GCPolicy<T* const> : public js::InternalGCPointerPolicy<T* const> {};

template <typename T>
struct GCPolicy<js::HeapPtr<T>> {
  static void trace(JSTracer* trc, js::HeapPtr<T>* thingp, const char* name) {
    js::TraceNullableEdge(trc, thingp, name);
  }
  static bool traceWeak(JSTracer* trc, js::HeapPtr<T>* thingp) {
    return js::TraceWeakEdge(trc, thingp, "HeapPtr");
  }
};

template <typename T>
struct GCPolicy<js::PreBarriered<T>> {
  static void trace(JSTracer* trc, js::PreBarriered<T>* thingp,
                    const char* name) {
    js::TraceNullableEdge(trc, thingp, name);
  }
};

template <typename T>
struct GCPolicy<js::WeakHeapPtr<T>> {
  static void trace(JSTracer* trc, js::WeakHeapPtr<T>* thingp,
                    const char* name) {
    js::TraceEdge(trc, thingp, name);
  }
  static bool traceWeak(JSTracer* trc, js::WeakHeapPtr<T>* thingp) {
    return js::TraceWeakEdge(trc, thingp, "traceWeak");
  }
};

template <typename T>
struct GCPolicy<js::UnsafeBarePtr<T>> {
  static bool traceWeak(JSTracer* trc, js::UnsafeBarePtr<T>* vp) {
    if (*vp) {
      return js::TraceManuallyBarrieredWeakEdge(trc, vp->unbarrieredAddress(),
                                                "UnsafeBarePtr");
    }
    return true;
  }
};

template <>
struct GCPolicy<JS::GCCellPtr> {
  static void trace(JSTracer* trc, JS::GCCellPtr* thingp, const char* name) {
    // It's not safe to trace unbarriered pointers except as part of root
    // marking.
    js::TraceGCCellPtrRoot(trc, thingp, name);
  }
};

}  // namespace JS

#endif  // gc_Policy_h
