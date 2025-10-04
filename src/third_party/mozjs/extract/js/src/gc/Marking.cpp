/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "gc/Marking-inl.h"

#include "mozilla/DebugOnly.h"
#include "mozilla/IntegerRange.h"
#include "mozilla/MathAlgorithms.h"
#include "mozilla/Maybe.h"
#include "mozilla/PodOperations.h"
#include "mozilla/ScopeExit.h"

#include <algorithm>
#include <type_traits>

#include "gc/GCInternals.h"
#include "gc/ParallelMarking.h"
#include "gc/TraceKind.h"
#include "jit/JitCode.h"
#include "js/GCTypeMacros.h"  // JS_FOR_EACH_PUBLIC_{,TAGGED_}GC_POINTER_TYPE
#include "js/SliceBudget.h"
#include "util/Poison.h"
#include "vm/GeneratorObject.h"

#include "gc/GC-inl.h"
#include "gc/PrivateIterators-inl.h"
#include "gc/TraceMethods-inl.h"
#include "gc/WeakMap-inl.h"
#include "vm/GeckoProfiler-inl.h"

using namespace js;
using namespace js::gc;

using JS::MapTypeToTraceKind;

using mozilla::DebugOnly;
using mozilla::IntegerRange;
using mozilla::PodCopy;

// [SMDOC] GC Tracing
//
// Tracing Overview
// ================
//
// Tracing, in this context, refers to an abstract visitation of some or all of
// the GC-controlled heap. The effect of tracing an edge of the graph depends
// on the subclass of the JSTracer on whose behalf we are tracing.
//
// Marking
// -------
//
// The primary JSTracer is the GCMarker. The marking tracer causes the target
// of each traversed edge to be marked black and the target edge's children to
// be marked either gray (in the gc algorithm sense) or immediately black.
//
// Callback
// --------
//
// The secondary JSTracer is the CallbackTracer. This simply invokes a callback
// on each edge in a child.
//
// The following is a rough outline of the general struture of the tracing
// internals.
//
/* clang-format off */
//
//  +-------------------+                             ......................
//  |                   |                             :                    :
//  |                   v                             v                +---+---+
//  |   TraceRoot   TraceEdge   TraceRange        GCMarker::           |       |
//  |       |           |           |         processMarkStackTop      | Mark  |
//  |       +-----------------------+                 |                | Stack |
//  |                   |                             |                |       |
//  |                   v                             |                +---+---+
//  |           TraceEdgeInternal                     |                    ^
//  |                   |                             +<-------------+     :
//  |                   |                             |              |     :
//  |                   v                             v              |     :
//  |            CallbackTracer::             markAndTraverseEdge    |     :
//  |              onSomeEdge                         |              |     :
//  |                   |                             |              |     :
//  |                   |                             |              |     :
//  |                   +-------------+---------------+              |     :
//  |                                 |                              |     :
//  |                                 v                              |     :
//  |                          markAndTraverse                       |     :
//  |                                 |                              |     :
//  |                                 |                              |     :
//  |                              traverse                          |     :
//  |                                 |                              |     :
//  |             +--------------------------------------+           |     :
//  |             |                   |                  |           |     :
//  |             v                   v                  v           |     :
//  |    markAndTraceChildren    markAndPush    eagerlyMarkChildren  |     :
//  |             |                   :                  |           |     :
//  |             v                   :                  +-----------+     :
//  |      T::traceChildren           :                                    :
//  |             |                   :                                    :
//  +-------------+                   ......................................
//
//   Legend:
//     ------- Direct calls
//     ....... Data flow
//
/* clang-format on */

static const size_t ValueRangeWords =
    sizeof(MarkStack::SlotsOrElementsRange) / sizeof(uintptr_t);

/*** Tracing Invariants *****************************************************/

template <typename T>
static inline bool IsOwnedByOtherRuntime(JSRuntime* rt, T thing) {
  bool other = thing->runtimeFromAnyThread() != rt;
  MOZ_ASSERT_IF(other, thing->isPermanentAndMayBeShared());
  return other;
}

#ifdef DEBUG

static inline bool IsInFreeList(TenuredCell* cell) {
  Arena* arena = cell->arena();
  uintptr_t addr = reinterpret_cast<uintptr_t>(cell);
  MOZ_ASSERT(Arena::isAligned(addr, arena->getThingSize()));
  return arena->inFreeList(addr);
}

template <typename T>
void js::CheckTracedThing(JSTracer* trc, T* thing) {
  MOZ_ASSERT(trc);
  MOZ_ASSERT(thing);

  if (IsForwarded(thing)) {
    JS::TracerKind kind = trc->kind();
    MOZ_ASSERT(kind == JS::TracerKind::Tenuring ||
               kind == JS::TracerKind::MinorSweeping ||
               kind == JS::TracerKind::Moving);
    thing = Forwarded(thing);
  }

  /* This function uses data that's not available in the nursery. */
  if (IsInsideNursery(thing)) {
    return;
  }

  /*
   * Permanent shared things that are not associated with this runtime will be
   * ignored during marking.
   */
  Zone* zone = thing->zoneFromAnyThread();
  if (IsOwnedByOtherRuntime(trc->runtime(), thing)) {
    MOZ_ASSERT(!zone->wasGCStarted());
    MOZ_ASSERT(thing->isMarkedBlack());
    return;
  }

  JSRuntime* rt = trc->runtime();
  MOZ_ASSERT(zone->runtimeFromAnyThread() == rt);

  bool isGcMarkingTracer = trc->isMarkingTracer();
  bool isUnmarkGrayTracer = IsTracerKind(trc, JS::TracerKind::UnmarkGray);
  bool isClearEdgesTracer = IsTracerKind(trc, JS::TracerKind::ClearEdges);

  if (TlsContext.get()) {
    // If we're on the main thread we must have access to the runtime and zone.
    MOZ_ASSERT(CurrentThreadCanAccessRuntime(rt));
    MOZ_ASSERT(CurrentThreadCanAccessZone(zone));
  } else {
    MOZ_ASSERT(isGcMarkingTracer || isUnmarkGrayTracer || isClearEdgesTracer ||
               IsTracerKind(trc, JS::TracerKind::Moving) ||
               IsTracerKind(trc, JS::TracerKind::Sweeping));
    MOZ_ASSERT_IF(!isClearEdgesTracer, CurrentThreadIsPerformingGC());
  }

  MOZ_ASSERT(thing->isAligned());
  MOZ_ASSERT(MapTypeToTraceKind<std::remove_pointer_t<T>>::kind ==
             thing->getTraceKind());

  /*
   * Check that we only mark allocated cells.
   *
   * This check is restricted to marking for two reasons: Firstly, if background
   * sweeping is running and concurrently modifying the free list then it is not
   * safe. Secondly, it was thought to be slow so this is a compromise so as to
   * not affect test times too much.
   */
  MOZ_ASSERT_IF(zone->isGCMarking(), !IsInFreeList(&thing->asTenured()));
}

template <typename T>
void js::CheckTracedThing(JSTracer* trc, const T& thing) {
  ApplyGCThingTyped(thing, [trc](auto t) { CheckTracedThing(trc, t); });
}

template <typename T>
static void CheckMarkedThing(GCMarker* gcMarker, T* thing) {
  Zone* zone = thing->zoneFromAnyThread();

  MOZ_ASSERT(zone->shouldMarkInZone(gcMarker->markColor()));

  MOZ_ASSERT_IF(gcMarker->shouldCheckCompartments(),
                zone->isCollectingFromAnyThread() || zone->isAtomsZone());

  MOZ_ASSERT_IF(gcMarker->markColor() == MarkColor::Gray,
                !zone->isGCMarkingBlackOnly() || zone->isAtomsZone());

  MOZ_ASSERT(!(zone->isGCSweeping() || zone->isGCFinished() ||
               zone->isGCCompacting()));

  // Check that we don't stray from the current compartment and zone without
  // using TraceCrossCompartmentEdge.
  Compartment* comp = thing->maybeCompartment();
  MOZ_ASSERT_IF(gcMarker->tracingCompartment && comp,
                gcMarker->tracingCompartment == comp);
  MOZ_ASSERT_IF(gcMarker->tracingZone,
                gcMarker->tracingZone == zone || zone->isAtomsZone());
}

namespace js {

#  define IMPL_CHECK_TRACED_THING(_, type, _1, _2) \
    template void CheckTracedThing<type>(JSTracer*, type*);
JS_FOR_EACH_TRACEKIND(IMPL_CHECK_TRACED_THING);
#  undef IMPL_CHECK_TRACED_THING

template void CheckTracedThing<Value>(JSTracer*, const Value&);
template void CheckTracedThing<wasm::AnyRef>(JSTracer*, const wasm::AnyRef&);

}  // namespace js

#endif

static inline bool ShouldMarkCrossCompartment(GCMarker* marker, JSObject* src,
                                              Cell* dstCell, const char* name) {
  MarkColor color = marker->markColor();

  if (!dstCell->isTenured()) {
#ifdef DEBUG
    // Bug 1743098: This shouldn't be possible but it does seem to happen. Log
    // some useful information in debug builds.
    if (color != MarkColor::Black) {
      SEprinter printer;
      printer.printf(
          "ShouldMarkCrossCompartment: cross compartment edge '%s' from gray "
          "object to nursery thing\n",
          name);
      printer.put("src: ");
      src->dump(printer);
      printer.put("dst: ");
      dstCell->dump(printer);
    }
#endif
    MOZ_ASSERT(color == MarkColor::Black);
    return false;
  }
  TenuredCell& dst = dstCell->asTenured();

  JS::Zone* dstZone = dst.zone();
  if (!src->zone()->isGCMarking() && !dstZone->isGCMarking()) {
    return false;
  }

  if (color == MarkColor::Black) {
    // Check our sweep groups are correct: we should never have to
    // mark something in a zone that we have started sweeping.
    MOZ_ASSERT_IF(!dst.isMarkedBlack(), !dstZone->isGCSweeping());

    /*
     * Having black->gray edges violates our promise to the cycle collector so
     * we ensure that gray things we encounter when marking black end up getting
     * marked black.
     *
     * This can happen for two reasons:
     *
     * 1) If we're collecting a compartment and it has an edge to an uncollected
     * compartment it's possible that the source and destination of the
     * cross-compartment edge should be gray, but the source was marked black by
     * the write barrier.
     *
     * 2) If we yield during gray marking and the write barrier marks a gray
     * thing black.
     *
     * We handle the first case before returning whereas the second case happens
     * as part of normal marking.
     */
    if (dst.isMarkedGray() && !dstZone->isGCMarking()) {
      UnmarkGrayGCThingUnchecked(marker,
                                 JS::GCCellPtr(&dst, dst.getTraceKind()));
      return false;
    }

    return dstZone->isGCMarking();
  }

  // Check our sweep groups are correct as above.
  MOZ_ASSERT_IF(!dst.isMarkedAny(), !dstZone->isGCSweeping());

  if (dstZone->isGCMarkingBlackOnly()) {
    /*
     * The destination compartment is being not being marked gray now,
     * but it will be later, so record the cell so it can be marked gray
     * at the appropriate time.
     */
    if (!dst.isMarkedAny()) {
      DelayCrossCompartmentGrayMarking(marker, src);
    }
    return false;
  }

  return dstZone->isGCMarkingBlackAndGray();
}

static bool ShouldTraceCrossCompartment(JSTracer* trc, JSObject* src,
                                        Cell* dstCell, const char* name) {
  if (!trc->isMarkingTracer()) {
    return true;
  }

  return ShouldMarkCrossCompartment(GCMarker::fromTracer(trc), src, dstCell,
                                    name);
}

static bool ShouldTraceCrossCompartment(JSTracer* trc, JSObject* src,
                                        const Value& val, const char* name) {
  return val.isGCThing() &&
         ShouldTraceCrossCompartment(trc, src, val.toGCThing(), name);
}

#ifdef DEBUG

template <typename T>
void js::gc::AssertShouldMarkInZone(GCMarker* marker, T* thing) {
  if (thing->isMarkedBlack()) {
    return;
  }

  // Allow marking marking atoms if we're not collected the atoms zone, except
  // for symbols which may entrain other GC things if they're used as weakmap
  // keys.
  bool allowAtoms = !std::is_same_v<T, JS::Symbol>;

  Zone* zone = thing->zone();
  MOZ_ASSERT(zone->shouldMarkInZone(marker->markColor()) ||
             (allowAtoms && zone->isAtomsZone()));
}

void js::gc::AssertRootMarkingPhase(JSTracer* trc) {
  MOZ_ASSERT_IF(trc->isMarkingTracer(),
                trc->runtime()->gc.state() == State::NotActive ||
                    trc->runtime()->gc.state() == State::MarkRoots);
}

#endif  // DEBUG

/*** Tracing Interface ******************************************************/

template <typename T>
static void TraceExternalEdgeHelper(JSTracer* trc, T* thingp,
                                    const char* name) {
  MOZ_ASSERT(InternalBarrierMethods<T>::isMarkable(*thingp));
  TraceEdgeInternal(trc, ConvertToBase(thingp), name);
}

JS_PUBLIC_API void js::UnsafeTraceManuallyBarrieredEdge(JSTracer* trc,
                                                        JSObject** thingp,
                                                        const char* name) {
  TraceEdgeInternal(trc, ConvertToBase(thingp), name);
}

template <typename T>
static void TraceRootHelper(JSTracer* trc, T* thingp, const char* name) {
  MOZ_ASSERT(thingp);
  js::TraceNullableRoot(trc, thingp, name);
}

namespace js {
class AbstractGeneratorObject;
class SavedFrame;
}  // namespace js

#define DEFINE_TRACE_EXTERNAL_EDGE_FUNCTION(type)                           \
  JS_PUBLIC_API void js::gc::TraceExternalEdge(JSTracer* trc, type* thingp, \
                                               const char* name) {          \
    TraceExternalEdgeHelper(trc, thingp, name);                             \
  }

// Define TraceExternalEdge for each public GC pointer type.
JS_FOR_EACH_PUBLIC_GC_POINTER_TYPE(DEFINE_TRACE_EXTERNAL_EDGE_FUNCTION)
JS_FOR_EACH_PUBLIC_TAGGED_GC_POINTER_TYPE(DEFINE_TRACE_EXTERNAL_EDGE_FUNCTION)

#undef DEFINE_TRACE_EXTERNAL_EDGE_FUNCTION

#define DEFINE_UNSAFE_TRACE_ROOT_FUNCTION(type)                 \
  JS_PUBLIC_API void JS::TraceRoot(JSTracer* trc, type* thingp, \
                                   const char* name) {          \
    TraceRootHelper(trc, thingp, name);                         \
  }

// Define TraceRoot for each public GC pointer type.
JS_FOR_EACH_PUBLIC_GC_POINTER_TYPE(DEFINE_UNSAFE_TRACE_ROOT_FUNCTION)
JS_FOR_EACH_PUBLIC_TAGGED_GC_POINTER_TYPE(DEFINE_UNSAFE_TRACE_ROOT_FUNCTION)

// Also, for the moment, define TraceRoot for internal GC pointer types.
DEFINE_UNSAFE_TRACE_ROOT_FUNCTION(AbstractGeneratorObject*)
DEFINE_UNSAFE_TRACE_ROOT_FUNCTION(SavedFrame*)
DEFINE_UNSAFE_TRACE_ROOT_FUNCTION(wasm::AnyRef)

#undef DEFINE_UNSAFE_TRACE_ROOT_FUNCTION

namespace js::gc {

#define INSTANTIATE_INTERNAL_TRACE_FUNCTIONS(type)                     \
  template void TraceRangeInternal<type>(JSTracer*, size_t len, type*, \
                                         const char*);

#define INSTANTIATE_INTERNAL_TRACE_FUNCTIONS_FROM_TRACEKIND(_1, type, _2, _3) \
  INSTANTIATE_INTERNAL_TRACE_FUNCTIONS(type*)

JS_FOR_EACH_TRACEKIND(INSTANTIATE_INTERNAL_TRACE_FUNCTIONS_FROM_TRACEKIND)
JS_FOR_EACH_PUBLIC_TAGGED_GC_POINTER_TYPE(INSTANTIATE_INTERNAL_TRACE_FUNCTIONS)
INSTANTIATE_INTERNAL_TRACE_FUNCTIONS(TaggedProto)

#undef INSTANTIATE_INTERNAL_TRACE_FUNCTIONS_FROM_TRACEKIND
#undef INSTANTIATE_INTERNAL_TRACE_FUNCTIONS

}  // namespace js::gc

// In debug builds, makes a note of the current compartment before calling a
// trace hook or traceChildren() method on a GC thing.
class MOZ_RAII AutoSetTracingSource {
#ifndef DEBUG
 public:
  template <typename T>
  AutoSetTracingSource(JSTracer* trc, T* thing) {}
  ~AutoSetTracingSource() {}
#else
  GCMarker* marker = nullptr;

 public:
  template <typename T>
  AutoSetTracingSource(JSTracer* trc, T* thing) {
    if (trc->isMarkingTracer() && thing) {
      marker = GCMarker::fromTracer(trc);
      MOZ_ASSERT(!marker->tracingZone);
      marker->tracingZone = thing->asTenured().zone();
      MOZ_ASSERT(!marker->tracingCompartment);
      marker->tracingCompartment = thing->maybeCompartment();
    }
  }

  ~AutoSetTracingSource() {
    if (marker) {
      marker->tracingZone = nullptr;
      marker->tracingCompartment = nullptr;
    }
  }
#endif
};

// In debug builds, clear the trace hook compartment. This happens after the
// trace hook has called back into one of our trace APIs and we've checked the
// traced thing.
class MOZ_RAII AutoClearTracingSource {
#ifndef DEBUG
 public:
  explicit AutoClearTracingSource(GCMarker* marker) {}
  explicit AutoClearTracingSource(JSTracer* trc) {}
  ~AutoClearTracingSource() {}
#else
  GCMarker* marker = nullptr;
  JS::Zone* prevZone = nullptr;
  Compartment* prevCompartment = nullptr;

 public:
  explicit AutoClearTracingSource(JSTracer* trc) {
    if (trc->isMarkingTracer()) {
      marker = GCMarker::fromTracer(trc);
      prevZone = marker->tracingZone;
      marker->tracingZone = nullptr;
      prevCompartment = marker->tracingCompartment;
      marker->tracingCompartment = nullptr;
    }
  }
  ~AutoClearTracingSource() {
    if (marker) {
      marker->tracingZone = prevZone;
      marker->tracingCompartment = prevCompartment;
    }
  }
#endif
};

template <typename T>
void js::TraceManuallyBarrieredCrossCompartmentEdge(JSTracer* trc,
                                                    JSObject* src, T* dst,
                                                    const char* name) {
  // Clear expected compartment for cross-compartment edge.
  AutoClearTracingSource acts(trc);

  if (ShouldTraceCrossCompartment(trc, src, *dst, name)) {
    TraceEdgeInternal(trc, dst, name);
  }
}
template void js::TraceManuallyBarrieredCrossCompartmentEdge<Value>(
    JSTracer*, JSObject*, Value*, const char*);
template void js::TraceManuallyBarrieredCrossCompartmentEdge<JSObject*>(
    JSTracer*, JSObject*, JSObject**, const char*);
template void js::TraceManuallyBarrieredCrossCompartmentEdge<BaseScript*>(
    JSTracer*, JSObject*, BaseScript**, const char*);

template <typename T>
void js::TraceSameZoneCrossCompartmentEdge(JSTracer* trc,
                                           const BarrieredBase<T>* dst,
                                           const char* name) {
#ifdef DEBUG
  if (trc->isMarkingTracer()) {
    T thing = *dst->unbarrieredAddress();
    MOZ_ASSERT(thing->maybeCompartment(),
               "Use TraceEdge for GC things without a compartment");

    GCMarker* gcMarker = GCMarker::fromTracer(trc);
    MOZ_ASSERT_IF(gcMarker->tracingZone,
                  thing->zone() == gcMarker->tracingZone);
  }

  // Skip compartment checks for this edge.
  if (trc->kind() == JS::TracerKind::CompartmentCheck) {
    return;
  }
#endif

  // Clear expected compartment for cross-compartment edge.
  AutoClearTracingSource acts(trc);
  TraceEdgeInternal(trc, ConvertToBase(dst->unbarrieredAddress()), name);
}
template void js::TraceSameZoneCrossCompartmentEdge(
    JSTracer*, const BarrieredBase<Shape*>*, const char*);

template <typename T>
void js::TraceWeakMapKeyEdgeInternal(JSTracer* trc, Zone* weakMapZone,
                                     T** thingp, const char* name) {
  // We can't use ShouldTraceCrossCompartment here because that assumes the
  // source of the edge is a CCW object which could be used to delay gray
  // marking. Instead, assert that the weak map zone is in the same marking
  // state as the target thing's zone and therefore we can go ahead and mark it.
#ifdef DEBUG
  auto thing = *thingp;
  if (trc->isMarkingTracer()) {
    MOZ_ASSERT(weakMapZone->isGCMarking());
    MOZ_ASSERT(weakMapZone->gcState() == thing->zone()->gcState());
  }
#endif

  // Clear expected compartment for cross-compartment edge.
  AutoClearTracingSource acts(trc);

  TraceEdgeInternal(trc, thingp, name);
}

template <typename T>
void js::TraceWeakMapKeyEdgeInternal(JSTracer* trc, Zone* weakMapZone,
                                     T* thingp, const char* name) {
  // We can't use ShouldTraceCrossCompartment here because that assumes the
  // source of the edge is a CCW object which could be used to delay gray
  // marking. Instead, assert that the weak map zone is in the same marking
  // state as the target thing's zone and therefore we can go ahead and mark it.
#ifdef DEBUG
  if (trc->isMarkingTracer()) {
    MOZ_ASSERT(weakMapZone->isGCMarking());
    MOZ_ASSERT(weakMapZone->gcState() ==
               gc::ToMarkable(*thingp)->zone()->gcState());
  }
#endif

  // Clear expected compartment for cross-compartment edge.
  AutoClearTracingSource acts(trc);

  TraceEdgeInternal(trc, thingp, name);
}

template void js::TraceWeakMapKeyEdgeInternal<JSObject>(JSTracer*, Zone*,
                                                        JSObject**,
                                                        const char*);
template void js::TraceWeakMapKeyEdgeInternal<BaseScript>(JSTracer*, Zone*,
                                                          BaseScript**,
                                                          const char*);
template void js::TraceWeakMapKeyEdgeInternal<JS::Value>(JSTracer*, Zone*,
                                                         JS::Value*,
                                                         const char*);

static Cell* TraceGenericPointerRootAndType(JSTracer* trc, Cell* thing,
                                            JS::TraceKind kind,
                                            const char* name) {
  return MapGCThingTyped(thing, kind, [trc, name](auto t) -> Cell* {
    TraceRoot(trc, &t, name);
    return t;
  });
}

void js::TraceGenericPointerRoot(JSTracer* trc, Cell** thingp,
                                 const char* name) {
  MOZ_ASSERT(thingp);
  Cell* thing = *thingp;
  if (!thing) {
    return;
  }

  Cell* traced =
      TraceGenericPointerRootAndType(trc, thing, thing->getTraceKind(), name);
  if (traced != thing) {
    *thingp = traced;
  }
}

void js::TraceManuallyBarrieredGenericPointerEdge(JSTracer* trc, Cell** thingp,
                                                  const char* name) {
  MOZ_ASSERT(thingp);
  Cell* thing = *thingp;
  if (!*thingp) {
    return;
  }

  auto* traced = MapGCThingTyped(thing, thing->getTraceKind(),
                                 [trc, name](auto t) -> Cell* {
                                   TraceManuallyBarrieredEdge(trc, &t, name);
                                   return t;
                                 });
  if (traced != thing) {
    *thingp = traced;
  }
}

void js::TraceGCCellPtrRoot(JSTracer* trc, JS::GCCellPtr* thingp,
                            const char* name) {
  Cell* thing = thingp->asCell();
  if (!thing) {
    return;
  }

  Cell* traced =
      TraceGenericPointerRootAndType(trc, thing, thingp->kind(), name);

  if (!traced) {
    *thingp = JS::GCCellPtr();
  } else if (traced != thingp->asCell()) {
    *thingp = JS::GCCellPtr(traced, thingp->kind());
  }
}

void js::TraceManuallyBarrieredGCCellPtr(JSTracer* trc, JS::GCCellPtr* thingp,
                                         const char* name) {
  Cell* thing = thingp->asCell();
  if (!thing) {
    return;
  }

  Cell* traced = MapGCThingTyped(thing, thing->getTraceKind(),
                                 [trc, name](auto t) -> Cell* {
                                   TraceManuallyBarrieredEdge(trc, &t, name);
                                   return t;
                                 });

  if (!traced) {
    // If we are clearing edges, also erase the type. This happens when using
    // ClearEdgesTracer.
    *thingp = JS::GCCellPtr();
  } else if (traced != thingp->asCell()) {
    *thingp = JS::GCCellPtr(traced, thingp->kind());
  }
}

template <typename T>
inline bool TraceTaggedPtrEdge(JSTracer* trc, T* thingp, const char* name) {
  // Return true by default. For some types the lambda below won't be called.
  bool ret = true;
  auto thing = MapGCThingTyped(*thingp, [&](auto thing) {
    if (!TraceEdgeInternal(trc, &thing, name)) {
      ret = false;
      return TaggedPtr<T>::empty();
    }

    return TaggedPtr<T>::wrap(thing);
  });

  // Only update *thingp if the value changed, to avoid TSan false positives for
  // template objects when using DumpHeapTracer or UbiNode tracers while Ion
  // compiling off-thread.
  if (thing.isSome() && thing.value() != *thingp) {
    *thingp = thing.value();
  }

  return ret;
}

bool js::gc::TraceEdgeInternal(JSTracer* trc, Value* thingp, const char* name) {
  return TraceTaggedPtrEdge(trc, thingp, name);
}
bool js::gc::TraceEdgeInternal(JSTracer* trc, jsid* thingp, const char* name) {
  return TraceTaggedPtrEdge(trc, thingp, name);
}
bool js::gc::TraceEdgeInternal(JSTracer* trc, TaggedProto* thingp,
                               const char* name) {
  return TraceTaggedPtrEdge(trc, thingp, name);
}
bool js::gc::TraceEdgeInternal(JSTracer* trc, wasm::AnyRef* thingp,
                               const char* name) {
  return TraceTaggedPtrEdge(trc, thingp, name);
}

template <typename T>
void js::gc::TraceRangeInternal(JSTracer* trc, size_t len, T* vec,
                                const char* name) {
  JS::AutoTracingIndex index(trc);
  for (auto i : IntegerRange(len)) {
    if (InternalBarrierMethods<T>::isMarkable(vec[i])) {
      TraceEdgeInternal(trc, &vec[i], name);
    }
    ++index;
  }
}

/*** GC Marking Interface ***************************************************/

namespace js {

void GCMarker::markEphemeronEdges(EphemeronEdgeVector& edges,
                                  gc::MarkColor srcColor) {
  // This is called as part of GC weak marking or by barriers outside of GC.
  MOZ_ASSERT_IF(CurrentThreadIsPerformingGC(),
                state == MarkingState::WeakMarking);

  DebugOnly<size_t> initialLength = edges.length();

  for (auto& edge : edges) {
    MarkColor targetColor = std::min(srcColor, MarkColor(edge.color));
    MOZ_ASSERT(markColor() >= targetColor);
    if (targetColor == markColor()) {
      ApplyGCThingTyped(edge.target, edge.target->getTraceKind(),
                        [this](auto t) {
                          markAndTraverse<MarkingOptions::MarkImplicitEdges>(t);
                        });
    }
  }

  // The above marking always goes through markAndPush, which will not cause
  // 'edges' to be appended to while iterating.
  MOZ_ASSERT(edges.length() == initialLength);

  // This is not just an optimization. When nuking a CCW, we conservatively
  // mark through the related edges and then lose the CCW->target connection
  // that induces a sweep group edge. As a result, it is possible for the
  // delegate zone to get marked later, look up an edge in this table, and
  // then try to mark something in a Zone that is no longer marking.
  if (srcColor == MarkColor::Black && markColor() == MarkColor::Black) {
    edges.eraseIf([](auto& edge) { return edge.color == MarkColor::Black; });
  }
}

template <typename T>
struct TypeCanHaveImplicitEdges : std::false_type {};
template <>
struct TypeCanHaveImplicitEdges<JSObject> : std::true_type {};
template <>
struct TypeCanHaveImplicitEdges<BaseScript> : std::true_type {};
template <>
struct TypeCanHaveImplicitEdges<JS::Symbol> : std::true_type {};

template <typename T>
void GCMarker::markImplicitEdges(T* markedThing) {
  if constexpr (!TypeCanHaveImplicitEdges<T>::value) {
    return;
  }

  if (!isWeakMarking()) {
    return;
  }

  Zone* zone = markedThing->asTenured().zone();
  MOZ_ASSERT(zone->isGCMarking());
  MOZ_ASSERT(!zone->isGCSweeping());

  auto& ephemeronTable = zone->gcEphemeronEdges();
  auto* p = ephemeronTable.get(markedThing);
  if (!p) {
    return;
  }

  EphemeronEdgeVector& edges = p->value;

  // markedThing might be a key in a debugger weakmap, which can end up marking
  // values that are in a different compartment.
  AutoClearTracingSource acts(tracer());

  MarkColor thingColor = markColor();
  MOZ_ASSERT(CellColor(thingColor) ==
             gc::detail::GetEffectiveColor(this, markedThing));

  markEphemeronEdges(edges, thingColor);

  if (edges.empty()) {
    ephemeronTable.remove(p);
  }
}

template void GCMarker::markImplicitEdges(JSObject*);
template void GCMarker::markImplicitEdges(BaseScript*);
#ifdef NIGHTLY_BUILD
template void GCMarker::markImplicitEdges(JS::Symbol*);
#endif

}  // namespace js

template <typename T>
static inline bool ShouldMark(GCMarker* gcmarker, T* thing) {
  // We may encounter nursery things during normal marking since we don't
  // collect the nursery at the start of every GC slice.
  if (!thing->isTenured()) {
    return false;
  }

  // Don't mark things outside a zone if we are in a per-zone GC. Don't mark
  // permanent shared things owned by other runtimes (we will never observe
  // their zone being collected).
  Zone* zone = thing->asTenured().zoneFromAnyThread();
  return zone->shouldMarkInZone(gcmarker->markColor());
}

template <uint32_t opts>
MarkingTracerT<opts>::MarkingTracerT(JSRuntime* runtime, GCMarker* marker)
    : GenericTracerImpl<MarkingTracerT<opts>>(
          runtime, JS::TracerKind::Marking,
          JS::TraceOptions(JS::WeakMapTraceAction::Expand,
                           JS::WeakEdgeTraceAction::Skip)) {
  // Marking tracers are owned by (and part of) a GCMarker.
  MOZ_ASSERT(this == marker->tracer());
  MOZ_ASSERT(getMarker() == marker);
}

template <uint32_t opts>
MOZ_ALWAYS_INLINE GCMarker* MarkingTracerT<opts>::getMarker() {
  return GCMarker::fromTracer(this);
}

template <uint32_t opts>
template <typename T>
void MarkingTracerT<opts>::onEdge(T** thingp, const char* name) {
  T* thing = *thingp;

  // Do per-type marking precondition checks.
  GCMarker* marker = getMarker();
  if (!ShouldMark(marker, thing)) {
    MOZ_ASSERT(gc::detail::GetEffectiveColor(marker, thing) ==
               js::gc::CellColor::Black);
    return;
  }

  MOZ_ASSERT(!IsOwnedByOtherRuntime(this->runtime(), thing));

#ifdef DEBUG
  CheckMarkedThing(marker, thing);
#endif

  AutoClearTracingSource acts(this);
  marker->markAndTraverse<opts>(thing);
}

#define INSTANTIATE_ONEDGE_METHOD(name, type, _1, _2)                 \
  template void MarkingTracerT<MarkingOptions::None>::onEdge<type>(   \
      type * *thingp, const char* name);                              \
  template void                                                       \
  MarkingTracerT<MarkingOptions::MarkImplicitEdges>::onEdge<type>(    \
      type * *thingp, const char* name);                              \
  template void                                                       \
  MarkingTracerT<MarkingOptions::MarkRootCompartments>::onEdge<type>( \
      type * *thingp, const char* name);
JS_FOR_EACH_TRACEKIND(INSTANTIATE_ONEDGE_METHOD)
#undef INSTANTIATE_ONEDGE_METHOD

static void TraceEdgeForBarrier(GCMarker* gcmarker, TenuredCell* thing,
                                JS::TraceKind kind) {
  // Dispatch to markAndTraverse without checking ShouldMark.
  ApplyGCThingTyped(thing, kind, [gcmarker](auto thing) {
    MOZ_ASSERT(ShouldMark(gcmarker, thing));
    CheckTracedThing(gcmarker->tracer(), thing);
    AutoClearTracingSource acts(gcmarker->tracer());
#ifdef DEBUG
    AutoSetThreadIsMarking threadIsMarking;
#endif  // DEBUG
    gcmarker->markAndTraverse<NormalMarkingOptions>(thing);
  });
}

JS_PUBLIC_API void js::gc::PerformIncrementalReadBarrier(JS::GCCellPtr thing) {
  // Optimized marking for read barriers. This is called from
  // ExposeGCThingToActiveJS which has already checked the prerequisites for
  // performing a read barrier. This means we can skip a bunch of checks and
  // call into the tracer directly.

  MOZ_ASSERT(thing);
  MOZ_ASSERT(!JS::RuntimeHeapIsCollecting());

  TenuredCell* cell = &thing.asCell()->asTenured();
  MOZ_ASSERT(!cell->isMarkedBlack());

  Zone* zone = cell->zone();
  MOZ_ASSERT(zone->needsIncrementalBarrier());

  // Skip dispatching on known tracer type.
  GCMarker* gcmarker = GCMarker::fromTracer(zone->barrierTracer());
  TraceEdgeForBarrier(gcmarker, cell, thing.kind());
}

void js::gc::PerformIncrementalReadBarrier(TenuredCell* cell) {
  // Internal version of previous function.

  MOZ_ASSERT(cell);
  MOZ_ASSERT(!JS::RuntimeHeapIsCollecting());

  if (cell->isMarkedBlack()) {
    return;
  }

  Zone* zone = cell->zone();
  MOZ_ASSERT(zone->needsIncrementalBarrier());

  // Skip dispatching on known tracer type.
  GCMarker* gcmarker = GCMarker::fromTracer(zone->barrierTracer());
  TraceEdgeForBarrier(gcmarker, cell, cell->getTraceKind());
}

void js::gc::PerformIncrementalPreWriteBarrier(TenuredCell* cell) {
  // The same as PerformIncrementalReadBarrier except for an extra check on the
  // runtime for cells in atoms zone.

  Zone* zone = cell->zoneFromAnyThread();

  MOZ_ASSERT(cell);
  if (cell->isMarkedBlack()) {
    return;
  }

  // Barriers can be triggered off the main thread by background finalization of
  // HeapPtrs to the atoms zone. We don't want to trigger the barrier in this
  // case.
  bool checkThread = zone->isAtomsZone();
  JSRuntime* runtime = cell->runtimeFromAnyThread();
  if (checkThread && !CurrentThreadCanAccessRuntime(runtime)) {
    MOZ_ASSERT(CurrentThreadIsGCFinalizing());
    return;
  }

  MOZ_ASSERT(zone->needsIncrementalBarrier());
  MOZ_ASSERT(CurrentThreadIsMainThread());
  MOZ_ASSERT(!JS::RuntimeHeapIsMajorCollecting());

  // Skip dispatching on known tracer type.
  GCMarker* gcmarker = GCMarker::fromTracer(zone->barrierTracer());
  TraceEdgeForBarrier(gcmarker, cell, cell->getTraceKind());
}

void js::gc::PerformIncrementalBarrierDuringFlattening(JSString* str) {
  TenuredCell* cell = &str->asTenured();

  // Skip eager marking of ropes during flattening. Their children will also be
  // barriered by flattening process so we don't need to traverse them.
  if (str->isRope()) {
    cell->markBlack();
    return;
  }

  PerformIncrementalPreWriteBarrier(cell);
}

template <uint32_t opts, typename T>
void js::GCMarker::markAndTraverse(T* thing) {
  if (mark<opts>(thing)) {
    // We only mark permanent things during initialization.
    MOZ_ASSERT_IF(thing->isPermanentAndMayBeShared(),
                  !runtime()->permanentAtomsPopulated());

    // We don't need to pass MarkRootCompartments options on to children.
    constexpr uint32_t traverseOpts =
        opts & ~MarkingOptions::MarkRootCompartments;

    traverse<traverseOpts>(thing);

    if constexpr (bool(opts & MarkingOptions::MarkRootCompartments)) {
      // Mark the compartment as live.
      SetCompartmentHasMarkedCells(thing);
    }
  }
}

// The |traverse| method overloads select the traversal strategy for each kind.
//
// There are three possible strategies:
//
// 1. traceChildren
//
//    The simplest traversal calls out to the fully generic traceChildren
//    function to visit the child edges. In the absence of other traversal
//    mechanisms, this function will rapidly grow the stack past its bounds and
//    crash the process. Thus, this generic tracing should only be used in cases
//    where subsequent tracing will not recurse.
//
// 2. scanChildren
//
//    Strings, Shapes, and Scopes are extremely common, but have simple patterns
//    of recursion. We traverse trees of these edges immediately, with
//    aggressive, manual inlining, implemented by eagerlyTraceChildren.
//
// 3. pushThing
//
//    Objects are extremely common and can contain arbitrarily nested graphs, so
//    are not trivially inlined. In this case we use the mark stack to control
//    recursion. JitCode shares none of these properties, but is included for
//    historical reasons. JSScript normally cannot recurse, but may be used as a
//    weakmap key and thereby recurse into weakmapped values.

template <uint32_t opts>
void GCMarker::traverse(BaseShape* thing) {
  traceChildren<opts>(thing);
}
template <uint32_t opts>
void GCMarker::traverse(GetterSetter* thing) {
  traceChildren<opts>(thing);
}
template <uint32_t opts>
void GCMarker::traverse(JS::Symbol* thing) {
#ifdef NIGHTLY_BUILD
  if constexpr (bool(opts & MarkingOptions::MarkImplicitEdges)) {
    markImplicitEdges(thing);
  }
#endif
  traceChildren<opts>(thing);
}
template <uint32_t opts>
void GCMarker::traverse(JS::BigInt* thing) {
  traceChildren<opts>(thing);
}
template <uint32_t opts>
void GCMarker::traverse(RegExpShared* thing) {
  traceChildren<opts>(thing);
}
template <uint32_t opts>
void GCMarker::traverse(JSString* thing) {
  scanChildren<opts>(thing);
}
template <uint32_t opts>
void GCMarker::traverse(Shape* thing) {
  scanChildren<opts>(thing);
}
template <uint32_t opts>
void GCMarker::traverse(PropMap* thing) {
  scanChildren<opts>(thing);
}
template <uint32_t opts>
void GCMarker::traverse(js::Scope* thing) {
  scanChildren<opts>(thing);
}
template <uint32_t opts>
void GCMarker::traverse(JSObject* thing) {
  pushThing<opts>(thing);
}
template <uint32_t opts>
void GCMarker::traverse(jit::JitCode* thing) {
  pushThing<opts>(thing);
}
template <uint32_t opts>
void GCMarker::traverse(BaseScript* thing) {
  pushThing<opts>(thing);
}

template <uint32_t opts, typename T>
void js::GCMarker::traceChildren(T* thing) {
  MOZ_ASSERT(!thing->isPermanentAndMayBeShared());
  MOZ_ASSERT(thing->isMarkedAny());
  AutoSetTracingSource asts(tracer(), thing);
  thing->traceChildren(tracer());
}

template <uint32_t opts, typename T>
void js::GCMarker::scanChildren(T* thing) {
  MOZ_ASSERT(!thing->isPermanentAndMayBeShared());
  MOZ_ASSERT(thing->isMarkedAny());
  eagerlyMarkChildren<opts>(thing);
}

template <uint32_t opts, typename T>
void js::GCMarker::pushThing(T* thing) {
  MOZ_ASSERT(!thing->isPermanentAndMayBeShared());
  MOZ_ASSERT(thing->isMarkedAny());
  pushTaggedPtr(thing);
}

template void js::GCMarker::markAndTraverse<MarkingOptions::None, JSObject>(
    JSObject* thing);
template void js::GCMarker::markAndTraverse<MarkingOptions::MarkImplicitEdges,
                                            JSObject>(JSObject* thing);
template void js::GCMarker::markAndTraverse<
    MarkingOptions::MarkRootCompartments, JSObject>(JSObject* thing);

#ifdef DEBUG
void GCMarker::setCheckAtomMarking(bool check) {
  MOZ_ASSERT(check != checkAtomMarking);
  checkAtomMarking = check;
}
#endif

template <typename S, typename T>
inline void GCMarker::checkTraversedEdge(S source, T* target) {
#ifdef DEBUG
  // Atoms and Symbols do not have or mark their internal pointers,
  // respectively.
  MOZ_ASSERT(!source->isPermanentAndMayBeShared());

  // Shared things are already black so we will not mark them.
  if (target->isPermanentAndMayBeShared()) {
    Zone* zone = target->zoneFromAnyThread();
    MOZ_ASSERT(!zone->wasGCStarted());
    MOZ_ASSERT(!zone->needsIncrementalBarrier());
    MOZ_ASSERT(target->isMarkedBlack());
    MOZ_ASSERT(!target->maybeCompartment());
    return;
  }

  Zone* sourceZone = source->zone();
  Zone* targetZone = target->zone();

  // Atoms and Symbols do not have access to a compartment pointer, or we'd need
  // to adjust the subsequent check to catch that case.
  MOZ_ASSERT_IF(targetZone->isAtomsZone(), !target->maybeCompartment());

  // The Zones must match, unless the target is an atom.
  MOZ_ASSERT(targetZone == sourceZone || targetZone->isAtomsZone());

  // If we are marking an atom, that atom must be marked in the source zone's
  // atom bitmap.
  if (checkAtomMarking && !sourceZone->isAtomsZone() &&
      targetZone->isAtomsZone()) {
    MOZ_ASSERT(target->runtimeFromAnyThread()->gc.atomMarking.atomIsMarked(
        sourceZone, reinterpret_cast<TenuredCell*>(target)));
  }

  // If we have access to a compartment pointer for both things, they must
  // match.
  MOZ_ASSERT_IF(source->maybeCompartment() && target->maybeCompartment(),
                source->maybeCompartment() == target->maybeCompartment());
#endif
}

template <uint32_t opts, typename S, typename T>
void js::GCMarker::markAndTraverseEdge(S* source, T* target) {
  checkTraversedEdge(source, target);
  markAndTraverse<opts>(target);
}

template <uint32_t opts, typename S, typename T>
void js::GCMarker::markAndTraverseEdge(S* source, const T& target) {
  ApplyGCThingTyped(target, [this, source](auto t) {
    this->markAndTraverseEdge<opts>(source, t);
  });
}

template <uint32_t opts>
MOZ_NEVER_INLINE bool js::GCMarker::markAndTraversePrivateGCThing(
    JSObject* source, TenuredCell* target) {
  JS::TraceKind kind = target->getTraceKind();
  ApplyGCThingTyped(target, kind, [this, source](auto t) {
    this->markAndTraverseEdge<opts>(source, t);
  });

  // Ensure stack headroom in case we pushed.
  if (MOZ_UNLIKELY(!stack.ensureSpace(ValueRangeWords))) {
    delayMarkingChildrenOnOOM(source);
    return false;
  }

  return true;
}

template <uint32_t opts, typename T>
bool js::GCMarker::mark(T* thing) {
  if (!thing->isTenured()) {
    return false;
  }

  // Don't mark symbols if we're not collecting the atoms zone.
  if constexpr (std::is_same_v<T, JS::Symbol>) {
    if (IsOwnedByOtherRuntime(runtime(), thing) ||
        !thing->zone()->isGCMarkingOrVerifyingPreBarriers()) {
      return false;
    }
  }

  AssertShouldMarkInZone(this, thing);

  MarkColor color =
      TraceKindCanBeGray<T>::value ? markColor() : MarkColor::Black;

  if constexpr (bool(opts & MarkingOptions::ParallelMarking)) {
    return thing->asTenured().markIfUnmarkedAtomic(color);
  }

  return thing->asTenured().markIfUnmarked(color);
}

/*** Mark-stack Marking *****************************************************/

// Call the trace hook set on the object, if present.
static inline void CallTraceHook(JSTracer* trc, JSObject* obj) {
  const JSClass* clasp = obj->getClass();
  MOZ_ASSERT(clasp);

  if (clasp->hasTrace()) {
    AutoSetTracingSource asts(trc, obj);
    clasp->doTrace(trc, obj);
  }
}

static gcstats::PhaseKind GrayMarkingPhaseForCurrentPhase(
    const gcstats::Statistics& stats) {
  using namespace gcstats;
  switch (stats.currentPhaseKind()) {
    case PhaseKind::MARK:
      return PhaseKind::MARK_GRAY;
    case PhaseKind::MARK_WEAK:
      return PhaseKind::MARK_GRAY_WEAK;
    default:
      MOZ_CRASH("Unexpected current phase");
  }
}

void GCMarker::moveWork(GCMarker* dst, GCMarker* src) {
  MOZ_ASSERT(dst->stack.isEmpty());
  MOZ_ASSERT(src->canDonateWork());

  MarkStack::moveWork(dst->stack, src->stack);
}

bool GCMarker::initStack() {
  MOZ_ASSERT(!isActive());
  MOZ_ASSERT(markColor_ == gc::MarkColor::Black);
  return stack.init();
}

void GCMarker::resetStackCapacity() {
  MOZ_ASSERT(!isActive());
  MOZ_ASSERT(markColor_ == gc::MarkColor::Black);
  (void)stack.resetStackCapacity();
}

void GCMarker::freeStack() {
  MOZ_ASSERT(!isActive());
  MOZ_ASSERT(markColor_ == gc::MarkColor::Black);
  stack.clearAndFreeStack();
}

bool GCMarker::markUntilBudgetExhausted(SliceBudget& budget,
                                        ShouldReportMarkTime reportTime) {
#ifdef DEBUG
  MOZ_ASSERT(!strictCompartmentChecking);
  strictCompartmentChecking = true;
  auto acc = mozilla::MakeScopeExit([&] { strictCompartmentChecking = false; });
#endif

  if (budget.isOverBudget()) {
    return false;
  }

  if (isWeakMarking()) {
    return doMarking<MarkingOptions::MarkImplicitEdges>(budget, reportTime);
  }

  return doMarking<MarkingOptions::None>(budget, reportTime);
}

template <uint32_t opts>
bool GCMarker::doMarking(SliceBudget& budget, ShouldReportMarkTime reportTime) {
  GCRuntime& gc = runtime()->gc;

  // This method leaves the mark color as it found it.

  if (hasBlackEntries() && !markOneColor<opts, MarkColor::Black>(budget)) {
    return false;
  }

  if (hasGrayEntries()) {
    mozilla::Maybe<gcstats::AutoPhase> ap;
    if (reportTime) {
      auto& stats = runtime()->gc.stats();
      ap.emplace(stats, GrayMarkingPhaseForCurrentPhase(stats));
    }

    if (!markOneColor<opts, MarkColor::Gray>(budget)) {
      return false;
    }
  }

  // Mark children of things that caused too deep recursion during the above
  // tracing. All normal marking happens before any delayed marking.
  if (gc.hasDelayedMarking()) {
    gc.markAllDelayedChildren(reportTime);
  }

  MOZ_ASSERT(!gc.hasDelayedMarking());
  MOZ_ASSERT(isDrained());

  return true;
}

class MOZ_RAII gc::AutoUpdateMarkStackRanges {
  GCMarker& marker_;

 public:
  explicit AutoUpdateMarkStackRanges(GCMarker& marker) : marker_(marker) {
    marker_.updateRangesAtStartOfSlice();
  }
  ~AutoUpdateMarkStackRanges() { marker_.updateRangesAtEndOfSlice(); }
};

template <uint32_t opts, MarkColor color>
bool GCMarker::markOneColor(SliceBudget& budget) {
  AutoSetMarkColor setColor(*this, color);
  AutoUpdateMarkStackRanges updateRanges(*this);

  while (processMarkStackTop<opts>(budget)) {
    if (stack.isEmpty()) {
      return true;
    }
  }

  return false;
}

bool GCMarker::markCurrentColorInParallel(SliceBudget& budget) {
  AutoUpdateMarkStackRanges updateRanges(*this);

  ParallelMarker::AtomicCount& waitingTaskCount =
      parallelMarker_->waitingTaskCountRef();

  while (processMarkStackTop<MarkingOptions::ParallelMarking>(budget)) {
    if (stack.isEmpty()) {
      return true;
    }

    // TODO: It might be better to only check this occasionally, possibly
    // combined with the slice budget check. Experiments with giving this its
    // own counter resulted in worse performance.
    if (waitingTaskCount && shouldDonateWork()) {
      parallelMarker_->donateWorkFrom(this);
    }
  }

  return false;
}

#ifdef DEBUG
bool GCMarker::markOneObjectForTest(JSObject* obj) {
  MOZ_ASSERT(obj->zone()->isGCMarking());
  MOZ_ASSERT(!obj->isMarked(markColor()));

  size_t oldPosition = stack.position();
  markAndTraverse<NormalMarkingOptions>(obj);
  if (stack.position() == oldPosition) {
    return false;
  }

  AutoUpdateMarkStackRanges updateRanges(*this);

  SliceBudget unlimited = SliceBudget::unlimited();
  processMarkStackTop<NormalMarkingOptions>(unlimited);

  return true;
}
#endif

static inline void CheckForCompartmentMismatch(JSObject* obj, JSObject* obj2) {
#ifdef DEBUG
  if (MOZ_UNLIKELY(obj->compartment() != obj2->compartment())) {
    fprintf(
        stderr,
        "Compartment mismatch in pointer from %s object slot to %s object\n",
        obj->getClass()->name, obj2->getClass()->name);
    MOZ_CRASH("Compartment mismatch");
  }
#endif
}

static inline size_t NumUsedFixedSlots(NativeObject* obj) {
  return std::min(obj->numFixedSlots(), obj->slotSpan());
}

static inline size_t NumUsedDynamicSlots(NativeObject* obj) {
  size_t nfixed = obj->numFixedSlots();
  size_t nslots = obj->slotSpan();
  if (nslots < nfixed) {
    return 0;
  }

  return nslots - nfixed;
}

void GCMarker::updateRangesAtStartOfSlice() {
  for (MarkStackIter iter(stack); !iter.done(); iter.next()) {
    if (iter.isSlotsOrElementsRange()) {
      MarkStack::SlotsOrElementsRange& range = iter.slotsOrElementsRange();
      JSObject* obj = range.ptr().asRangeObject();
      if (!obj->is<NativeObject>()) {
        range.setEmpty();
      } else if (range.kind() == SlotsOrElementsKind::Elements) {
        NativeObject* obj = &range.ptr().asRangeObject()->as<NativeObject>();
        size_t index = range.start();
        size_t numShifted = obj->getElementsHeader()->numShiftedElements();
        index -= std::min(numShifted, index);
        range.setStart(index);
      }
    }
  }

#ifdef DEBUG
  MOZ_ASSERT(!stack.elementsRangesAreValid);
  stack.elementsRangesAreValid = true;
#endif
}

void GCMarker::updateRangesAtEndOfSlice() {
  for (MarkStackIter iter(stack); !iter.done(); iter.next()) {
    if (iter.isSlotsOrElementsRange()) {
      MarkStack::SlotsOrElementsRange& range = iter.slotsOrElementsRange();
      if (range.kind() == SlotsOrElementsKind::Elements) {
        NativeObject* obj = &range.ptr().asRangeObject()->as<NativeObject>();
        size_t numShifted = obj->getElementsHeader()->numShiftedElements();
        range.setStart(range.start() + numShifted);
      }
    }
  }

#ifdef DEBUG
  MOZ_ASSERT(stack.elementsRangesAreValid);
  stack.elementsRangesAreValid = false;
#endif
}

template <uint32_t opts>
inline bool GCMarker::processMarkStackTop(SliceBudget& budget) {
  /*
   * This function uses explicit goto and scans objects directly. This allows us
   * to eliminate tail recursion and significantly improve the marking
   * performance, see bug 641025.
   *
   * Note that the mutator can change the size and layout of objects between
   * marking slices, so we must check slots and element ranges read from the
   * stack.
   */

  MOZ_ASSERT(!stack.isEmpty());
  MOZ_ASSERT(stack.elementsRangesAreValid);
  MOZ_ASSERT_IF(markColor() == MarkColor::Gray, !hasBlackEntries());

  JSObject* obj;             // The object being scanned.
  SlotsOrElementsKind kind;  // The kind of slot range being scanned, if any.
  HeapSlot* base;            // Slot range base pointer.
  size_t index;              // Index of the next slot to mark.
  size_t end;                // End of slot range to mark.

  if (stack.peekTag() == MarkStack::SlotsOrElementsRangeTag) {
    auto range = stack.popSlotsOrElementsRange();
    obj = range.ptr().asRangeObject();
    NativeObject* nobj = &obj->as<NativeObject>();
    kind = range.kind();
    index = range.start();

    switch (kind) {
      case SlotsOrElementsKind::FixedSlots: {
        base = nobj->fixedSlots();
        end = NumUsedFixedSlots(nobj);
        break;
      }

      case SlotsOrElementsKind::DynamicSlots: {
        base = nobj->slots_;
        end = NumUsedDynamicSlots(nobj);
        break;
      }

      case SlotsOrElementsKind::Elements: {
        base = nobj->getDenseElements();
        end = nobj->getDenseInitializedLength();
        break;
      }

      case SlotsOrElementsKind::Unused: {
        MOZ_MAKE_COMPILER_ASSUME_IS_UNREACHABLE("Unused SlotsOrElementsKind");
      }
    }

    goto scan_value_range;
  }

  budget.step();
  if (budget.isOverBudget()) {
    return false;
  }

  {
    MarkStack::TaggedPtr ptr = stack.popPtr();
    switch (ptr.tag()) {
      case MarkStack::ObjectTag: {
        obj = ptr.as<JSObject>();
        AssertShouldMarkInZone(this, obj);
        goto scan_obj;
      }

      case MarkStack::JitCodeTag: {
        auto* code = ptr.as<jit::JitCode>();
        AutoSetTracingSource asts(tracer(), code);
        code->traceChildren(tracer());
        return true;
      }

      case MarkStack::ScriptTag: {
        auto* script = ptr.as<BaseScript>();
        if constexpr (bool(opts & MarkingOptions::MarkImplicitEdges)) {
          markImplicitEdges(script);
        }
        AutoSetTracingSource asts(tracer(), script);
        script->traceChildren(tracer());
        return true;
      }

      default:
        MOZ_CRASH("Invalid tag in mark stack");
    }
  }

  return true;

scan_value_range:
  while (index < end) {
    MOZ_ASSERT(stack.capacity() >= stack.position() + ValueRangeWords);

    budget.step();
    if (budget.isOverBudget()) {
      pushValueRange(obj, kind, index, end);
      return false;
    }

    const Value& v = base[index];
    index++;

    if (!v.isGCThing()) {
      continue;
    }

    if (v.isString()) {
      markAndTraverseEdge<opts>(obj, v.toString());
    } else if (v.hasObjectPayload()) {
      JSObject* obj2 = &v.getObjectPayload();
#ifdef DEBUG
      if (!obj2) {
        fprintf(stderr,
                "processMarkStackTop found ObjectValue(nullptr) "
                "at %zu Values from end of range in object:\n",
                size_t(end - (index - 1)));
        obj->dump();
      }
#endif
      CheckForCompartmentMismatch(obj, obj2);
      if (mark<opts>(obj2)) {
        // Save the rest of this value range for later and start scanning obj2's
        // children.
        pushValueRange(obj, kind, index, end);
        obj = obj2;
        goto scan_obj;
      }
    } else if (v.isSymbol()) {
      markAndTraverseEdge<opts>(obj, v.toSymbol());
    } else if (v.isBigInt()) {
      markAndTraverseEdge<opts>(obj, v.toBigInt());
    } else {
      MOZ_ASSERT(v.isPrivateGCThing());
      if (!markAndTraversePrivateGCThing<opts>(obj,
                                               &v.toGCThing()->asTenured())) {
        return true;
      }
    }
  }

  return true;

scan_obj: {
  AssertShouldMarkInZone(this, obj);

  if constexpr (bool(opts & MarkingOptions::MarkImplicitEdges)) {
    markImplicitEdges(obj);
  }
  markAndTraverseEdge<opts>(obj, obj->shape());

  CallTraceHook(tracer(), obj);

  if (!obj->is<NativeObject>()) {
    return true;
  }

  NativeObject* nobj = &obj->as<NativeObject>();

  // Ensure stack headroom for three ranges (fixed slots, dynamic slots and
  // elements).
  if (MOZ_UNLIKELY(!stack.ensureSpace(ValueRangeWords * 3))) {
    delayMarkingChildrenOnOOM(obj);
    return true;
  }

  unsigned nslots = nobj->slotSpan();

  if (!nobj->hasEmptyElements()) {
    base = nobj->getDenseElements();
    kind = SlotsOrElementsKind::Elements;
    index = 0;
    end = nobj->getDenseInitializedLength();

    if (!nslots) {
      // No slots at all. Scan elements immediately.
      goto scan_value_range;
    }

    pushValueRange(nobj, kind, index, end);
  }

  unsigned nfixed = nobj->numFixedSlots();
  base = nobj->fixedSlots();
  kind = SlotsOrElementsKind::FixedSlots;
  index = 0;

  if (nslots > nfixed) {
    // Push dynamic slots for later scan.
    pushValueRange(nobj, SlotsOrElementsKind::DynamicSlots, 0, nslots - nfixed);
    end = nfixed;
  } else {
    end = nslots;
  }

  // Scan any fixed slots.
  goto scan_value_range;
}
}

/*** Mark Stack *************************************************************/

static_assert(sizeof(MarkStack::TaggedPtr) == sizeof(uintptr_t),
              "A TaggedPtr should be the same size as a pointer");
static_assert((sizeof(MarkStack::SlotsOrElementsRange) % sizeof(uintptr_t)) ==
                  0,
              "SlotsOrElementsRange size should be a multiple of "
              "the pointer size");

template <typename T>
struct MapTypeToMarkStackTag {};
template <>
struct MapTypeToMarkStackTag<JSObject*> {
  static const auto value = MarkStack::ObjectTag;
};
template <>
struct MapTypeToMarkStackTag<jit::JitCode*> {
  static const auto value = MarkStack::JitCodeTag;
};
template <>
struct MapTypeToMarkStackTag<BaseScript*> {
  static const auto value = MarkStack::ScriptTag;
};

static inline bool TagIsRangeTag(MarkStack::Tag tag) {
  return tag == MarkStack::SlotsOrElementsRangeTag;
}

inline MarkStack::TaggedPtr::TaggedPtr(Tag tag, Cell* ptr)
    : bits(tag | uintptr_t(ptr)) {
  assertValid();
}

inline uintptr_t MarkStack::TaggedPtr::asBits() const { return bits; }

inline uintptr_t MarkStack::TaggedPtr::tagUnchecked() const {
  return bits & TagMask;
}

inline MarkStack::Tag MarkStack::TaggedPtr::tag() const {
  auto tag = Tag(bits & TagMask);
  MOZ_ASSERT(tag <= LastTag);
  return tag;
}

inline Cell* MarkStack::TaggedPtr::ptr() const {
  return reinterpret_cast<Cell*>(bits & ~TagMask);
}

inline void MarkStack::TaggedPtr::assertValid() const {
  (void)tag();
  MOZ_ASSERT(IsCellPointerValid(ptr()));
}

template <typename T>
inline T* MarkStack::TaggedPtr::as() const {
  MOZ_ASSERT(tag() == MapTypeToMarkStackTag<T*>::value);
  MOZ_ASSERT(ptr()->isTenured());
  MOZ_ASSERT(ptr()->is<T>());
  return static_cast<T*>(ptr());
}

inline JSObject* MarkStack::TaggedPtr::asRangeObject() const {
  MOZ_ASSERT(TagIsRangeTag(tag()));
  MOZ_ASSERT(ptr()->isTenured());
  return ptr()->as<JSObject>();
}

inline JSRope* MarkStack::TaggedPtr::asTempRope() const {
  MOZ_ASSERT(tag() == TempRopeTag);
  return &ptr()->as<JSString>()->asRope();
}

inline MarkStack::SlotsOrElementsRange::SlotsOrElementsRange(
    SlotsOrElementsKind kindArg, JSObject* obj, size_t startArg)
    : startAndKind_((startArg << StartShift) | size_t(kindArg)),
      ptr_(SlotsOrElementsRangeTag, obj) {
  assertValid();
  MOZ_ASSERT(kind() == kindArg);
  MOZ_ASSERT(start() == startArg);
}

inline void MarkStack::SlotsOrElementsRange::assertValid() const {
  ptr_.assertValid();
  MOZ_ASSERT(TagIsRangeTag(ptr_.tag()));
}

inline SlotsOrElementsKind MarkStack::SlotsOrElementsRange::kind() const {
  return SlotsOrElementsKind(startAndKind_ & KindMask);
}

inline size_t MarkStack::SlotsOrElementsRange::start() const {
  return startAndKind_ >> StartShift;
}

inline void MarkStack::SlotsOrElementsRange::setStart(size_t newStart) {
  startAndKind_ = (newStart << StartShift) | uintptr_t(kind());
  MOZ_ASSERT(start() == newStart);
}

inline void MarkStack::SlotsOrElementsRange::setEmpty() {
  TaggedPtr entry = TaggedPtr(ObjectTag, ptr().asRangeObject());
  ptr_ = entry;
  startAndKind_ = entry.asBits();
}

inline MarkStack::TaggedPtr MarkStack::SlotsOrElementsRange::ptr() const {
  return ptr_;
}

MarkStack::MarkStack() { MOZ_ASSERT(isEmpty()); }

MarkStack::~MarkStack() { MOZ_ASSERT(isEmpty()); }

void MarkStack::swap(MarkStack& other) {
  std::swap(stack_, other.stack_);
  std::swap(topIndex_, other.topIndex_);
#ifdef JS_GC_ZEAL
  std::swap(maxCapacity_, other.maxCapacity_);
#endif
#ifdef DEBUG
  std::swap(elementsRangesAreValid, other.elementsRangesAreValid);
#endif
}

bool MarkStack::init() { return resetStackCapacity(); }

bool MarkStack::resetStackCapacity() {
  MOZ_ASSERT(isEmpty());

  size_t capacity = MARK_STACK_BASE_CAPACITY;

#ifdef JS_GC_ZEAL
  capacity = std::min(capacity, maxCapacity_.ref());
#endif

  return resize(capacity);
}

#ifdef JS_GC_ZEAL
void MarkStack::setMaxCapacity(size_t maxCapacity) {
  MOZ_ASSERT(maxCapacity != 0);
  MOZ_ASSERT(isEmpty());

  maxCapacity_ = maxCapacity;
  if (capacity() > maxCapacity_) {
    // If the realloc fails, just keep using the existing stack; it's
    // not ideal but better than failing.
    (void)resize(maxCapacity_);
  }
}
#endif

MOZ_ALWAYS_INLINE bool MarkStack::indexIsEntryBase(size_t index) const {
  // The mark stack holds both TaggedPtr and SlotsOrElementsRange entries, which
  // are one or two words long respectively. Determine whether |index| points to
  // the base of an entry (i.e. the lowest word in memory).
  //
  // The possible cases are that |index| points to:
  //  1. a single word TaggedPtr entry => true
  //  2. the startAndKind_ word of SlotsOrElementsRange => true
  //     (startAndKind_ is a uintptr_t tagged with SlotsOrElementsKind)
  //  3. the ptr_ word of SlotsOrElementsRange (itself a TaggedPtr) => false
  //
  // To check for case 3, interpret the word as a TaggedPtr: if it is tagged as
  // a SlotsOrElementsRange tagged pointer then we are inside such a range and
  // |index| does not point to the base of an entry. This requires that no
  // startAndKind_ word can be interpreted as such, which is arranged by making
  // SlotsOrElementsRangeTag zero and all SlotsOrElementsKind tags non-zero.

  MOZ_ASSERT(index < position());
  return stack()[index].tagUnchecked() != SlotsOrElementsRangeTag;
}

/* static */
void MarkStack::moveWork(MarkStack& dst, MarkStack& src) {
  // Move some work from |src| to |dst|. Assumes |dst| is empty.
  //
  // When this method runs during parallel marking, we are on the thread that
  // owns |src|, and the thread that owns |dst| is blocked waiting on the
  // ParallelMarkTask::resumed condition variable.

  // Limit the size of moves to stop threads with work spending too much time
  // donating.
  static const size_t MaxWordsToMove = 4096;

  size_t totalWords = src.position();
  size_t wordsToMove = std::min(totalWords / 2, MaxWordsToMove);

  size_t targetPos = src.position() - wordsToMove;

  // Adjust the target position in case it points to the middle of a two word
  // entry.
  if (!src.indexIsEntryBase(targetPos)) {
    targetPos--;
    wordsToMove++;
  }
  MOZ_ASSERT(src.indexIsEntryBase(targetPos));
  MOZ_ASSERT(targetPos < src.position());
  MOZ_ASSERT(targetPos > 0);
  MOZ_ASSERT(wordsToMove == src.position() - targetPos);

  if (!dst.ensureSpace(wordsToMove)) {
    return;
  }

  // TODO: This doesn't have good cache behaviour when moving work between
  // threads. It might be better if the original thread ended up with the top
  // part of the stack, in src words if this method stole from the bottom of
  // the stack rather than the top.

  mozilla::PodCopy(dst.topPtr(), src.stack().begin() + targetPos, wordsToMove);
  dst.topIndex_ += wordsToMove;
  dst.peekPtr().assertValid();

  src.topIndex_ = targetPos;
#ifdef DEBUG
  src.poisonUnused();
#endif
  src.peekPtr().assertValid();
}

void MarkStack::clearAndResetCapacity() {
  // Fall back to the smaller initial capacity so we don't hold on to excess
  // memory between GCs.
  stack().clear();
  topIndex_ = 0;
  (void)resetStackCapacity();
}

void MarkStack::clearAndFreeStack() {
  // Free all stack memory so we don't hold on to excess memory between GCs.
  stack().clearAndFree();
  topIndex_ = 0;
}

inline MarkStack::TaggedPtr* MarkStack::topPtr() { return &stack()[topIndex_]; }

template <typename T>
inline bool MarkStack::push(T* ptr) {
  return push(TaggedPtr(MapTypeToMarkStackTag<T*>::value, ptr));
}

inline bool MarkStack::pushTempRope(JSRope* rope) {
  return push(TaggedPtr(TempRopeTag, rope));
}

inline bool MarkStack::push(const TaggedPtr& ptr) {
  if (!ensureSpace(1)) {
    return false;
  }

  infalliblePush(ptr);
  return true;
}

inline void MarkStack::infalliblePush(const TaggedPtr& ptr) {
  *topPtr() = ptr;
  topIndex_++;
  MOZ_ASSERT(position() <= capacity());
}

inline void MarkStack::infalliblePush(JSObject* obj, SlotsOrElementsKind kind,
                                      size_t start) {
  MOZ_ASSERT(position() + ValueRangeWords <= capacity());

  SlotsOrElementsRange array(kind, obj, start);
  array.assertValid();
  *reinterpret_cast<SlotsOrElementsRange*>(topPtr()) = array;
  topIndex_ += ValueRangeWords;
  MOZ_ASSERT(TagIsRangeTag(peekTag()));
}

inline const MarkStack::TaggedPtr& MarkStack::peekPtr() const {
  MOZ_ASSERT(!isEmpty());
  return stack()[topIndex_ - 1];
}

inline MarkStack::Tag MarkStack::peekTag() const {
  MOZ_ASSERT(!isEmpty());
  return peekPtr().tag();
}

inline MarkStack::TaggedPtr MarkStack::popPtr() {
  MOZ_ASSERT(!isEmpty());
  MOZ_ASSERT(!TagIsRangeTag(peekTag()));
  peekPtr().assertValid();
  topIndex_--;
  return *topPtr();
}

inline MarkStack::SlotsOrElementsRange MarkStack::popSlotsOrElementsRange() {
  MOZ_ASSERT(!isEmpty());
  MOZ_ASSERT(TagIsRangeTag(peekTag()));
  MOZ_ASSERT(position() >= ValueRangeWords);

  topIndex_ -= ValueRangeWords;
  const auto& array = *reinterpret_cast<SlotsOrElementsRange*>(topPtr());
  array.assertValid();
  return array;
}

inline bool MarkStack::ensureSpace(size_t count) {
  if (MOZ_LIKELY((topIndex_ + count) <= capacity())) {
    return true;
  }

  return enlarge(count);
}

MOZ_NEVER_INLINE bool MarkStack::enlarge(size_t count) {
  size_t required = capacity() + count;
  size_t newCapacity = mozilla::RoundUpPow2(required);

#ifdef JS_GC_ZEAL
  newCapacity = std::min(newCapacity, maxCapacity_.ref());
  if (newCapacity < required) {
    return false;
  }
#endif

  return resize(newCapacity);
}

bool MarkStack::resize(size_t newCapacity) {
  MOZ_ASSERT(newCapacity != 0);
  MOZ_ASSERT(newCapacity >= position());

  if (!stack().resize(newCapacity)) {
    return false;
  }

  poisonUnused();
  return true;
}

inline void MarkStack::poisonUnused() {
  static_assert((JS_FRESH_MARK_STACK_PATTERN & TagMask) > LastTag,
                "The mark stack poison pattern must not look like a valid "
                "tagged pointer");

  AlwaysPoison(stack().begin() + topIndex_, JS_FRESH_MARK_STACK_PATTERN,
               stack().capacity() - topIndex_, MemCheckKind::MakeUndefined);
}

size_t MarkStack::sizeOfExcludingThis(
    mozilla::MallocSizeOf mallocSizeOf) const {
  return stack().sizeOfExcludingThis(mallocSizeOf);
}

MarkStackIter::MarkStackIter(MarkStack& stack)
    : stack_(stack), pos_(stack.position()) {}

inline size_t MarkStackIter::position() const { return pos_; }

inline bool MarkStackIter::done() const { return position() == 0; }

inline void MarkStackIter::next() {
  if (isSlotsOrElementsRange()) {
    MOZ_ASSERT(position() >= ValueRangeWords);
    pos_ -= ValueRangeWords;
    return;
  }

  MOZ_ASSERT(!done());
  pos_--;
}

inline bool MarkStackIter::isSlotsOrElementsRange() const {
  return TagIsRangeTag(peekTag());
}

inline MarkStack::Tag MarkStackIter::peekTag() const { return peekPtr().tag(); }

inline MarkStack::TaggedPtr MarkStackIter::peekPtr() const {
  MOZ_ASSERT(!done());
  return stack_.stack()[pos_ - 1];
}

inline MarkStack::SlotsOrElementsRange& MarkStackIter::slotsOrElementsRange() {
  MOZ_ASSERT(TagIsRangeTag(peekTag()));
  MOZ_ASSERT(position() >= ValueRangeWords);

  MarkStack::TaggedPtr* ptr = &stack_.stack()[pos_ - ValueRangeWords];
  auto& range = *reinterpret_cast<MarkStack::SlotsOrElementsRange*>(ptr);
  range.assertValid();
  return range;
}

/*** GCMarker ***************************************************************/

/*
 * WeakMapTraceAction::Expand: the GC is recomputing the liveness of WeakMap
 * entries by expanding each live WeakMap into its constituent key->value edges,
 * a table of which will be consulted in a later phase whenever marking a
 * potential key.
 */
GCMarker::GCMarker(JSRuntime* rt)
    : tracer_(mozilla::VariantType<MarkingTracer>(), rt, this),
      runtime_(rt),
      haveSwappedStacks(false),
      markColor_(MarkColor::Black),
      state(NotActive),
      incrementalWeakMapMarkingEnabled(
          TuningDefaults::IncrementalWeakMapMarkingEnabled)
#ifdef DEBUG
      ,
      checkAtomMarking(true),
      strictCompartmentChecking(false)
#endif
{
}

bool GCMarker::init() { return stack.init(); }

void GCMarker::start() {
  MOZ_ASSERT(state == NotActive);
  MOZ_ASSERT(stack.isEmpty());
  state = RegularMarking;
  haveAllImplicitEdges = true;
  setMarkColor(MarkColor::Black);
}

static void ClearEphemeronEdges(JSRuntime* rt) {
  AutoEnterOOMUnsafeRegion oomUnsafe;
  for (GCZonesIter zone(rt); !zone.done(); zone.next()) {
    if (!zone->gcEphemeronEdges().clear()) {
      oomUnsafe.crash("clearing weak keys in GCMarker::stop()");
    }
    if (!zone->gcNurseryEphemeronEdges().clear()) {
      oomUnsafe.crash("clearing (nursery) weak keys in GCMarker::stop()");
    }
  }
}

void GCMarker::stop() {
  MOZ_ASSERT(isDrained());
  MOZ_ASSERT(markColor() == MarkColor::Black);
  MOZ_ASSERT(!haveSwappedStacks);

  if (state == NotActive) {
    return;
  }
  state = NotActive;

  otherStack.clearAndFreeStack();
  ClearEphemeronEdges(runtime());
  unmarkGrayStack.clearAndFree();
}

void GCMarker::reset() {
  state = NotActive;

  stack.clearAndResetCapacity();
  otherStack.clearAndFreeStack();
  ClearEphemeronEdges(runtime());
  MOZ_ASSERT(isDrained());

  setMarkColor(MarkColor::Black);
  MOZ_ASSERT(!haveSwappedStacks);

  unmarkGrayStack.clearAndFree();
}

void GCMarker::setMarkColor(gc::MarkColor newColor) {
  if (markColor_.ref() == newColor) {
    return;
  }

  // We don't support gray marking while there is black marking work to do.
  MOZ_ASSERT(!hasBlackEntries());

  markColor_ = newColor;

  // Switch stacks. We only need to do this if there are any stack entries (as
  // empty stacks are interchangeable) or to swtich back to the original stack.
  if (!isDrained() || haveSwappedStacks) {
    stack.swap(otherStack);
    haveSwappedStacks = !haveSwappedStacks;
  }
}

bool GCMarker::hasEntries(MarkColor color) const {
  const MarkStack& stackForColor = color == markColor() ? stack : otherStack;
  return stackForColor.hasEntries();
}

template <typename T>
inline void GCMarker::pushTaggedPtr(T* ptr) {
  checkZone(ptr);
  if (!stack.push(ptr)) {
    delayMarkingChildrenOnOOM(ptr);
  }
}

inline void GCMarker::pushValueRange(JSObject* obj, SlotsOrElementsKind kind,
                                     size_t start, size_t end) {
  checkZone(obj);
  MOZ_ASSERT(obj->is<NativeObject>());
  MOZ_ASSERT(start <= end);

  if (start != end) {
    stack.infalliblePush(obj, kind, start);
  }
}

void GCMarker::setRootMarkingMode(bool newState) {
  if (newState) {
    setMarkingStateAndTracer<RootMarkingTracer>(RegularMarking, RootMarking);
  } else {
    setMarkingStateAndTracer<MarkingTracer>(RootMarking, RegularMarking);
  }
}

void GCMarker::enterParallelMarkingMode(ParallelMarker* pm) {
  MOZ_ASSERT(pm);
  MOZ_ASSERT(!parallelMarker_);
  setMarkingStateAndTracer<ParallelMarkingTracer>(RegularMarking,
                                                  ParallelMarking);
  parallelMarker_ = pm;
}

void GCMarker::leaveParallelMarkingMode() {
  MOZ_ASSERT(parallelMarker_);
  setMarkingStateAndTracer<MarkingTracer>(ParallelMarking, RegularMarking);
  parallelMarker_ = nullptr;
}

// It may not be worth the overhead of donating very few mark stack entries. For
// some (non-parallelizable) workloads this could lead to constantly
// interrupting marking work and makes parallel marking slower than single
// threaded.
//
// Conversely, we do want to try splitting up work occasionally or we may fail
// to parallelize workloads that result in few mark stack entries.
//
// Therefore we try hard to split work up at the start of a slice (calling
// canDonateWork) but when a slice is running we only donate if there is enough
// work to make it worthwhile (calling shouldDonateWork).
bool GCMarker::canDonateWork() const {
  return stack.position() > ValueRangeWords;
}
bool GCMarker::shouldDonateWork() const {
  constexpr size_t MinWordCount = 12;
  static_assert(MinWordCount >= ValueRangeWords,
                "We must always leave at least one stack entry.");

  return stack.position() > MinWordCount;
}

template <typename Tracer>
void GCMarker::setMarkingStateAndTracer(MarkingState prev, MarkingState next) {
  MOZ_ASSERT(state == prev);
  state = next;
  tracer_.emplace<Tracer>(runtime(), this);
}

bool GCMarker::enterWeakMarkingMode() {
  MOZ_ASSERT(tracer()->weakMapAction() == JS::WeakMapTraceAction::Expand);
  if (!haveAllImplicitEdges) {
    return false;
  }

  // During weak marking mode, we maintain a table mapping weak keys to
  // entries in known-live weakmaps. Initialize it with the keys of marked
  // weakmaps -- or more precisely, the keys of marked weakmaps that are
  // mapped to not yet live values. (Once bug 1167452 implements incremental
  // weakmap marking, this initialization step will become unnecessary, as
  // the table will already hold all such keys.)

  // Set state before doing anything else, so any new key that is marked
  // during the following gcEphemeronEdges scan will itself be looked up in
  // gcEphemeronEdges and marked according to ephemeron rules.
  setMarkingStateAndTracer<WeakMarkingTracer>(RegularMarking, WeakMarking);

  return true;
}

IncrementalProgress JS::Zone::enterWeakMarkingMode(GCMarker* marker,
                                                   SliceBudget& budget) {
  MOZ_ASSERT(marker->isWeakMarking());

  if (!marker->incrementalWeakMapMarkingEnabled) {
    for (WeakMapBase* m : gcWeakMapList()) {
      if (IsMarked(m->mapColor())) {
        (void)m->markEntries(marker);
      }
    }
    return IncrementalProgress::Finished;
  }

  // gcEphemeronEdges contains the keys from all weakmaps marked so far, or at
  // least the keys that might still need to be marked through. Scan through
  // gcEphemeronEdges and mark all values whose keys are marked. This marking
  // may recursively mark through other weakmap entries (immediately since we
  // are now in WeakMarking mode). The end result is a consistent state where
  // all values are marked if both their map and key are marked -- though note
  // that we may later leave weak marking mode, do some more marking, and then
  // enter back in.
  if (!isGCMarking()) {
    return IncrementalProgress::Finished;
  }

  MOZ_ASSERT(gcNurseryEphemeronEdges().count() == 0);

  // An OrderedHashMap::MutableRange stays valid even when the underlying table
  // (zone->gcEphemeronEdges) is mutated, which is useful here since we may add
  // additional entries while iterating over the Range.
  EphemeronEdgeTable::MutableRange r = gcEphemeronEdges().mutableAll();
  while (!r.empty()) {
    Cell* src = r.front().key;
    CellColor srcColor = gc::detail::GetEffectiveColor(marker, src);
    auto& edges = r.front().value;
    r.popFront();  // Pop before any mutations happen.

    if (IsMarked(srcColor) && edges.length() > 0) {
      uint32_t steps = edges.length();
      marker->markEphemeronEdges(edges, AsMarkColor(srcColor));
      budget.step(steps);
      if (budget.isOverBudget()) {
        return NotFinished;
      }
    }
  }

  return IncrementalProgress::Finished;
}

void GCMarker::leaveWeakMarkingMode() {
  if (state == RegularMarking) {
    return;
  }

  setMarkingStateAndTracer<MarkingTracer>(WeakMarking, RegularMarking);

  // The gcEphemeronEdges table is still populated and may be used during a
  // future weak marking mode within this GC.
}

void GCMarker::abortLinearWeakMarking() {
  haveAllImplicitEdges = false;
  if (state == WeakMarking) {
    leaveWeakMarkingMode();
  }
}

MOZ_NEVER_INLINE void GCMarker::delayMarkingChildrenOnOOM(Cell* cell) {
  runtime()->gc.delayMarkingChildren(cell, markColor());
}

bool GCRuntime::hasDelayedMarking() const {
  bool result = delayedMarkingList;
  MOZ_ASSERT(result == (markLaterArenas != 0));
  return result;
}

void GCRuntime::delayMarkingChildren(Cell* cell, MarkColor color) {
  // Synchronize access to delayed marking state during parallel marking.
  LockGuard<Mutex> lock(delayedMarkingLock);

  Arena* arena = cell->asTenured().arena();
  if (!arena->onDelayedMarkingList()) {
    arena->setNextDelayedMarkingArena(delayedMarkingList);
    delayedMarkingList = arena;
#ifdef DEBUG
    markLaterArenas++;
#endif
  }

  if (!arena->hasDelayedMarking(color)) {
    arena->setHasDelayedMarking(color, true);
    delayedMarkingWorkAdded = true;
  }
}

void GCRuntime::markDelayedChildren(Arena* arena, MarkColor color) {
  JSTracer* trc = marker().tracer();
  JS::TraceKind kind = MapAllocToTraceKind(arena->getAllocKind());
  MarkColor colorToCheck =
      TraceKindCanBeMarkedGray(kind) ? color : MarkColor::Black;

  for (ArenaCellIterUnderGC cell(arena); !cell.done(); cell.next()) {
    if (cell->isMarked(colorToCheck)) {
      ApplyGCThingTyped(cell, kind, [trc, this](auto t) {
        t->traceChildren(trc);
        marker().markImplicitEdges(t);
      });
    }
  }
}

/*
 * Process arenas from |delayedMarkingList| by marking the unmarked children of
 * marked cells of color |color|.
 *
 * This is called twice, first to mark gray children and then to mark black
 * children.
 */
void GCRuntime::processDelayedMarkingList(MarkColor color) {
  // Marking delayed children may add more arenas to the list, including arenas
  // we are currently processing or have previously processed. Handle this by
  // clearing a flag on each arena before marking its children. This flag will
  // be set again if the arena is re-added. Iterate the list until no new arenas
  // were added.

  AutoSetMarkColor setColor(marker(), color);
  AutoUpdateMarkStackRanges updateRanges(marker());

  do {
    delayedMarkingWorkAdded = false;
    for (Arena* arena = delayedMarkingList; arena;
         arena = arena->getNextDelayedMarking()) {
      if (arena->hasDelayedMarking(color)) {
        arena->setHasDelayedMarking(color, false);
        markDelayedChildren(arena, color);
      }
    }
    while (marker().hasEntriesForCurrentColor()) {
      SliceBudget budget = SliceBudget::unlimited();
      MOZ_ALWAYS_TRUE(
          marker().processMarkStackTop<NormalMarkingOptions>(budget));
    }
  } while (delayedMarkingWorkAdded);

  MOZ_ASSERT(marker().isDrained());
}

void GCRuntime::markAllDelayedChildren(ShouldReportMarkTime reportTime) {
  MOZ_ASSERT(CurrentThreadIsMainThread() || CurrentThreadIsPerformingGC());
  MOZ_ASSERT(marker().isDrained());
  MOZ_ASSERT(hasDelayedMarking());

  mozilla::Maybe<gcstats::AutoPhase> ap;
  if (reportTime) {
    ap.emplace(stats(), gcstats::PhaseKind::MARK_DELAYED);
  }

  // We have a list of arenas containing marked cells with unmarked children
  // where we ran out of stack space during marking. Both black and gray cells
  // in these arenas may have unmarked children. Mark black children first.

  const MarkColor colors[] = {MarkColor::Black, MarkColor::Gray};
  for (MarkColor color : colors) {
    processDelayedMarkingList(color);
    rebuildDelayedMarkingList();
  }

  MOZ_ASSERT(!hasDelayedMarking());
}

void GCRuntime::rebuildDelayedMarkingList() {
  // Rebuild the delayed marking list, removing arenas which do not need further
  // marking.

  Arena* listTail = nullptr;
  forEachDelayedMarkingArena([&](Arena* arena) {
    if (!arena->hasAnyDelayedMarking()) {
      arena->clearDelayedMarkingState();
#ifdef DEBUG
      MOZ_ASSERT(markLaterArenas);
      markLaterArenas--;
#endif
      return;
    }

    appendToDelayedMarkingList(&listTail, arena);
  });
  appendToDelayedMarkingList(&listTail, nullptr);
}

void GCRuntime::resetDelayedMarking() {
  MOZ_ASSERT(CurrentThreadIsMainThread());

  forEachDelayedMarkingArena([&](Arena* arena) {
    MOZ_ASSERT(arena->onDelayedMarkingList());
    arena->clearDelayedMarkingState();
#ifdef DEBUG
    MOZ_ASSERT(markLaterArenas);
    markLaterArenas--;
#endif
  });
  delayedMarkingList = nullptr;
  MOZ_ASSERT(!markLaterArenas);
}

inline void GCRuntime::appendToDelayedMarkingList(Arena** listTail,
                                                  Arena* arena) {
  if (*listTail) {
    (*listTail)->updateNextDelayedMarkingArena(arena);
  } else {
    delayedMarkingList = arena;
  }
  *listTail = arena;
}

template <typename F>
inline void GCRuntime::forEachDelayedMarkingArena(F&& f) {
  Arena* arena = delayedMarkingList;
  Arena* next;
  while (arena) {
    next = arena->getNextDelayedMarking();
    f(arena);
    arena = next;
  }
}

#ifdef DEBUG
void GCMarker::checkZone(void* p) {
  MOZ_ASSERT(state != NotActive);
  DebugOnly<Cell*> cell = static_cast<Cell*>(p);
  MOZ_ASSERT_IF(cell->isTenured(),
                cell->asTenured().zone()->isCollectingFromAnyThread());
}
#endif

size_t GCMarker::sizeOfIncludingThis(mozilla::MallocSizeOf mallocSizeOf) const {
  return mallocSizeOf(this) + stack.sizeOfExcludingThis(mallocSizeOf) +
         otherStack.sizeOfExcludingThis(mallocSizeOf);
}

/*** IsMarked / IsAboutToBeFinalized ****************************************/

template <typename T>
static inline void CheckIsMarkedThing(T* thing) {
#define IS_SAME_TYPE_OR(name, type, _, _1) std::is_same_v<type, T> ||
  static_assert(JS_FOR_EACH_TRACEKIND(IS_SAME_TYPE_OR) false,
                "Only the base cell layout types are allowed into "
                "marking/tracing internals");
#undef IS_SAME_TYPE_OR

#ifdef DEBUG
  MOZ_ASSERT(thing);

  // Allow any thread access to uncollected things.
  Zone* zone = thing->zoneFromAnyThread();
  if (thing->isPermanentAndMayBeShared()) {
    MOZ_ASSERT(!zone->wasGCStarted());
    MOZ_ASSERT(!zone->needsIncrementalBarrier());
    MOZ_ASSERT(thing->isMarkedBlack());
    return;
  }

  // Allow the current thread access if it is sweeping or in sweep-marking, but
  // try to check the zone. Some threads have access to all zones when sweeping.
  JS::GCContext* gcx = TlsGCContext.get();
  MOZ_ASSERT(gcx->gcUse() != GCUse::Finalizing);
  if (gcx->gcUse() == GCUse::Sweeping || gcx->gcUse() == GCUse::Marking) {
    MOZ_ASSERT_IF(gcx->gcSweepZone(),
                  gcx->gcSweepZone() == zone || zone->isAtomsZone());
    return;
  }

  // Otherwise only allow access from the main thread or this zone's associated
  // thread.
  MOZ_ASSERT(CurrentThreadCanAccessRuntime(thing->runtimeFromAnyThread()) ||
             CurrentThreadCanAccessZone(thing->zoneFromAnyThread()));
#endif
}

template <typename T>
bool js::gc::IsMarkedInternal(JSRuntime* rt, T* thing) {
  // Don't depend on the mark state of other cells during finalization.
  MOZ_ASSERT(!CurrentThreadIsGCFinalizing());
  MOZ_ASSERT(rt->heapState() != JS::HeapState::MinorCollecting);
  MOZ_ASSERT(thing);
  CheckIsMarkedThing(thing);

  // This is not used during minor sweeping nor used to update moved GC things.
  MOZ_ASSERT(!IsForwarded(thing));

  // Permanent things are never marked by non-owning runtimes.
  TenuredCell* cell = &thing->asTenured();
  Zone* zone = cell->zoneFromAnyThread();
#ifdef DEBUG
  if (IsOwnedByOtherRuntime(rt, thing)) {
    MOZ_ASSERT(!zone->wasGCStarted());
    MOZ_ASSERT(thing->isMarkedBlack());
  }
#endif

  return !zone->isGCMarking() || TenuredThingIsMarkedAny(thing);
}

template <typename T>
bool js::gc::IsAboutToBeFinalizedInternal(T* thing) {
  // Don't depend on the mark state of other cells during finalization.
  MOZ_ASSERT(!CurrentThreadIsGCFinalizing());
  MOZ_ASSERT(thing);
  CheckIsMarkedThing(thing);

  // This is not used during minor sweeping nor used to update moved GC things.
  MOZ_ASSERT(!IsForwarded(thing));

  if (!thing->isTenured()) {
    return false;
  }

  // Permanent things are never finalized by non-owning runtimes.
  TenuredCell* cell = &thing->asTenured();
  Zone* zone = cell->zoneFromAnyThread();
#ifdef DEBUG
  JSRuntime* rt = TlsGCContext.get()->runtimeFromAnyThread();
  if (IsOwnedByOtherRuntime(rt, thing)) {
    MOZ_ASSERT(!zone->wasGCStarted());
    MOZ_ASSERT(thing->isMarkedBlack());
  }
#endif

  return zone->isGCSweeping() && !TenuredThingIsMarkedAny(thing);
}

template <typename T>
bool js::gc::IsAboutToBeFinalizedInternal(const T& thing) {
  bool dying = false;
  ApplyGCThingTyped(
      thing, [&dying](auto t) { dying = IsAboutToBeFinalizedInternal(t); });
  return dying;
}

SweepingTracer::SweepingTracer(JSRuntime* rt)
    : GenericTracerImpl(rt, JS::TracerKind::Sweeping,
                        JS::WeakMapTraceAction::TraceKeysAndValues) {}

template <typename T>
inline void SweepingTracer::onEdge(T** thingp, const char* name) {
  T* thing = *thingp;
  CheckIsMarkedThing(thing);

  if (!thing->isTenured()) {
    return;
  }

  // Permanent things are never finalized by non-owning runtimes.
  TenuredCell* cell = &thing->asTenured();
  Zone* zone = cell->zoneFromAnyThread();
#ifdef DEBUG
  if (IsOwnedByOtherRuntime(runtime(), thing)) {
    MOZ_ASSERT(!zone->wasGCStarted());
    MOZ_ASSERT(thing->isMarkedBlack());
  }
#endif

  // It would be nice if we could assert that the zone of the tenured cell is in
  // the Sweeping state, but that isn't always true for:
  //  - atoms
  //  - the jitcode map
  //  - the mark queue
  if ((zone->isGCSweeping() || zone->isAtomsZone()) && !cell->isMarkedAny()) {
    *thingp = nullptr;
  }
}

namespace js::gc {

template <typename T>
JS_PUBLIC_API bool TraceWeakEdge(JSTracer* trc, JS::Heap<T>* thingp) {
  return TraceEdgeInternal(trc, gc::ConvertToBase(thingp->unsafeGet()),
                           "JS::Heap edge");
}

template <typename T>
JS_PUBLIC_API bool EdgeNeedsSweepUnbarrieredSlow(T* thingp) {
  return IsAboutToBeFinalizedInternal(*ConvertToBase(thingp));
}

// Instantiate a copy of the Tracing templates for each public GC type.
#define INSTANTIATE_ALL_VALID_HEAP_TRACE_FUNCTIONS(type)            \
  template JS_PUBLIC_API bool TraceWeakEdge<type>(JSTracer * trc,   \
                                                  JS::Heap<type>*); \
  template JS_PUBLIC_API bool EdgeNeedsSweepUnbarrieredSlow<type>(type*);
JS_FOR_EACH_PUBLIC_GC_POINTER_TYPE(INSTANTIATE_ALL_VALID_HEAP_TRACE_FUNCTIONS)
JS_FOR_EACH_PUBLIC_TAGGED_GC_POINTER_TYPE(
    INSTANTIATE_ALL_VALID_HEAP_TRACE_FUNCTIONS)

#define INSTANTIATE_INTERNAL_IS_MARKED_FUNCTION(type) \
  template bool IsMarkedInternal(JSRuntime* rt, type thing);

#define INSTANTIATE_INTERNAL_IATBF_FUNCTION(type) \
  template bool IsAboutToBeFinalizedInternal(type thingp);

#define INSTANTIATE_INTERNAL_MARKING_FUNCTIONS_FROM_TRACEKIND(_1, type, _2, \
                                                              _3)           \
  INSTANTIATE_INTERNAL_IS_MARKED_FUNCTION(type*)                            \
  INSTANTIATE_INTERNAL_IATBF_FUNCTION(type*)

JS_FOR_EACH_TRACEKIND(INSTANTIATE_INTERNAL_MARKING_FUNCTIONS_FROM_TRACEKIND)

#define INSTANTIATE_IATBF_FUNCTION_FOR_TAGGED_POINTER(type) \
  INSTANTIATE_INTERNAL_IATBF_FUNCTION(const type&)

JS_FOR_EACH_PUBLIC_TAGGED_GC_POINTER_TYPE(
    INSTANTIATE_IATBF_FUNCTION_FOR_TAGGED_POINTER)

#undef INSTANTIATE_INTERNAL_IS_MARKED_FUNCTION
#undef INSTANTIATE_INTERNAL_IATBF_FUNCTION
#undef INSTANTIATE_INTERNAL_MARKING_FUNCTIONS_FROM_TRACEKIND
#undef INSTANTIATE_IATBF_FUNCTION_FOR_TAGGED_POINTER

}  // namespace js::gc

/*** Cycle Collector Barrier Implementation *********************************/

/*
 * The GC and CC are run independently. Consequently, the following sequence of
 * events can occur:
 * 1. GC runs and marks an object gray.
 * 2. The mutator runs (specifically, some C++ code with access to gray
 *    objects) and creates a pointer from a JS root or other black object to
 *    the gray object. If we re-ran a GC at this point, the object would now be
 *    black.
 * 3. Now we run the CC. It may think it can collect the gray object, even
 *    though it's reachable from the JS heap.
 *
 * To prevent this badness, we unmark the gray bit of an object when it is
 * accessed by callers outside XPConnect. This would cause the object to go
 * black in step 2 above. This must be done on everything reachable from the
 * object being returned. The following code takes care of the recursive
 * re-coloring.
 *
 * There is an additional complication for certain kinds of edges that are not
 * contained explicitly in the source object itself, such as from a weakmap key
 * to its value. These "implicit edges" are represented in some other
 * container object, such as the weakmap itself. In these
 * cases, calling unmark gray on an object won't find all of its children.
 *
 * Handling these implicit edges has two parts:
 * - A special pass enumerating all of the containers that know about the
 *   implicit edges to fix any black-gray edges that have been created. This
 *   is implemented in nsXPConnect::FixWeakMappingGrayBits.
 * - To prevent any incorrectly gray objects from escaping to live JS outside
 *   of the containers, we must add unmark-graying read barriers to these
 *   containers.
 */

#ifdef DEBUG
struct AssertNonGrayTracer final : public JS::CallbackTracer {
  // This is used by the UnmarkGray tracer only, and needs to report itself as
  // the non-gray tracer to not trigger assertions.  Do not use it in another
  // context without making this more generic.
  explicit AssertNonGrayTracer(JSRuntime* rt)
      : JS::CallbackTracer(rt, JS::TracerKind::UnmarkGray) {}
  void onChild(JS::GCCellPtr thing, const char* name) override {
    MOZ_ASSERT(!thing.asCell()->isMarkedGray());
  }
};
#endif

class js::gc::UnmarkGrayTracer final : public JS::CallbackTracer {
 public:
  // We set weakMapAction to WeakMapTraceAction::Skip because the cycle
  // collector will fix up any color mismatches involving weakmaps when it runs.
  explicit UnmarkGrayTracer(GCMarker* marker)
      : JS::CallbackTracer(marker->runtime(), JS::TracerKind::UnmarkGray,
                           JS::WeakMapTraceAction::Skip),
        unmarkedAny(false),
        oom(false),
        marker(marker),
        stack(marker->unmarkGrayStack) {}

  void unmark(JS::GCCellPtr cell);

  // Whether we unmarked anything.
  bool unmarkedAny;

  // Whether we ran out of memory.
  bool oom;

 private:
  // Marker to use if we need to unmark in zones that are currently being
  // marked.
  GCMarker* marker;

  // Stack of cells to traverse.
  Vector<JS::GCCellPtr, 0, SystemAllocPolicy>& stack;

  void onChild(JS::GCCellPtr thing, const char* name) override;
};

void UnmarkGrayTracer::onChild(JS::GCCellPtr thing, const char* name) {
  Cell* cell = thing.asCell();

  // Cells in the nursery cannot be gray, and nor can certain kinds of tenured
  // cells. These must necessarily point only to black edges.
  if (!cell->isTenured() || !TraceKindCanBeMarkedGray(thing.kind())) {
#ifdef DEBUG
    MOZ_ASSERT(!cell->isMarkedGray());
    AssertNonGrayTracer nongray(runtime());
    JS::TraceChildren(&nongray, thing);
#endif
    return;
  }

  TenuredCell& tenured = cell->asTenured();
  Zone* zone = tenured.zone();

  // If the cell is in a zone whose mark bits are being cleared, then it will
  // end up white.
  if (zone->isGCPreparing()) {
    return;
  }

  // If the cell is in a zone that we're currently marking, then it's possible
  // that it is currently white but will end up gray. To handle this case,
  // trigger the barrier for any cells in zones that are currently being
  // marked. This will ensure they will eventually get marked black.
  if (zone->isGCMarking()) {
    if (!cell->isMarkedBlack()) {
      TraceEdgeForBarrier(marker, &tenured, thing.kind());
      unmarkedAny = true;
    }
    return;
  }

  if (!tenured.isMarkedGray()) {
    return;
  }

  // TODO: It may be a small improvement to only use the atomic version during
  // parallel marking.
  tenured.markBlackAtomic();
  unmarkedAny = true;

  if (!stack.append(thing)) {
    oom = true;
  }
}

void UnmarkGrayTracer::unmark(JS::GCCellPtr cell) {
  MOZ_ASSERT(stack.empty());

  onChild(cell, "unmarking root");

  while (!stack.empty() && !oom) {
    TraceChildren(this, stack.popCopy());
  }

  if (oom) {
    // If we run out of memory, we take a drastic measure: require that we
    // GC again before the next CC.
    stack.clear();
    runtime()->gc.setGrayBitsInvalid();
    return;
  }
}

bool js::gc::UnmarkGrayGCThingUnchecked(GCMarker* marker, JS::GCCellPtr thing) {
  MOZ_ASSERT(thing);
  MOZ_ASSERT(thing.asCell()->isMarkedGray());

  mozilla::Maybe<AutoGeckoProfilerEntry> profilingStackFrame;
  if (JSContext* cx = TlsContext.get()) {
    profilingStackFrame.emplace(cx, "UnmarkGrayGCThing",
                                JS::ProfilingCategoryPair::GCCC_UnmarkGray);
  }

  UnmarkGrayTracer unmarker(marker);
  unmarker.unmark(thing);
  return unmarker.unmarkedAny;
}

JS_PUBLIC_API bool JS::UnmarkGrayGCThingRecursively(JS::GCCellPtr thing) {
  MOZ_ASSERT(!JS::RuntimeHeapIsCollecting());
  MOZ_ASSERT(!JS::RuntimeHeapIsCycleCollecting());

  JSRuntime* rt = thing.asCell()->runtimeFromMainThread();
  if (thing.asCell()->zone()->isGCPreparing()) {
    // Mark bits are being cleared in preparation for GC.
    return false;
  }

  return UnmarkGrayGCThingUnchecked(&rt->gc.marker(), thing);
}

void js::gc::UnmarkGrayGCThingRecursively(TenuredCell* cell) {
  JS::UnmarkGrayGCThingRecursively(JS::GCCellPtr(cell, cell->getTraceKind()));
}

bool js::UnmarkGrayShapeRecursively(Shape* shape) {
  return JS::UnmarkGrayGCThingRecursively(JS::GCCellPtr(shape));
}

#ifdef DEBUG
Cell* js::gc::UninlinedForwarded(const Cell* cell) { return Forwarded(cell); }
#endif

namespace js::debug {

MarkInfo GetMarkInfo(void* vp) {
  GCRuntime& gc = TlsGCContext.get()->runtime()->gc;
  if (gc.nursery().isInside(vp)) {
    return MarkInfo::NURSERY;
  }

  if (!gc.isPointerWithinTenuredCell(vp)) {
    return MarkInfo::UNKNOWN;
  }

  if (!IsCellPointerValid(vp)) {
    return MarkInfo::UNKNOWN;
  }

  TenuredCell* cell = reinterpret_cast<TenuredCell*>(vp);
  if (cell->isMarkedGray()) {
    return MarkInfo::GRAY;
  }
  if (cell->isMarkedBlack()) {
    return MarkInfo::BLACK;
  }
  return MarkInfo::UNMARKED;
}

uintptr_t* GetMarkWordAddress(Cell* cell) {
  if (!cell->isTenured()) {
    return nullptr;
  }

  MarkBitmapWord* wordp;
  uintptr_t mask;
  TenuredChunkBase* chunk = gc::detail::GetCellChunkBase(&cell->asTenured());
  chunk->markBits.getMarkWordAndMask(&cell->asTenured(), ColorBit::BlackBit,
                                     &wordp, &mask);
  return reinterpret_cast<uintptr_t*>(wordp);
}

uintptr_t GetMarkMask(Cell* cell, uint32_t colorBit) {
  MOZ_ASSERT(colorBit == 0 || colorBit == 1);

  if (!cell->isTenured()) {
    return 0;
  }

  ColorBit bit = colorBit == 0 ? ColorBit::BlackBit : ColorBit::GrayOrBlackBit;
  MarkBitmapWord* wordp;
  uintptr_t mask;
  TenuredChunkBase* chunk = gc::detail::GetCellChunkBase(&cell->asTenured());
  chunk->markBits.getMarkWordAndMask(&cell->asTenured(), bit, &wordp, &mask);
  return mask;
}

}  // namespace js::debug
