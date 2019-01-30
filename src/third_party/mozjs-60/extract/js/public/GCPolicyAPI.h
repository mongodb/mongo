/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
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
//   static T initial()
//       - Construct and return an empty T.
//
//   static void trace(JSTracer, T* tp, const char* name)
//       - Trace the edge |*tp|, calling the edge |name|. Containers like
//         GCHashMap and GCHashSet use this method to trace their children.
//
//   static bool needsSweep(T* tp)
//       - Return true if |*tp| is about to be finalized. Otherwise, update the
//         edge for moving GC, and return false. Containers like GCHashMap and
//         GCHashSet use this method to decide when to remove an entry: if this
//         function returns true on a key/value/member/etc, its entry is dropped
//         from the container. Specializing this method is the standard way to
//         get custom weak behavior from a container type.
//
// The default GCPolicy<T> assumes that T has a default constructor and |trace|
// and |needsSweep| methods, and forwards to them. GCPolicy has appropriate
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

#include "js/TraceKind.h"
#include "js/TracingAPI.h"
#include "js/TypeDecls.h"

// Expand the given macro D for each public GC pointer.
#define FOR_EACH_PUBLIC_GC_POINTER_TYPE(D) \
    D(JS::Symbol*) \
    D(JSAtom*) \
    D(JSFunction*) \
    D(JSObject*) \
    D(JSScript*) \
    D(JSString*)

// Expand the given macro D for each public tagged GC pointer type.
#define FOR_EACH_PUBLIC_TAGGED_GC_POINTER_TYPE(D) \
    D(JS::Value) \
    D(jsid)

#define FOR_EACH_PUBLIC_AGGREGATE_GC_POINTER_TYPE(D) \
    D(JSPropertyDescriptor)

namespace JS {

// Defines a policy for container types with non-GC, i.e. C storage. This
// policy dispatches to the underlying struct for GC interactions.
template <typename T>
struct StructGCPolicy
{
    static T initial() {
        return T();
    }

    static void trace(JSTracer* trc, T* tp, const char* name) {
        tp->trace(trc);
    }

    static void sweep(T* tp) {
        return tp->sweep();
    }

    static bool needsSweep(T* tp) {
        return tp->needsSweep();
    }

    static bool isValid(const T& tp) {
        return true;
    }
};

// The default GC policy attempts to defer to methods on the underlying type.
// Most C++ structures that contain a default constructor, a trace function and
// a sweep function will work out of the box with Rooted, Handle, GCVector,
// and GCHash{Set,Map}.
template <typename T> struct GCPolicy : public StructGCPolicy<T> {};

// This policy ignores any GC interaction, e.g. for non-GC types.
template <typename T>
struct IgnoreGCPolicy {
    static T initial() { return T(); }
    static void trace(JSTracer* trc, T* t, const char* name) {}
    static bool needsSweep(T* v) { return false; }
    static bool isValid(const T& v) { return true; }
};
template <> struct GCPolicy<uint32_t> : public IgnoreGCPolicy<uint32_t> {};
template <> struct GCPolicy<uint64_t> : public IgnoreGCPolicy<uint64_t> {};

template <typename T>
struct GCPointerPolicy
{
    static T initial() { return nullptr; }
    static void trace(JSTracer* trc, T* vp, const char* name) {
        if (*vp)
            js::UnsafeTraceManuallyBarrieredEdge(trc, vp, name);
    }
    static bool needsSweep(T* vp) {
        if (*vp)
            return js::gc::IsAboutToBeFinalizedUnbarriered(vp);
        return false;
    }
    static bool isValid(T v) {
        return js::gc::IsCellPointerValidOrNull(v);
    }
};
template <> struct GCPolicy<JS::Symbol*> : public GCPointerPolicy<JS::Symbol*> {};
template <> struct GCPolicy<JSAtom*> : public GCPointerPolicy<JSAtom*> {};
template <> struct GCPolicy<JSFunction*> : public GCPointerPolicy<JSFunction*> {};
template <> struct GCPolicy<JSObject*> : public GCPointerPolicy<JSObject*> {};
template <> struct GCPolicy<JSScript*> : public GCPointerPolicy<JSScript*> {};
template <> struct GCPolicy<JSString*> : public GCPointerPolicy<JSString*> {};

template <typename T>
struct NonGCPointerPolicy
{
    static T initial() { return nullptr; }
    static void trace(JSTracer* trc, T* vp, const char* name) {
        if (*vp)
            (*vp)->trace(trc);
    }
    static bool needsSweep(T* vp) {
        if (*vp)
            return (*vp)->needsSweep();
        return false;
    }
    static bool isValid(T v) {
        return true;
    }
};

template <typename T>
struct GCPolicy<JS::Heap<T>>
{
    static void trace(JSTracer* trc, JS::Heap<T>* thingp, const char* name) {
        TraceEdge(trc, thingp, name);
    }
    static bool needsSweep(JS::Heap<T>* thingp) {
        return *thingp && js::gc::EdgeNeedsSweep(thingp);
    }
};

// GCPolicy<UniquePtr<T>> forwards the contained pointer to GCPolicy<T>.
template <typename T, typename D>
struct GCPolicy<mozilla::UniquePtr<T, D>>
{
    static mozilla::UniquePtr<T,D> initial() { return mozilla::UniquePtr<T,D>(); }
    static void trace(JSTracer* trc, mozilla::UniquePtr<T,D>* tp, const char* name) {
        if (tp->get())
            GCPolicy<T>::trace(trc, tp->get(), name);
    }
    static bool needsSweep(mozilla::UniquePtr<T,D>* tp) {
        if (tp->get())
            return GCPolicy<T>::needsSweep(tp->get());
        return false;
    }
    static bool isValid(const mozilla::UniquePtr<T,D>& t) {
        if (t.get())
            return GCPolicy<T>::isValid(*t.get());
        return true;
    }
};

// GCPolicy<Maybe<T>> forwards tracing/sweeping to GCPolicy<T*> if
// when the Maybe<T> is full.
template <typename T>
struct GCPolicy<mozilla::Maybe<T>>
{
    static mozilla::Maybe<T> initial() { return mozilla::Maybe<T>(); }
    static void trace(JSTracer* trc, mozilla::Maybe<T>* tp, const char* name) {
        if (tp->isSome())
            GCPolicy<T>::trace(trc, tp->ptr(), name);
    }
    static bool needsSweep(mozilla::Maybe<T>* tp) {
        if (tp->isSome())
            return GCPolicy<T>::needsSweep(tp->ptr());
        return false;
    }
    static bool isValid(const mozilla::Maybe<T>& t) {
        if (t.isSome())
            return GCPolicy<T>::isValid(t.ref());
        return true;
    }
};

template <> struct GCPolicy<JS::Realm*>;  // see Realm.h

} // namespace JS

#endif // GCPolicyAPI_h
