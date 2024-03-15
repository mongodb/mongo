/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef js_Tracer_h
#define js_Tracer_h

#include "gc/Barrier.h"
#include "gc/TraceKind.h"
#include "js/HashTable.h"
#include "js/TracingAPI.h"

namespace JS {
using CompartmentSet =
    js::HashSet<Compartment*, js::DefaultHasher<Compartment*>,
                js::SystemAllocPolicy>;
}  // namespace JS

namespace js {

class TaggedProto;

// Internal Tracing API
//
// Tracing is an abstract visitation of each edge in a JS heap graph.[1] The
// most common (and performance sensitive) use of this infrastructure is for GC
// "marking" as part of the mark-and-sweep collector; however, this
// infrastructure is much more general than that and is used for many other
// purposes as well.
//
// One commonly misunderstood subtlety of the tracing architecture is the role
// of graph vertices versus graph edges. Graph vertices are the heap
// allocations -- GC things -- that are returned by Allocate. Graph edges are
// pointers -- including tagged pointers like Value and jsid -- that link the
// allocations into a complex heap. The tracing API deals *only* with edges.
// Any action taken on the target of a graph edge is independent of the tracing
// itself.
//
// Another common misunderstanding relates to the role of the JSTracer. The
// JSTracer instance determines what tracing does when visiting an edge; it
// does not itself participate in the tracing process, other than to be passed
// through as opaque data. It works like a closure in that respect.
//
// Tracing implementations internal to SpiderMonkey should use these interfaces
// instead of the public interfaces in js/TracingAPI.h. Unlike the public
// tracing methods, these work on internal types and avoid an external call.
//
// Note that the implementations for these methods are, surprisingly, in
// js/src/gc/Marking.cpp. This is so that the compiler can inline as much as
// possible in the common, marking pathways. Conceptually, however, they remain
// as part of the generic "tracing" architecture, rather than the more specific
// marking implementation of tracing.
//
// 1 - In SpiderMonkey, we call this concept tracing rather than visiting
//     because "visiting" is already used by the compiler. Also, it's been
//     called "tracing" forever and changing it would be extremely difficult at
//     this point.

class GCMarker;

// Debugging functions to check tracing invariants.
#ifdef DEBUG
template <typename T>
void CheckTracedThing(JSTracer* trc, T* thing);
template <typename T>
void CheckTracedThing(JSTracer* trc, const T& thing);
#else
template <typename T>
inline void CheckTracedThing(JSTracer* trc, T* thing) {}
template <typename T>
inline void CheckTracedThing(JSTracer* trc, const T& thing) {}
#endif

namespace gc {

// Our barrier templates are parameterized on the pointer types so that we can
// share the definitions with Value and jsid. Thus, we need to strip the
// pointer before sending the type to BaseGCType and re-add it on the other
// side. As such:
template <typename T>
struct PtrBaseGCType {
  using type = T;
};
template <typename T>
struct PtrBaseGCType<T*> {
  using type = typename BaseGCType<T>::type*;
};

// Cast a possibly-derived T** pointer to a base class pointer.
template <typename T>
typename PtrBaseGCType<T>::type* ConvertToBase(T* thingp) {
  return reinterpret_cast<typename PtrBaseGCType<T>::type*>(thingp);
}

// Internal methods to trace edges.

#define DEFINE_TRACE_FUNCTION(name, type, _1, _2)                        \
  MOZ_ALWAYS_INLINE bool TraceEdgeInternal(JSTracer* trc, type** thingp, \
                                           const char* name) {           \
    CheckTracedThing(trc, *thingp);                                      \
    trc->on##name##Edge(thingp, name);                                   \
    return *thingp;                                                      \
  }
JS_FOR_EACH_TRACEKIND(DEFINE_TRACE_FUNCTION)
#undef DEFINE_TRACE_FUNCTION

bool TraceEdgeInternal(JSTracer* trc, Value* thingp, const char* name);
bool TraceEdgeInternal(JSTracer* trc, jsid* thingp, const char* name);
bool TraceEdgeInternal(JSTracer* trc, TaggedProto* thingp, const char* name);

template <typename T>
void TraceRangeInternal(JSTracer* trc, size_t len, T* vec, const char* name);
template <typename T>
bool TraceWeakMapKeyInternal(JSTracer* trc, Zone* zone, T* thingp,
                             const char* name);

#ifdef DEBUG
void AssertRootMarkingPhase(JSTracer* trc);
void AssertShouldMarkInZone(GCMarker* marker, gc::Cell* thing);
#else
inline void AssertRootMarkingPhase(JSTracer* trc) {}
inline void AssertShouldMarkInZone(GCMarker* marker, gc::Cell* thing) {}
#endif

}  // namespace gc

// Trace through a strong edge in the live object graph on behalf of
// tracing. The effect of tracing the edge depends on the JSTracer being
// used. For pointer types, |*thingp| must not be null.
//
// Note that weak edges are handled separately. GC things with weak edges must
// not trace those edges during marking tracing (which would keep the referent
// alive) but instead arrange for the edge to be swept by calling
// js::gc::IsAboutToBeFinalized or TraceWeakEdge during sweeping.
//
// GC things that are weakly held in containers can use WeakMap or a container
// wrapped in the WeakCache<> template to perform the appropriate sweeping.

template <typename T>
inline void TraceEdge(JSTracer* trc, const WriteBarriered<T>* thingp,
                      const char* name) {
  gc::TraceEdgeInternal(trc, gc::ConvertToBase(thingp->unbarrieredAddress()),
                        name);
}

template <typename T>
inline void TraceEdge(JSTracer* trc, WeakHeapPtr<T>* thingp, const char* name) {
  gc::TraceEdgeInternal(trc, gc::ConvertToBase(thingp->unbarrieredAddress()),
                        name);
}

template <class BC, class T>
inline void TraceCellHeaderEdge(JSTracer* trc,
                                gc::CellWithTenuredGCPointer<BC, T>* thingp,
                                const char* name) {
  T* thing = thingp->headerPtr();
  gc::TraceEdgeInternal(trc, gc::ConvertToBase(&thing), name);
  if (thing != thingp->headerPtr()) {
    thingp->unbarrieredSetHeaderPtr(thing);
  }
}

template <class T>
inline void TraceCellHeaderEdge(JSTracer* trc,
                                gc::TenuredCellWithGCPointer<T>* thingp,
                                const char* name) {
  T* thing = thingp->headerPtr();
  gc::TraceEdgeInternal(trc, gc::ConvertToBase(&thing), name);
  if (thing != thingp->headerPtr()) {
    thingp->unbarrieredSetHeaderPtr(thing);
  }
}

// Trace through a possibly-null edge in the live object graph on behalf of
// tracing.

template <typename T>
inline void TraceNullableEdge(JSTracer* trc, const WriteBarriered<T>* thingp,
                              const char* name) {
  if (InternalBarrierMethods<T>::isMarkable(thingp->get())) {
    TraceEdge(trc, thingp, name);
  }
}

template <typename T>
inline void TraceNullableEdge(JSTracer* trc, WeakHeapPtr<T>* thingp,
                              const char* name) {
  if (InternalBarrierMethods<T>::isMarkable(thingp->unbarrieredGet())) {
    TraceEdge(trc, thingp, name);
  }
}

template <class BC, class T>
inline void TraceNullableCellHeaderEdge(
    JSTracer* trc, gc::CellWithTenuredGCPointer<BC, T>* thingp,
    const char* name) {
  T* thing = thingp->headerPtr();
  if (thing) {
    gc::TraceEdgeInternal(trc, gc::ConvertToBase(&thing), name);
    if (thing != thingp->headerPtr()) {
      thingp->unbarrieredSetHeaderPtr(thing);
    }
  }
}

// Trace through a "root" edge. These edges are the initial edges in the object
// graph traversal. Root edges are asserted to only be traversed in the initial
// phase of a GC.

template <typename T>
inline void TraceRoot(JSTracer* trc, T* thingp, const char* name) {
  gc::AssertRootMarkingPhase(trc);
  gc::TraceEdgeInternal(trc, gc::ConvertToBase(thingp), name);
}

template <typename T>
inline void TraceRoot(JSTracer* trc, const HeapPtr<T>* thingp,
                      const char* name) {
  TraceRoot(trc, thingp->unbarrieredAddress(), name);
}

// Idential to TraceRoot, except that this variant will not crash if |*thingp|
// is null.

template <typename T>
inline void TraceNullableRoot(JSTracer* trc, T* thingp, const char* name) {
  gc::AssertRootMarkingPhase(trc);
  if (InternalBarrierMethods<T>::isMarkable(*thingp)) {
    gc::TraceEdgeInternal(trc, gc::ConvertToBase(thingp), name);
  }
}

template <typename T>
inline void TraceNullableRoot(JSTracer* trc, WeakHeapPtr<T>* thingp,
                              const char* name) {
  TraceNullableRoot(trc, thingp->unbarrieredAddress(), name);
}

// Like TraceEdge, but for edges that do not use one of the automatic barrier
// classes and, thus, must be treated specially for moving GC. This method is
// separate from TraceEdge to make accidental use of such edges more obvious.

template <typename T>
inline void TraceManuallyBarrieredEdge(JSTracer* trc, T* thingp,
                                       const char* name) {
  gc::TraceEdgeInternal(trc, gc::ConvertToBase(thingp), name);
}

// Trace through a weak edge. If *thingp is not marked at the end of marking,
// it is replaced by nullptr, and this method will return false to indicate that
// the edge no longer exists.
template <typename T>
inline bool TraceManuallyBarrieredWeakEdge(JSTracer* trc, T* thingp,
                                           const char* name) {
  return gc::TraceEdgeInternal(trc, gc::ConvertToBase(thingp), name);
}

// The result of tracing a weak edge, which can be either:
//
//  - the target is dead (and the edge has been cleared), or
//  - the target is alive (and the edge may have been updated)
//
// This includes the initial and final values of the edge to allow cleanup if
// the target is dead or access to the referent if it is alive.
template <typename T>
struct TraceWeakResult {
  const bool live_;
  const T initial_;
  const T final_;

  bool isLive() const { return live_; }
  bool isDead() const { return !live_; }

  MOZ_IMPLICIT operator bool() const { return isLive(); }

  T initialTarget() const {
    MOZ_ASSERT(isDead());
    return initial_;
  }

  T finalTarget() const {
    MOZ_ASSERT(isLive());
    return final_;
  }
};

template <typename T>
inline TraceWeakResult<T> TraceWeakEdge(JSTracer* trc, BarrieredBase<T>* thingp,
                                        const char* name) {
  T* addr = thingp->unbarrieredAddress();
  T initial = *addr;
  bool live = !InternalBarrierMethods<T>::isMarkable(initial) ||
              gc::TraceEdgeInternal(trc, gc::ConvertToBase(addr), name);
  return TraceWeakResult<T>{live, initial, *addr};
}

// Trace all edges contained in the given array.

template <typename T>
void TraceRange(JSTracer* trc, size_t len, BarrieredBase<T>* vec,
                const char* name) {
  gc::TraceRangeInternal(trc, len,
                         gc::ConvertToBase(vec[0].unbarrieredAddress()), name);
}

// Trace all root edges in the given array.

template <typename T>
void TraceRootRange(JSTracer* trc, size_t len, T* vec, const char* name) {
  gc::AssertRootMarkingPhase(trc);
  gc::TraceRangeInternal(trc, len, gc::ConvertToBase(vec), name);
}

// As below but with manual barriers.
template <typename T>
void TraceManuallyBarrieredCrossCompartmentEdge(JSTracer* trc, JSObject* src,
                                                T* dst, const char* name);

// Trace an edge that crosses compartment boundaries. If the compartment of the
// destination thing is not being GC'd, then the edge will not be traced.
template <typename T>
void TraceCrossCompartmentEdge(JSTracer* trc, JSObject* src,
                               const WriteBarriered<T>* dst, const char* name) {
  TraceManuallyBarrieredCrossCompartmentEdge(
      trc, src, gc::ConvertToBase(dst->unbarrieredAddress()), name);
}

// Trace an edge that's guaranteed to be same-zone but may cross a compartment
// boundary. This should NOT be used for object => object edges, as those have
// to be in the cross-compartment wrapper map.
//
// WARNING: because this turns off certain compartment checks, you most likely
// don't want to use this! If you still think you need this function, talk to a
// GC peer first.
template <typename T>
void TraceSameZoneCrossCompartmentEdge(JSTracer* trc,
                                       const WriteBarriered<T>* dst,
                                       const char* name);

// Trace a weak map key. For debugger weak maps these may be cross compartment,
// but the compartment must always be within the current sweep group.
template <typename T>
void TraceWeakMapKeyEdgeInternal(JSTracer* trc, Zone* weakMapZone, T** thingp,
                                 const char* name);

template <typename T>
inline void TraceWeakMapKeyEdge(JSTracer* trc, Zone* weakMapZone,
                                const WriteBarriered<T>* thingp,
                                const char* name) {
  TraceWeakMapKeyEdgeInternal(
      trc, weakMapZone, gc::ConvertToBase(thingp->unbarrieredAddress()), name);
}

// Trace a root edge that uses the base GC thing type, instead of a more
// specific type.
void TraceGenericPointerRoot(JSTracer* trc, gc::Cell** thingp,
                             const char* name);

// Trace a non-root edge that uses the base GC thing type, instead of a more
// specific type.
void TraceManuallyBarrieredGenericPointerEdge(JSTracer* trc, gc::Cell** thingp,
                                              const char* name);

void TraceGCCellPtrRoot(JSTracer* trc, JS::GCCellPtr* thingp, const char* name);

void TraceManuallyBarrieredGCCellPtr(JSTracer* trc, JS::GCCellPtr* thingp,
                                     const char* name);

namespace gc {

// Trace through a shape or group iteratively during cycle collection to avoid
// deep or infinite recursion.
void TraceCycleCollectorChildren(JS::CallbackTracer* trc, Shape* shape);

/**
 * Trace every value within |compartments| that is wrapped by a
 * cross-compartment wrapper from a compartment that is not an element of
 * |compartments|.
 */
void TraceIncomingCCWs(JSTracer* trc, const JS::CompartmentSet& compartments);

/* Get information about a GC thing. Used when dumping the heap. */
void GetTraceThingInfo(char* buf, size_t bufsize, void* thing,
                       JS::TraceKind kind, bool includeDetails);

// Overloaded function to call the correct GenericTracer method based on the
// argument type.
#define DEFINE_DISPATCH_FUNCTION(name, type, _1, _2)         \
  inline void DispatchToOnEdge(JSTracer* trc, type** thingp, \
                               const char* name) {           \
    trc->on##name##Edge(thingp, name);                       \
  }
JS_FOR_EACH_TRACEKIND(DEFINE_DISPATCH_FUNCTION)
#undef DEFINE_DISPATCH_FUNCTION

}  // namespace gc
}  // namespace js

#endif /* js_Tracer_h */
