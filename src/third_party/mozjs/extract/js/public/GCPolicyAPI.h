/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// GC Policy Mechanism

// A GCPolicy controls how the GC interacts with both direct pointers to GC
// things (e.g. JSObject* or JSString*), tagged and/or optional pointers to GC
// things (e.g.  Value or jsid), and C++ container types (e.g.
// JSPropertyDescriptor or GCHashMap).
//
// The GCPolicy provides at a minimum:
//
//   static void trace(JSTracer, T* tp, const char* name)
//       - Trace the edge |*tp|, calling the edge |name|. Containers like
//         GCHashMap and GCHashSet use this method to trace their children.
//
//   static bool traceWeak(T* tp)
//       - Return false if |*tp| has been set to nullptr. Otherwise, update the
//         edge for moving GC, and return true. Containers like GCHashMap and
//         GCHashSet use this method to decide when to remove an entry: if this
//         function returns false on a key/value/member/etc, its entry is
//         dropped from the container. Specializing this method is the standard
//         way to get custom weak behavior from a container type.
//
//   static bool isValid(const T& t)
//       - Return false only if |t| is corrupt in some way. The built-in GC
//         types do some memory layout checks. For debugging only; it is ok
//         to always return true or even to omit this member entirely.
//
// The default GCPolicy<T> assumes that T has a default constructor and |trace|
// and |traceWeak| methods, and forwards to them. GCPolicy has appropriate
// specializations for pointers to GC things and pointer-like types like
// JS::Heap<T> and mozilla::UniquePtr<T>.
//
// There are some stock structs your specializations can inherit from.
// IgnoreGCPolicy<T> does nothing. StructGCPolicy<T> forwards the methods to the
// referent type T.

#ifndef GCPolicyAPI_h
#define GCPolicyAPI_h

#include "mozilla/Maybe.h"
#include "mozilla/UniquePtr.h"

#include <type_traits>

#include "js/GCTypeMacros.h"  // JS_FOR_EACH_PUBLIC_GC_POINTER_TYPE
#include "js/TraceKind.h"
#include "js/TracingAPI.h"
#include "js/TypeDecls.h"

namespace JS {

// Defines a policy for container types with non-GC, i.e. C storage. This
// policy dispatches to the underlying struct for GC interactions. Note that
// currently a type can define only the subset of the methods (trace and/or
// traceWeak) if it is never used in a context that requires the other.
template <typename T>
struct StructGCPolicy {
  static_assert(!std::is_pointer_v<T>,
                "Pointer type not allowed for StructGCPolicy");

  static void trace(JSTracer* trc, T* tp, const char* name) { tp->trace(trc); }

  static bool traceWeak(JSTracer* trc, T* tp) { return tp->traceWeak(trc); }

  static bool isValid(const T& tp) { return true; }
};

// The default GC policy attempts to defer to methods on the underlying type.
// Most C++ structures that contain a default constructor, a trace function and
// a sweep function will work out of the box with Rooted, Handle, GCVector,
// and GCHash{Set,Map}.
template <typename T>
struct GCPolicy : public StructGCPolicy<T> {};

// This policy ignores any GC interaction, e.g. for non-GC types.
template <typename T>
struct IgnoreGCPolicy {
  static void trace(JSTracer* trc, T* t, const char* name) {}
  static bool traceWeak(JSTracer*, T* v) { return true; }
  static bool isValid(const T& v) { return true; }
};
template <>
struct GCPolicy<uint32_t> : public IgnoreGCPolicy<uint32_t> {};
template <>
struct GCPolicy<uint64_t> : public IgnoreGCPolicy<uint64_t> {};
template <>
struct GCPolicy<bool> : public IgnoreGCPolicy<bool> {};

template <typename T>
struct GCPointerPolicy {
  static_assert(std::is_pointer_v<T>,
                "Non-pointer type not allowed for GCPointerPolicy");

  static void trace(JSTracer* trc, T* vp, const char* name) {
    // This should only be called as part of root marking since that's the only
    // time we should trace unbarriered GC thing pointers. This will assert if
    // called at other times.
    TraceRoot(trc, vp, name);
  }
  static bool isTenured(T v) { return !v || !js::gc::IsInsideNursery(v); }
  static bool isValid(T v) { return js::gc::IsCellPointerValidOrNull(v); }
};
#define EXPAND_SPECIALIZE_GCPOLICY(Type)                   \
  template <>                                              \
  struct GCPolicy<Type> : public GCPointerPolicy<Type> {}; \
  template <>                                              \
  struct GCPolicy<Type const> : public GCPointerPolicy<Type const> {};
JS_FOR_EACH_PUBLIC_GC_POINTER_TYPE(EXPAND_SPECIALIZE_GCPOLICY)
#undef EXPAND_SPECIALIZE_GCPOLICY

template <typename T>
struct NonGCPointerPolicy {
  static void trace(JSTracer* trc, T* vp, const char* name) {
    if (*vp) {
      (*vp)->trace(trc);
    }
  }
  static bool traceWeak(JSTracer* trc, T* vp) {
    if (*vp) {
      return (*vp)->traceWeak(trc);
    }
    return true;
  }

  static bool isValid(T v) { return true; }
};

template <typename T>
struct GCPolicy<JS::Heap<T>> {
  static void trace(JSTracer* trc, JS::Heap<T>* thingp, const char* name) {
    TraceEdge(trc, thingp, name);
  }
  static bool traceWeak(JSTracer* trc, JS::Heap<T>* thingp) {
    return !*thingp || js::gc::TraceWeakEdge(trc, thingp);
  }
};

// GCPolicy<UniquePtr<T>> forwards the contained pointer to GCPolicy<T>.
template <typename T, typename D>
struct GCPolicy<mozilla::UniquePtr<T, D>> {
  static void trace(JSTracer* trc, mozilla::UniquePtr<T, D>* tp,
                    const char* name) {
    if (tp->get()) {
      GCPolicy<T>::trace(trc, tp->get(), name);
    }
  }
  static bool traceWeak(JSTracer* trc, mozilla::UniquePtr<T, D>* tp) {
    if (tp->get()) {
      return GCPolicy<T>::traceWeak(trc, tp->get());
    }
    return true;
  }
  static bool isValid(const mozilla::UniquePtr<T, D>& t) {
    if (t.get()) {
      return GCPolicy<T>::isValid(*t.get());
    }
    return true;
  }
};

template <>
struct GCPolicy<mozilla::Nothing> : public IgnoreGCPolicy<mozilla::Nothing> {};

// GCPolicy<Maybe<T>> forwards tracing/sweeping to GCPolicy<T*> if
// the Maybe<T> is filled and T* can be traced via GCPolicy<T*>.
template <typename T>
struct GCPolicy<mozilla::Maybe<T>> {
  static void trace(JSTracer* trc, mozilla::Maybe<T>* tp, const char* name) {
    if (tp->isSome()) {
      GCPolicy<T>::trace(trc, tp->ptr(), name);
    }
  }
  static bool traceWeak(JSTracer* trc, mozilla::Maybe<T>* tp) {
    if (tp->isSome()) {
      return GCPolicy<T>::traceWeak(trc, tp->ptr());
    }
    return true;
  }
  static bool isValid(const mozilla::Maybe<T>& t) {
    if (t.isSome()) {
      return GCPolicy<T>::isValid(t.ref());
    }
    return true;
  }
};

template <typename T1, typename T2>
struct GCPolicy<std::pair<T1, T2>> {
  static void trace(JSTracer* trc, std::pair<T1, T2>* tp, const char* name) {
    GCPolicy<T1>::trace(trc, &tp->first, name);
    GCPolicy<T2>::trace(trc, &tp->second, name);
  }
  static bool traceWeak(JSTracer* trc, std::pair<T1, T2>* tp) {
    return GCPolicy<T1>::traceWeak(trc, &tp->first) &&
           GCPolicy<T2>::traceWeak(trc, &tp->second);
  }
  static bool isValid(const std::pair<T1, T2>& t) {
    return GCPolicy<T1>::isValid(t.first) && GCPolicy<T2>::isValid(t.second);
  }
};

template <>
struct GCPolicy<JS::Realm*>;  // see Realm.h

template <>
struct GCPolicy<mozilla::Ok> : public IgnoreGCPolicy<mozilla::Ok> {};

template <typename V, typename E>
struct GCPolicy<mozilla::Result<V, E>> {
  static void trace(JSTracer* trc, mozilla::Result<V, E>* tp,
                    const char* name) {
    if (tp->isOk()) {
      V tmp = tp->unwrap();
      JS::GCPolicy<V>::trace(trc, &tmp, "Result value");
      tp->updateAfterTracing(std::move(tmp));
    }

    if (tp->isErr()) {
      E tmp = tp->unwrapErr();
      JS::GCPolicy<E>::trace(trc, &tmp, "Result error");
      tp->updateErrorAfterTracing(std::move(tmp));
    }
  }

  static bool isValid(const mozilla::Result<V, E>& t) { return true; }
};

}  // namespace JS

#endif  // GCPolicyAPI_h
