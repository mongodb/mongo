/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "gc/Marking-inl.h"

#include "mozilla/DebugOnly.h"
#include "mozilla/IntegerRange.h"
#include "mozilla/Maybe.h"
#include "mozilla/ReentrancyGuard.h"
#include "mozilla/ScopeExit.h"

#include <algorithm>
#include <initializer_list>
#include <type_traits>

#include "builtin/ModuleObject.h"
#include "debugger/DebugAPI.h"
#include "gc/GCInternals.h"
#include "gc/GCProbes.h"
#include "gc/Policy.h"
#include "jit/JitCode.h"
#include "js/friend/DumpFunctions.h"  // js::DumpObject
#include "js/GCTypeMacros.h"  // JS_FOR_EACH_PUBLIC_{,TAGGED_}GC_POINTER_TYPE
#include "js/SliceBudget.h"
#include "util/DiagnosticAssertions.h"
#include "util/Memory.h"
#include "util/Poison.h"
#include "vm/ArgumentsObject.h"
#include "vm/ArrayObject.h"
#include "vm/BigIntType.h"
#include "vm/GeneratorObject.h"
#include "vm/GetterSetter.h"
#include "vm/PropMap.h"
#include "vm/RegExpShared.h"
#include "vm/Scope.h"  // GetScopeDataTrailingNames
#include "vm/Shape.h"
#include "vm/SymbolType.h"
#include "vm/TypedArrayObject.h"
#include "wasm/WasmJS.h"

#include "gc/GC-inl.h"
#include "gc/Nursery-inl.h"
#include "gc/PrivateIterators-inl.h"
#include "gc/WeakMap-inl.h"
#include "gc/Zone-inl.h"
#include "vm/GeckoProfiler-inl.h"
#include "vm/JSScript-inl.h"
#include "vm/NativeObject-inl.h"
#include "vm/PlainObject-inl.h"  // js::PlainObject
#include "vm/Realm-inl.h"
#include "vm/StringType-inl.h"

#define MAX_DEDUPLICATABLE_STRING_LENGTH 500

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
//  +----------------------+                             ...................
//  |                      |                             :                 :
//  |                      v                             v                 :
//  |      TraceRoot   TraceEdge   TraceRange        GCMarker::            :
//  |          |           |           |         processMarkStackTop   +---+---+
//  |          +-----------+-----------+                 |             |       |
//  |                      |                             |             | Mark  |
//  |                      v                             |             | Stack |
//  |              TraceEdgeInternal                     |             |       |
//  |                      |                             |             +---+---+
//  |                      |                             |                 ^
//  |         +------------+---------------+             +<----------+     :
//  |         |                            |             |           |     :
//  |         v                            v             v           |     :
//  |    DoCallback                    DoMarking markAndTraverseEdge |     :
//  |         |                            |             |           |     :
//  |         |                            +------+------+           |     :
//  |         |                                   |                  |     :
//  |         v                                   v                  |     :
//  |  CallbackTracer::                    markAndTraverse           |     :
//  |    onSomeEdge                               |                  |     :
//  |                                             |                  |     :
//  |                                          traverse              |     :
//  |                                             |                  |     :
//  |             +-------------------+-----------+------+           |     :
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

/*** Tracing Invariants *****************************************************/

#if defined(DEBUG)
template <typename T>
static inline bool IsThingPoisoned(T* thing) {
  const uint8_t poisonBytes[] = {
      JS_FRESH_NURSERY_PATTERN,      JS_SWEPT_NURSERY_PATTERN,
      JS_ALLOCATED_NURSERY_PATTERN,  JS_FRESH_TENURED_PATTERN,
      JS_MOVED_TENURED_PATTERN,      JS_SWEPT_TENURED_PATTERN,
      JS_ALLOCATED_TENURED_PATTERN,  JS_FREED_HEAP_PTR_PATTERN,
      JS_FREED_CHUNK_PATTERN,        JS_FREED_ARENA_PATTERN,
      JS_SWEPT_TI_PATTERN,           JS_SWEPT_CODE_PATTERN,
      JS_RESET_VALUE_PATTERN,        JS_POISONED_JSSCRIPT_DATA_PATTERN,
      JS_OOB_PARSE_NODE_PATTERN,     JS_LIFO_UNDEFINED_PATTERN,
      JS_LIFO_UNINITIALIZED_PATTERN,
  };
  const int numPoisonBytes = sizeof(poisonBytes) / sizeof(poisonBytes[0]);
  uint32_t* p =
      reinterpret_cast<uint32_t*>(reinterpret_cast<FreeSpan*>(thing) + 1);
  // Note: all free patterns are odd to make the common, not-poisoned case a
  // single test.
  if ((*p & 1) == 0) {
    return false;
  }
  for (int i = 0; i < numPoisonBytes; ++i) {
    const uint8_t pb = poisonBytes[i];
    const uint32_t pw = pb | (pb << 8) | (pb << 16) | (pb << 24);
    if (*p == pw) {
      return true;
    }
  }
  return false;
}
#endif

template <typename T>
static inline bool IsOwnedByOtherRuntime(JSRuntime* rt, T thing) {
  bool other = thing->runtimeFromAnyThread() != rt;
  MOZ_ASSERT_IF(other, thing->isPermanentAndMayBeShared() ||
                           thing->zoneFromAnyThread()->isSelfHostingZone());
  return other;
}

#ifdef DEBUG

template <typename T>
void js::CheckTracedThing(JSTracer* trc, T* thing) {
  MOZ_ASSERT(trc);
  MOZ_ASSERT(thing);

  if (IsForwarded(thing)) {
    MOZ_ASSERT(IsTracerKind(trc, JS::TracerKind::Moving) ||
               trc->isTenuringTracer());
    thing = Forwarded(thing);
  }

  /* This function uses data that's not available in the nursery. */
  if (IsInsideNursery(thing)) {
    return;
  }

  /*
   * Permanent atoms and things in the self-hosting zone are not associated
   * with this runtime, but will be ignored during marking.
   */
  if (IsOwnedByOtherRuntime(trc->runtime(), thing)) {
    return;
  }

  Zone* zone = thing->zoneFromAnyThread();
  JSRuntime* rt = trc->runtime();
  MOZ_ASSERT(zone->runtimeFromAnyThread() == rt);

  bool isGcMarkingTracer = trc->isMarkingTracer();
  bool isUnmarkGrayTracer = IsTracerKind(trc, JS::TracerKind::UnmarkGray);
  bool isClearEdgesTracer = IsTracerKind(trc, JS::TracerKind::ClearEdges);

  if (TlsContext.get()->isMainThreadContext()) {
    // If we're on the main thread we must have access to the runtime and zone.
    MOZ_ASSERT(CurrentThreadCanAccessRuntime(rt));
    MOZ_ASSERT(CurrentThreadCanAccessZone(zone));
  } else {
    MOZ_ASSERT(isGcMarkingTracer || isUnmarkGrayTracer || isClearEdgesTracer ||
               IsTracerKind(trc, JS::TracerKind::Moving) ||
               IsTracerKind(trc, JS::TracerKind::GrayBuffering) ||
               IsTracerKind(trc, JS::TracerKind::Sweeping));
    MOZ_ASSERT_IF(!isClearEdgesTracer, CurrentThreadIsPerformingGC());
  }

  MOZ_ASSERT(thing->isAligned());
  MOZ_ASSERT(MapTypeToTraceKind<std::remove_pointer_t<T>>::kind ==
             thing->getTraceKind());

  if (isGcMarkingTracer) {
    GCMarker* gcMarker = GCMarker::fromTracer(trc);
    MOZ_ASSERT(zone->shouldMarkInZone());

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

  /*
   * Try to assert that the thing is allocated.
   *
   * We would like to assert that the thing is not in the free list, but this
   * check is very slow. Instead we check whether the thing has been poisoned:
   * if it has not then we assume it is allocated, but if it has then it is
   * either free or uninitialized in which case we check the free list.
   *
   * Further complications are that background sweeping may be running and
   * concurrently modifiying the free list and that tracing is done off
   * thread during compacting GC and reading the contents of the thing by
   * IsThingPoisoned would be racy in this case.
   */
  MOZ_ASSERT_IF(JS::RuntimeHeapIsBusy() && !zone->isGCSweeping() &&
                    !zone->isGCFinished() && !zone->isGCCompacting(),
                !IsThingPoisoned(thing) ||
                    !InFreeList(thing->asTenured().arena(), thing));
}

template <typename T>
void js::CheckTracedThing(JSTracer* trc, const T& thing) {
  ApplyGCThingTyped(thing, [trc](auto t) { CheckTracedThing(trc, t); });
}

namespace js {
#  define IMPL_CHECK_TRACED_THING(_, type, _1, _2) \
    template void CheckTracedThing<type>(JSTracer*, type*);
JS_FOR_EACH_TRACEKIND(IMPL_CHECK_TRACED_THING);
#  undef IMPL_CHECK_TRACED_THING
}  // namespace js

#endif

static inline bool ShouldMarkCrossCompartment(GCMarker* marker, JSObject* src,
                                              Cell* dstCell) {
  MarkColor color = marker->markColor();

  if (!dstCell->isTenured()) {
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
      UnmarkGrayGCThingUnchecked(marker->runtime(),
                                 JS::GCCellPtr(&dst, dst.getTraceKind()));
      return false;
    }

    return dstZone->isGCMarking();
  } else {
    // Check our sweep groups are correct as above.
    MOZ_ASSERT_IF(!dst.isMarkedAny(), !dstZone->isGCSweeping());

    if (dstZone->isGCMarkingBlackOnly()) {
      /*
       * The destination compartment is being not being marked gray now,
       * but it will be later, so record the cell so it can be marked gray
       * at the appropriate time.
       */
      if (!dst.isMarkedAny()) {
        DelayCrossCompartmentGrayMarking(src);
      }
      return false;
    }

    return dstZone->isGCMarkingBlackAndGray();
  }
}

static bool ShouldTraceCrossCompartment(JSTracer* trc, JSObject* src,
                                        Cell* dstCell) {
  if (!trc->isMarkingTracer()) {
    return true;
  }

  return ShouldMarkCrossCompartment(GCMarker::fromTracer(trc), src, dstCell);
}

static bool ShouldTraceCrossCompartment(JSTracer* trc, JSObject* src,
                                        const Value& val) {
  return val.isGCThing() &&
         ShouldTraceCrossCompartment(trc, src, val.toGCThing());
}

static void AssertShouldMarkInZone(Cell* thing) {
  MOZ_ASSERT(thing->asTenured().zone()->shouldMarkInZone());
}

static void AssertShouldMarkInZone(JSString* str) {
#ifdef DEBUG
  Zone* zone = str->zone();
  MOZ_ASSERT(zone->shouldMarkInZone() || zone->isAtomsZone());
#endif
}

static void AssertShouldMarkInZone(JS::Symbol* sym) {
#ifdef DEBUG
  Zone* zone = sym->asTenured().zone();
  MOZ_ASSERT(zone->shouldMarkInZone() || zone->isAtomsZone());
#endif
}

#ifdef DEBUG
void js::gc::AssertRootMarkingPhase(JSTracer* trc) {
  MOZ_ASSERT_IF(trc->isMarkingTracer(),
                trc->runtime()->gc.state() == State::NotActive ||
                    trc->runtime()->gc.state() == State::MarkRoots);
}
#endif

/*** Tracing Interface ******************************************************/

template <typename T>
void DoMarking(GCMarker* gcmarker, T* thing);
template <typename T>
void DoMarking(GCMarker* gcmarker, const T& thing);

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
static void UnsafeTraceRootHelper(JSTracer* trc, T* thingp, const char* name) {
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

#define DEFINE_UNSAFE_TRACE_ROOT_FUNCTION(type)                       \
  JS_PUBLIC_API void JS::UnsafeTraceRoot(JSTracer* trc, type* thingp, \
                                         const char* name) {          \
    UnsafeTraceRootHelper(trc, thingp, name);                         \
  }

// Define UnsafeTraceRoot for each public GC pointer type.
JS_FOR_EACH_PUBLIC_GC_POINTER_TYPE(DEFINE_UNSAFE_TRACE_ROOT_FUNCTION)
JS_FOR_EACH_PUBLIC_TAGGED_GC_POINTER_TYPE(DEFINE_UNSAFE_TRACE_ROOT_FUNCTION)

// Also, for the moment, define UnsafeTraceRoot for internal GC pointer types.
DEFINE_UNSAFE_TRACE_ROOT_FUNCTION(AbstractGeneratorObject*)
DEFINE_UNSAFE_TRACE_ROOT_FUNCTION(SavedFrame*)

#undef DEFINE_UNSAFE_TRACE_ROOT_FUNCTION

namespace js {
namespace gc {

#define INSTANTIATE_INTERNAL_TRACE_FUNCTIONS(type)                      \
  template bool TraceEdgeInternal<type>(JSTracer*, type*, const char*); \
  template void TraceRangeInternal<type>(JSTracer*, size_t len, type*,  \
                                         const char*);

#define INSTANTIATE_INTERNAL_TRACE_FUNCTIONS_FROM_TRACEKIND(_1, type, _2, _3) \
  INSTANTIATE_INTERNAL_TRACE_FUNCTIONS(type*)

JS_FOR_EACH_TRACEKIND(INSTANTIATE_INTERNAL_TRACE_FUNCTIONS_FROM_TRACEKIND)
JS_FOR_EACH_PUBLIC_TAGGED_GC_POINTER_TYPE(INSTANTIATE_INTERNAL_TRACE_FUNCTIONS)

#undef INSTANTIATE_INTERNAL_TRACE_FUNCTIONS_FROM_TRACEKIND
#undef INSTANTIATE_INTERNAL_TRACE_FUNCTIONS

}  // namespace gc
}  // namespace js

// In debug builds, makes a note of the current compartment before calling a
// trace hook or traceChildren() method on a GC thing.
class MOZ_RAII AutoSetTracingSource {
#ifdef DEBUG
  GCMarker* marker = nullptr;
#endif

 public:
  template <typename T>
  AutoSetTracingSource(JSTracer* trc, T* thing) {
#ifdef DEBUG
    if (trc->isMarkingTracer() && thing) {
      marker = GCMarker::fromTracer(trc);
      MOZ_ASSERT(!marker->tracingZone);
      marker->tracingZone = thing->asTenured().zone();
      MOZ_ASSERT(!marker->tracingCompartment);
      marker->tracingCompartment = thing->maybeCompartment();
    }
#endif
  }

  ~AutoSetTracingSource() {
#ifdef DEBUG
    if (marker) {
      marker->tracingZone = nullptr;
      marker->tracingCompartment = nullptr;
    }
#endif
  }
};

// In debug builds, clear the trace hook compartment. This happens
// after the trace hook has called back into one of our trace APIs and we've
// checked the traced thing.
class MOZ_RAII AutoClearTracingSource {
#ifdef DEBUG
  GCMarker* marker = nullptr;
  JS::Zone* prevZone = nullptr;
  Compartment* prevCompartment = nullptr;
#endif

 public:
  explicit AutoClearTracingSource(JSTracer* trc) {
#ifdef DEBUG
    if (trc->isMarkingTracer()) {
      marker = GCMarker::fromTracer(trc);
      prevZone = marker->tracingZone;
      marker->tracingZone = nullptr;
      prevCompartment = marker->tracingCompartment;
      marker->tracingCompartment = nullptr;
    }
#endif
  }

  ~AutoClearTracingSource() {
#ifdef DEBUG
    if (marker) {
      marker->tracingZone = prevZone;
      marker->tracingCompartment = prevCompartment;
    }
#endif
  }
};

template <typename T>
void js::TraceManuallyBarrieredCrossCompartmentEdge(JSTracer* trc,
                                                    JSObject* src, T* dst,
                                                    const char* name) {
  // Clear expected compartment for cross-compartment edge.
  AutoClearTracingSource acts(trc);

  if (ShouldTraceCrossCompartment(trc, src, *dst)) {
    TraceEdgeInternal(trc, dst, name);
  }
}
template void js::TraceManuallyBarrieredCrossCompartmentEdge<Value>(
    JSTracer*, JSObject*, Value*, const char*);
template void js::TraceManuallyBarrieredCrossCompartmentEdge<JSObject*>(
    JSTracer*, JSObject*, JSObject**, const char*);
template void js::TraceManuallyBarrieredCrossCompartmentEdge<BaseScript*>(
    JSTracer*, JSObject*, BaseScript**, const char*);

class MOZ_RAII AutoDisableCompartmentCheckTracer {
#ifdef DEBUG
  JSContext* cx_;
  bool prev_;

 public:
  AutoDisableCompartmentCheckTracer()
      : cx_(TlsContext.get()), prev_(cx_->disableCompartmentCheckTracer) {
    cx_->disableCompartmentCheckTracer = true;
  }
  ~AutoDisableCompartmentCheckTracer() {
    cx_->disableCompartmentCheckTracer = prev_;
  }
#else
 public:
  AutoDisableCompartmentCheckTracer(){};
#endif
};

template <typename T>
void js::TraceSameZoneCrossCompartmentEdge(JSTracer* trc,
                                           const WriteBarriered<T>* dst,
                                           const char* name) {
#ifdef DEBUG
  if (trc->isMarkingTracer()) {
    MOZ_ASSERT((*dst)->maybeCompartment(),
               "Use TraceEdge for GC things without a compartment");

    GCMarker* gcMarker = GCMarker::fromTracer(trc);
    MOZ_ASSERT_IF(gcMarker->tracingZone,
                  (*dst)->zone() == gcMarker->tracingZone);
  }
#endif

  // Clear expected compartment for cross-compartment edge.
  AutoClearTracingSource acts(trc);
  AutoDisableCompartmentCheckTracer adcct;
  TraceEdgeInternal(trc, ConvertToBase(dst->unbarrieredAddress()), name);
}
template void js::TraceSameZoneCrossCompartmentEdge(
    JSTracer*, const WriteBarriered<Shape*>*, const char*);

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

template void js::TraceWeakMapKeyEdgeInternal<JSObject>(JSTracer*, Zone*,
                                                        JSObject**,
                                                        const char*);
template void js::TraceWeakMapKeyEdgeInternal<BaseScript>(JSTracer*, Zone*,
                                                          BaseScript**,
                                                          const char*);

template <typename T>
void js::TraceProcessGlobalRoot(JSTracer* trc, T* thing, const char* name) {
  AssertRootMarkingPhase(trc);
  MOZ_ASSERT(thing->isPermanentAndMayBeShared());

  // We have to mark permanent atoms and well-known symbols through a special
  // method because the default DoMarking implementation automatically skips
  // them. Fortunately, atoms (permanent and non) cannot refer to other GC
  // things so they do not need to go through the mark stack and may simply
  // be marked directly.  Moreover, well-known symbols can refer only to
  // permanent atoms, so likewise require no subsequent marking.
  if (trc->isMarkingTracer()) {
    CheckTracedThing(trc, *ConvertToBase(&thing));
    AutoClearTracingSource acts(trc);
    thing->asTenured().markIfUnmarked(gc::MarkColor::Black);
    return;
  }

  T* tmp = thing;
  TraceEdgeInternal(trc, ConvertToBase(&tmp), name);
  MOZ_ASSERT(tmp == thing);  // We shouldn't move process global roots.
}
template void js::TraceProcessGlobalRoot<JSAtom>(JSTracer*, JSAtom*,
                                                 const char*);
template void js::TraceProcessGlobalRoot<JS::Symbol>(JSTracer*, JS::Symbol*,
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

  auto traced = MapGCThingTyped(thing, thing->getTraceKind(),
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

template <typename T>
inline bool DoCallback(GenericTracer* trc, T** thingp, const char* name) {
  CheckTracedThing(trc, *thingp);
  JS::AutoTracingName ctx(trc, name);

  T* thing = *thingp;
  T* post = DispatchToOnEdge(trc, thing);
  if (post != thing) {
    *thingp = post;
  }

  return post;
}

template <typename T>
inline bool DoCallback(GenericTracer* trc, T* thingp, const char* name) {
  JS::AutoTracingName ctx(trc, name);

  // Return true by default. For some types the lambda below won't be called.
  bool ret = true;
  auto thing = MapGCThingTyped(*thingp, [trc, &ret](auto thing) {
    CheckTracedThing(trc, thing);

    auto* post = DispatchToOnEdge(trc, thing);
    if (!post) {
      ret = false;
      return TaggedPtr<T>::empty();
    }

    return TaggedPtr<T>::wrap(post);
  });

  // Only update *thingp if the value changed, to avoid TSan false positives for
  // template objects when using DumpHeapTracer or UbiNode tracers while Ion
  // compiling off-thread.
  if (thing.isSome() && thing.value() != *thingp) {
    *thingp = thing.value();
  }

  return ret;
}

// This method is responsible for dynamic dispatch to the real tracer
// implementation. Consider replacing this choke point with virtual dispatch:
// a sufficiently smart C++ compiler may be able to devirtualize some paths.
template <typename T>
bool js::gc::TraceEdgeInternal(JSTracer* trc, T* thingp, const char* name) {
#define IS_SAME_TYPE_OR(name, type, _, _1) std::is_same_v<type*, T> ||
  static_assert(JS_FOR_EACH_TRACEKIND(IS_SAME_TYPE_OR)
                        std::is_same_v<T, JS::Value> ||
                    std::is_same_v<T, jsid> || std::is_same_v<T, TaggedProto>,
                "Only the base cell layout types are allowed into "
                "marking/tracing internals");
#undef IS_SAME_TYPE_OR

  if (trc->isMarkingTracer()) {
    DoMarking(GCMarker::fromTracer(trc), *thingp);
    return true;
  }

  MOZ_ASSERT(trc->isGenericTracer());
  return DoCallback(trc->asGenericTracer(), thingp, name);
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

using HasNoImplicitEdgesType = bool;

template <typename T>
struct ImplicitEdgeHolderType {
  using Type = HasNoImplicitEdgesType;
};

// For now, we only handle JSObject* and BaseScript* keys, but the linear time
// algorithm can be easily extended by adding in more types here, then making
// GCMarker::traverse<T> call markImplicitEdges.
template <>
struct ImplicitEdgeHolderType<JSObject*> {
  using Type = JSObject*;
};

template <>
struct ImplicitEdgeHolderType<BaseScript*> {
  using Type = BaseScript*;
};

void GCMarker::markEphemeronEdges(EphemeronEdgeVector& edges) {
  DebugOnly<size_t> initialLength = edges.length();

  for (auto& edge : edges) {
    gc::AutoSetMarkColor autoColor(*this,
                                   std::min(edge.color, CellColor(color)));
    ApplyGCThingTyped(edge.target, edge.target->getTraceKind(),
                      [this](auto t) { markAndTraverse(t); });
  }

  // The above marking always goes through markAndPush, which will not cause
  // 'edges' to be appended to while iterating.
  MOZ_ASSERT(edges.length() == initialLength);
}

// 'delegate' is no longer the delegate of 'key'.
void GCMarker::severWeakDelegate(JSObject* key, JSObject* delegate) {
  JS::Zone* zone = delegate->zone();
  if (!zone->needsIncrementalBarrier()) {
    MOZ_ASSERT(
        !zone->gcEphemeronEdges(delegate).get(delegate),
        "non-collecting zone should not have populated gcEphemeronEdges");
    return;
  }
  auto* p = zone->gcEphemeronEdges(delegate).get(delegate);
  if (!p) {
    return;
  }

  // We are losing 3 edges here: key -> delegate, delegate -> key, and
  // <delegate, map> -> value. Maintain snapshot-at-beginning (hereafter,
  // S-A-B) by conservatively assuming the delegate will end up black and
  // marking through the latter 2 edges.
  //
  // Note that this does not fully give S-A-B:
  //
  //  1. If the map is gray, then the value will only be marked gray here even
  //  though the map could later be discovered to be black.
  //
  //  2. If the map has not yet been marked, we won't have any entries to mark
  //  here in the first place.
  //
  //  3. We're not marking the delegate, since that would cause eg nukeAllCCWs
  //  to keep everything alive for another collection.
  //
  // We can't even assume that the delegate passed in here is live, because we
  // could have gotten here from nukeAllCCWs, which iterates over all CCWs
  // including dead ones.
  //
  // This is ok because S-A-B is only needed to prevent the case where an
  // unmarked object is removed from the graph and then re-inserted where it is
  // reachable only by things that have already been marked. None of the 3
  // target objects will be re-inserted anywhere as a result of this action.

  EphemeronEdgeVector& edges = p->value;
  gc::AutoSetMarkColor autoColor(*this, MarkColor::Black);
  markEphemeronEdges(edges);
}

// 'delegate' is now the delegate of 'key'. Update weakmap marking state.
void GCMarker::restoreWeakDelegate(JSObject* key, JSObject* delegate) {
  if (!key->zone()->needsIncrementalBarrier() ||
      !delegate->zone()->needsIncrementalBarrier()) {
    MOZ_ASSERT(
        !key->zone()->gcEphemeronEdges(key).has(key),
        "non-collecting zone should not have populated gcEphemeronEdges");
    return;
  }
  auto* p = key->zone()->gcEphemeronEdges(key).get(key);
  if (!p) {
    return;
  }

  // Similar to severWeakDelegate above, mark through the key -> value edge.
  EphemeronEdgeVector& edges = p->value;
  gc::AutoSetMarkColor autoColor(*this, MarkColor::Black);
  markEphemeronEdges(edges);
}

template <typename T>
void GCMarker::markImplicitEdgesHelper(T markedThing) {
  if (!isWeakMarking()) {
    return;
  }

  Zone* zone = markedThing->asTenured().zone();
  MOZ_ASSERT(zone->isGCMarking());
  MOZ_ASSERT(!zone->isGCSweeping());

  auto p = zone->gcEphemeronEdges().get(markedThing);
  if (!p) {
    return;
  }
  EphemeronEdgeVector& edges = p->value;

  // markedThing might be a key in a debugger weakmap, which can end up marking
  // values that are in a different compartment.
  AutoClearTracingSource acts(this);

  CellColor thingColor = gc::detail::GetEffectiveColor(runtime(), markedThing);
  gc::AutoSetMarkColor autoColor(*this, thingColor);
  markEphemeronEdges(edges);
}

template <>
void GCMarker::markImplicitEdgesHelper(HasNoImplicitEdgesType) {}

template <typename T>
void GCMarker::markImplicitEdges(T* thing) {
  markImplicitEdgesHelper<typename ImplicitEdgeHolderType<T*>::Type>(thing);
}

template void GCMarker::markImplicitEdges(JSObject*);
template void GCMarker::markImplicitEdges(BaseScript*);

}  // namespace js

template <typename T>
static inline bool ShouldMark(GCMarker* gcmarker, T* thing) {
  // Don't trace things that are owned by another runtime.
  if (IsOwnedByOtherRuntime(gcmarker->runtime(), thing)) {
    return false;
  }

  // We may encounter nursery things during normal marking since we don't
  // collect the nursery at the start of every GC slice.
  if (!thing->isTenured()) {
    return false;
  }

  // Don't mark things outside a zone if we are in a per-zone GC.
  return thing->asTenured().zone()->shouldMarkInZone();
}

template <typename T>
void DoMarking(GCMarker* gcmarker, T* thing) {
  // Do per-type marking precondition checks.
  if (!ShouldMark(gcmarker, thing)) {
    MOZ_ASSERT(gc::detail::GetEffectiveColor(gcmarker->runtime(), thing) ==
               js::gc::CellColor::Black);
    return;
  }

  CheckTracedThing(gcmarker, thing);
  AutoClearTracingSource acts(gcmarker);
  gcmarker->markAndTraverse(thing);

  // Mark the compartment as live.
  SetMaybeAliveFlag(thing);
}

template <typename T>
void DoMarking(GCMarker* gcmarker, const T& thing) {
  ApplyGCThingTyped(thing, [gcmarker](auto t) { DoMarking(gcmarker, t); });
}

JS_PUBLIC_API void js::gc::PerformIncrementalReadBarrier(JS::GCCellPtr thing) {
  // Optimized marking for read barriers. This is called from
  // ExposeGCThingToActiveJS which has already checked the prerequisites for
  // performing a read barrier. This means we can skip a bunch of checks and
  // call info the tracer directly.

  MOZ_ASSERT(thing);
  MOZ_ASSERT(!JS::RuntimeHeapIsMajorCollecting());

  TenuredCell* cell = &thing.asCell()->asTenured();
  Zone* zone = cell->zone();
  MOZ_ASSERT(zone->needsIncrementalBarrier());

  // Skip dispatching on known tracer type.
  BarrierTracer* trc = BarrierTracer::fromTracer(zone->barrierTracer());
  trc->performBarrier(thing);
}

void js::gc::PerformIncrementalBarrier(TenuredCell* cell) {
  // Internal version of previous function called for both read and pre-write
  // barriers.

  MOZ_ASSERT(cell);
  MOZ_ASSERT(!JS::RuntimeHeapIsMajorCollecting());

  Zone* zone = cell->zone();
  MOZ_ASSERT(zone->needsIncrementalBarrier());

  // Skip disptaching on known tracer type.
  BarrierTracer* trc = BarrierTracer::fromTracer(zone->barrierTracer());

  trc->performBarrier(JS::GCCellPtr(cell, cell->getTraceKind()));
}

void js::gc::PerformIncrementalBarrierDuringFlattening(JSString* str) {
  TenuredCell* cell = &str->asTenured();

  // Skip recording ropes. Buffering them is problematic because they will have
  // their flags temporarily overwritten during flattening. Fortunately their
  // children will also be barriered by flattening process so we don't need to
  // traverse them.
  if (str->isRope()) {
    cell->markBlack();
    return;
  }

  PerformIncrementalBarrier(cell);
}

template <typename T>
void js::GCMarker::markAndTraverse(T* thing) {
  if (thing->isPermanentAndMayBeShared()) {
    return;
  }

  if (mark(thing)) {
    traverse(thing);
  }
}

// The simplest traversal calls out to the fully generic traceChildren function
// to visit the child edges. In the absence of other traversal mechanisms, this
// function will rapidly grow the stack past its bounds and crash the process.
// Thus, this generic tracing should only be used in cases where subsequent
// tracing will not recurse.
template <typename T>
void js::GCMarker::traceChildren(T* thing) {
  MOZ_ASSERT(!thing->isPermanentAndMayBeShared());
  MOZ_ASSERT(thing->isMarkedAny());
  AutoSetTracingSource asts(this, thing);
  thing->traceChildren(this);
}
namespace js {
template <>
void GCMarker::traverse(BaseShape* thing) {
  traceChildren(thing);
}
template <>
void GCMarker::traverse(GetterSetter* thing) {
  traceChildren(thing);
}
template <>
void GCMarker::traverse(JS::Symbol* thing) {
  traceChildren(thing);
}
template <>
void GCMarker::traverse(JS::BigInt* thing) {
  traceChildren(thing);
}
template <>
void GCMarker::traverse(RegExpShared* thing) {
  traceChildren(thing);
}
}  // namespace js

// Strings, Shapes, and Scopes are extremely common, but have simple patterns of
// recursion. We traverse trees of these edges immediately, with aggressive,
// manual inlining, implemented by eagerlyTraceChildren.
template <typename T>
void js::GCMarker::scanChildren(T* thing) {
  MOZ_ASSERT(!thing->isPermanentAndMayBeShared());
  MOZ_ASSERT(thing->isMarkedAny());
  eagerlyMarkChildren(thing);
}
namespace js {
template <>
void GCMarker::traverse(JSString* thing) {
  scanChildren(thing);
}
template <>
void GCMarker::traverse(Shape* thing) {
  scanChildren(thing);
}
template <>
void GCMarker::traverse(PropMap* thing) {
  scanChildren(thing);
}
template <>
void GCMarker::traverse(js::Scope* thing) {
  scanChildren(thing);
}
}  // namespace js

// Object and ObjectGroup are extremely common and can contain arbitrarily
// nested graphs, so are not trivially inlined. In this case we use a mark
// stack to control recursion. JitCode shares none of these properties, but is
// included for historical reasons. JSScript normally cannot recurse, but may
// be used as a weakmap key and thereby recurse into weakmapped values.
template <typename T>
void js::GCMarker::pushThing(T* thing) {
  MOZ_ASSERT(!thing->isPermanentAndMayBeShared());
  MOZ_ASSERT(thing->isMarkedAny());
  pushTaggedPtr(thing);
}
namespace js {
template <>
void GCMarker::traverse(JSObject* thing) {
  pushThing(thing);
}
template <>
void GCMarker::traverse(jit::JitCode* thing) {
  pushThing(thing);
}
template <>
void GCMarker::traverse(BaseScript* thing) {
  pushThing(thing);
}
}  // namespace js

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

  if (target->isPermanentAndMayBeShared()) {
    MOZ_ASSERT(!target->maybeCompartment());

    // No further checks for parmanent/shared things.
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

template <typename S, typename T>
void js::GCMarker::markAndTraverseEdge(S source, T* target) {
  checkTraversedEdge(source, target);
  if (!target->isPermanentAndMayBeShared()) {
    markAndTraverse(target);
  }
}

template <typename S, typename T>
void js::GCMarker::markAndTraverseEdge(S source, const T& thing) {
  ApplyGCThingTyped(
      thing, [this, source](auto t) { this->markAndTraverseEdge(source, t); });
}

namespace {

template <typename T>
struct TraceKindCanBeGray {};
#define EXPAND_TRACEKIND_DEF(_, type, canBeGray, _1) \
  template <>                                        \
  struct TraceKindCanBeGray<type> {                  \
    static const bool value = canBeGray;             \
  };
JS_FOR_EACH_TRACEKIND(EXPAND_TRACEKIND_DEF)
#undef EXPAND_TRACEKIND_DEF

}  // namespace

struct TraceKindCanBeGrayFunctor {
  template <typename T>
  bool operator()() {
    return TraceKindCanBeGray<T>::value;
  }
};

static bool TraceKindCanBeMarkedGray(JS::TraceKind kind) {
  return DispatchTraceKindTyped(TraceKindCanBeGrayFunctor(), kind);
}

template <typename T>
bool js::GCMarker::mark(T* thing) {
  if (!thing->isTenured()) {
    return false;
  }

  AssertShouldMarkInZone(thing);
  TenuredCell* cell = &thing->asTenured();

  MarkColor color =
      TraceKindCanBeGray<T>::value ? markColor() : MarkColor::Black;
  bool marked = cell->markIfUnmarked(color);
  if (marked) {
    markCount++;
  }

  return marked;
}

/*** Inline, Eager GC Marking ***********************************************/

// Each of the eager, inline marking paths is directly preceeded by the
// out-of-line, generic tracing code for comparison. Both paths must end up
// traversing equivalent subgraphs.

void BaseScript::traceChildren(JSTracer* trc) {
  TraceEdge(trc, &functionOrGlobal_, "function");
  TraceEdge(trc, &sourceObject_, "sourceObject");

  warmUpData_.trace(trc);

  if (data_) {
    data_->trace(trc);
  }

  if (trc->isMarkingTracer()) {
    GCMarker::fromTracer(trc)->markImplicitEdges(this);
  }
}

void Shape::traceChildren(JSTracer* trc) {
  TraceCellHeaderEdge(trc, this, "base");
  TraceNullableEdge(trc, &propMap_, "propertymap");
}

inline void js::GCMarker::eagerlyMarkChildren(Shape* shape) {
  MOZ_ASSERT(shape->isMarked(markColor()));

  BaseShape* base = shape->base();
  checkTraversedEdge(shape, base);
  if (mark(base)) {
    base->traceChildren(this);
  }

  if (PropMap* map = shape->propMap()) {
    markAndTraverseEdge(shape, map);
  }
}

void JSString::traceChildren(JSTracer* trc) {
  if (hasBase()) {
    traceBase(trc);
  } else if (isRope()) {
    asRope().traceChildren(trc);
  }
}
inline void GCMarker::eagerlyMarkChildren(JSString* str) {
  if (str->isLinear()) {
    eagerlyMarkChildren(&str->asLinear());
  } else {
    eagerlyMarkChildren(&str->asRope());
  }
}

void JSString::traceBase(JSTracer* trc) {
  MOZ_ASSERT(hasBase());
  TraceManuallyBarrieredEdge(trc, &d.s.u3.base, "base");
}
inline void js::GCMarker::eagerlyMarkChildren(JSLinearString* linearStr) {
  AssertShouldMarkInZone(linearStr);
  MOZ_ASSERT(linearStr->isMarkedAny());
  MOZ_ASSERT(linearStr->JSString::isLinear());

  // Use iterative marking to avoid blowing out the stack.
  while (linearStr->hasBase()) {
    linearStr = linearStr->base();

    // It's possible to observe a rope as the base of a linear string if we
    // process barriers during rope flattening. See the assignment of base in
    // JSRope::flattenInternal's finish_node section.
    if (static_cast<JSString*>(linearStr)->isRope()) {
      MOZ_ASSERT(!JS::RuntimeHeapIsMajorCollecting());
      break;
    }

    MOZ_ASSERT(linearStr->JSString::isLinear());
    if (linearStr->isPermanentAtom()) {
      break;
    }
    AssertShouldMarkInZone(linearStr);
    if (!mark(static_cast<JSString*>(linearStr))) {
      break;
    }
  }
}

void JSRope::traceChildren(JSTracer* trc) {
  js::TraceManuallyBarrieredEdge(trc, &d.s.u2.left, "left child");
  js::TraceManuallyBarrieredEdge(trc, &d.s.u3.right, "right child");
}
inline void js::GCMarker::eagerlyMarkChildren(JSRope* rope) {
  // This function tries to scan the whole rope tree using the marking stack
  // as temporary storage. If that becomes full, the unscanned ropes are
  // added to the delayed marking list. When the function returns, the
  // marking stack is at the same depth as it was on entry. This way we avoid
  // using tags when pushing ropes to the stack as ropes never leak to other
  // users of the stack. This also assumes that a rope can only point to
  // other ropes or linear strings, it cannot refer to GC things of other
  // types.
  gc::MarkStack& stack = currentStack();
  size_t savedPos = stack.position();
  MOZ_DIAGNOSTIC_ASSERT(rope->getTraceKind() == JS::TraceKind::String);
  while (true) {
    MOZ_DIAGNOSTIC_ASSERT(rope->getTraceKind() == JS::TraceKind::String);
    MOZ_DIAGNOSTIC_ASSERT(rope->JSString::isRope());
    AssertShouldMarkInZone(rope);
    MOZ_ASSERT(rope->isMarkedAny());
    JSRope* next = nullptr;

    JSString* right = rope->rightChild();
    if (!right->isPermanentAtom() && mark(right)) {
      if (right->isLinear()) {
        eagerlyMarkChildren(&right->asLinear());
      } else {
        next = &right->asRope();
      }
    }

    JSString* left = rope->leftChild();
    if (!left->isPermanentAtom() && mark(left)) {
      if (left->isLinear()) {
        eagerlyMarkChildren(&left->asLinear());
      } else {
        // When both children are ropes, set aside the right one to
        // scan it later.
        if (next && !stack.pushTempRope(next)) {
          delayMarkingChildren(next);
        }
        next = &left->asRope();
      }
    }
    if (next) {
      rope = next;
    } else if (savedPos != stack.position()) {
      MOZ_ASSERT(savedPos < stack.position());
      rope = stack.popPtr().asTempRope();
    } else {
      break;
    }
  }
  MOZ_ASSERT(savedPos == stack.position());
}

static inline void TraceBindingNames(JSTracer* trc, BindingName* names,
                                     uint32_t length) {
  for (uint32_t i = 0; i < length; i++) {
    JSAtom* name = names[i].name();
    MOZ_ASSERT(name);
    TraceManuallyBarrieredEdge(trc, &name, "scope name");
  }
};
static inline void TraceNullableBindingNames(JSTracer* trc, BindingName* names,
                                             uint32_t length) {
  for (uint32_t i = 0; i < length; i++) {
    if (JSAtom* name = names[i].name()) {
      TraceManuallyBarrieredEdge(trc, &name, "scope name");
    }
  }
};
void AbstractBindingName<JSAtom>::trace(JSTracer* trc) {
  if (JSAtom* atom = name()) {
    TraceManuallyBarrieredEdge(trc, &atom, "binding name");
  }
}
void BindingIter::trace(JSTracer* trc) {
  TraceNullableBindingNames(trc, names_, length_);
}

template <typename SlotInfo>
void RuntimeScopeData<SlotInfo>::trace(JSTracer* trc) {
  TraceBindingNames(trc, GetScopeDataTrailingNamesPointer(this), length);
}
template void RuntimeScopeData<LexicalScope::SlotInfo>::trace(JSTracer* trc);
template void RuntimeScopeData<ClassBodyScope::SlotInfo>::trace(JSTracer* trc);
template void RuntimeScopeData<VarScope::SlotInfo>::trace(JSTracer* trc);
template void RuntimeScopeData<GlobalScope::SlotInfo>::trace(JSTracer* trc);
template void RuntimeScopeData<EvalScope::SlotInfo>::trace(JSTracer* trc);
template void RuntimeScopeData<WasmFunctionScope::SlotInfo>::trace(
    JSTracer* trc);

void FunctionScope::RuntimeData::trace(JSTracer* trc) {
  TraceNullableEdge(trc, &canonicalFunction, "scope canonical function");
  TraceNullableBindingNames(trc, GetScopeDataTrailingNamesPointer(this),
                            length);
}
void ModuleScope::RuntimeData::trace(JSTracer* trc) {
  TraceNullableEdge(trc, &module, "scope module");
  TraceBindingNames(trc, GetScopeDataTrailingNamesPointer(this), length);
}
void WasmInstanceScope::RuntimeData::trace(JSTracer* trc) {
  TraceNullableEdge(trc, &instance, "wasm instance");
  TraceBindingNames(trc, GetScopeDataTrailingNamesPointer(this), length);
}

void Scope::traceChildren(JSTracer* trc) {
  TraceNullableEdge(trc, &environmentShape_, "scope env shape");
  TraceNullableEdge(trc, &enclosingScope_, "scope enclosing");
  applyScopeDataTyped([trc](auto data) { data->trace(trc); });
}

inline void js::GCMarker::eagerlyMarkChildren(Scope* scope) {
  do {
    if (scope->environmentShape()) {
      markAndTraverseEdge(scope, scope->environmentShape());
    }
    mozilla::Span<AbstractBindingName<JSAtom>> names;
    switch (scope->kind()) {
      case ScopeKind::Function: {
        FunctionScope::RuntimeData& data = scope->as<FunctionScope>().data();
        if (data.canonicalFunction) {
          markAndTraverseObjectEdge(scope, data.canonicalFunction);
        }
        names = GetScopeDataTrailingNames(&data);
        break;
      }

      case ScopeKind::FunctionBodyVar: {
        VarScope::RuntimeData& data = scope->as<VarScope>().data();
        names = GetScopeDataTrailingNames(&data);
        break;
      }

      case ScopeKind::Lexical:
      case ScopeKind::SimpleCatch:
      case ScopeKind::Catch:
      case ScopeKind::NamedLambda:
      case ScopeKind::StrictNamedLambda:
      case ScopeKind::FunctionLexical: {
        LexicalScope::RuntimeData& data = scope->as<LexicalScope>().data();
        names = GetScopeDataTrailingNames(&data);
        break;
      }

      case ScopeKind::ClassBody: {
        ClassBodyScope::RuntimeData& data = scope->as<ClassBodyScope>().data();
        names = GetScopeDataTrailingNames(&data);
        break;
      }

      case ScopeKind::Global:
      case ScopeKind::NonSyntactic: {
        GlobalScope::RuntimeData& data = scope->as<GlobalScope>().data();
        names = GetScopeDataTrailingNames(&data);
        break;
      }

      case ScopeKind::Eval:
      case ScopeKind::StrictEval: {
        EvalScope::RuntimeData& data = scope->as<EvalScope>().data();
        names = GetScopeDataTrailingNames(&data);
        break;
      }

      case ScopeKind::Module: {
        ModuleScope::RuntimeData& data = scope->as<ModuleScope>().data();
        if (data.module) {
          markAndTraverseObjectEdge(scope, data.module);
        }
        names = GetScopeDataTrailingNames(&data);
        break;
      }

      case ScopeKind::With:
        break;

      case ScopeKind::WasmInstance: {
        WasmInstanceScope::RuntimeData& data =
            scope->as<WasmInstanceScope>().data();
        markAndTraverseObjectEdge(scope, data.instance);
        names = GetScopeDataTrailingNames(&data);
        break;
      }

      case ScopeKind::WasmFunction: {
        WasmFunctionScope::RuntimeData& data =
            scope->as<WasmFunctionScope>().data();
        names = GetScopeDataTrailingNames(&data);
        break;
      }
    }
    if (scope->kind_ == ScopeKind::Function) {
      for (auto& binding : names) {
        if (JSAtom* name = binding.name()) {
          markAndTraverseStringEdge(scope, name);
        }
      }
    } else {
      for (auto& binding : names) {
        markAndTraverseStringEdge(scope, binding.name());
      }
    }
    scope = scope->enclosing();
  } while (scope && mark(scope));
}

void BaseShape::traceChildren(JSTracer* trc) {
  // Note: the realm's global can be nullptr if we GC while creating the global.
  if (JSObject* global = realm()->unsafeUnbarrieredMaybeGlobal()) {
    TraceManuallyBarrieredEdge(trc, &global, "baseshape_global");
  }

  if (proto_.isObject()) {
    TraceEdge(trc, &proto_, "baseshape_proto");
  }
}

void GetterSetter::traceChildren(JSTracer* trc) {
  if (getter()) {
    TraceCellHeaderEdge(trc, this, "gettersetter_getter");
  }
  if (setter()) {
    TraceEdge(trc, &setter_, "gettersetter_setter");
  }
}

void PropMap::traceChildren(JSTracer* trc) {
  if (hasPrevious()) {
    TraceEdge(trc, &asLinked()->data_.previous, "propmap_previous");
  }

  if (isShared()) {
    SharedPropMap::TreeData& treeData = asShared()->treeDataRef();
    if (SharedPropMap* parent = treeData.parent.maybeMap()) {
      TraceManuallyBarrieredEdge(trc, &parent, "propmap_parent");
      if (parent != treeData.parent.map()) {
        treeData.setParent(parent, treeData.parent.index());
      }
    }
  }

  for (uint32_t i = 0; i < PropMap::Capacity; i++) {
    if (hasKey(i)) {
      TraceEdge(trc, &keys_[i], "propmap_key");
    }
  }

  if (canHaveTable() && asLinked()->hasTable()) {
    asLinked()->data_.table->trace(trc);
  }
}

void js::GCMarker::eagerlyMarkChildren(PropMap* map) {
  MOZ_ASSERT(map->isMarkedAny());
  do {
    for (uint32_t i = 0; i < PropMap::Capacity; i++) {
      if (map->hasKey(i)) {
        markAndTraverseEdge(map, map->getKey(i));
      }
    }

    if (map->canHaveTable()) {
      // Special case: if a map has a table then all its pointers must point to
      // this map or an ancestor. Since these pointers will be traced by this
      // loop they do not need to be traced here as well.
      MOZ_ASSERT(map->asLinked()->canSkipMarkingTable());
    }

    if (map->isDictionary()) {
      map = map->asDictionary()->previous();
    } else {
      // For shared maps follow the |parent| link and not the |previous| link.
      // They're different when a map had a branch that wasn't at the end of the
      // map, but in this case they must have the same |previous| map. This is
      // asserted in SharedPropMap::addChild. In other words, marking all
      // |parent| maps will also mark all |previous| maps.
      map = map->asShared()->treeDataRef().parent.maybeMap();
    }
  } while (map && mark(map));
}

void JS::BigInt::traceChildren(JSTracer* trc) {}

// Call the trace hook set on the object, if present.
static inline void CallTraceHook(JSTracer* trc, JSObject* obj) {
  const JSClass* clasp = obj->getClass();
  MOZ_ASSERT(clasp);

  if (clasp->hasTrace()) {
    AutoSetTracingSource asts(trc, obj);
    clasp->doTrace(trc, obj);
  }
}

/*** Mark-stack Marking *****************************************************/

GCMarker::MarkQueueProgress GCMarker::processMarkQueue() {
#ifdef DEBUG
  if (markQueue.empty()) {
    return QueueComplete;
  }

  GCRuntime& gcrt = runtime()->gc;
  if (queueMarkColor == mozilla::Some(MarkColor::Gray) &&
      gcrt.state() != State::Sweep) {
    return QueueSuspended;
  }

  // If the queue wants to be gray marking, but we've pushed a black object
  // since set-color-gray was processed, then we can't switch to gray and must
  // again wait until gray marking is possible.
  //
  // Remove this code if the restriction against marking gray during black is
  // relaxed.
  if (queueMarkColor == mozilla::Some(MarkColor::Gray) && hasBlackEntries()) {
    return QueueSuspended;
  }

  // If the queue wants to be marking a particular color, switch to that color.
  // In any case, restore the mark color to whatever it was when we entered
  // this function.
  AutoSetMarkColor autoRevertColor(*this, queueMarkColor.valueOr(markColor()));

  // Process the mark queue by taking each object in turn, pushing it onto the
  // mark stack, and processing just the top element with processMarkStackTop
  // without recursing into reachable objects.
  while (queuePos < markQueue.length()) {
    Value val = markQueue[queuePos++].get().unbarrieredGet();
    if (val.isObject()) {
      JSObject* obj = &val.toObject();
      JS::Zone* zone = obj->zone();
      if (!zone->isGCMarking() || obj->isMarkedAtLeast(markColor())) {
        continue;
      }

      // If we have started sweeping, obey sweep group ordering. But note that
      // we will first be called during the initial sweep slice, when the sweep
      // group indexes have not yet been computed. In that case, we can mark
      // freely.
      if (gcrt.state() == State::Sweep && gcrt.initialState != State::Sweep) {
        if (zone->gcSweepGroupIndex < gcrt.getCurrentSweepGroupIndex()) {
          // Too late. This must have been added after we started collecting,
          // and we've already processed its sweep group. Skip it.
          continue;
        }
        if (zone->gcSweepGroupIndex > gcrt.getCurrentSweepGroupIndex()) {
          // Not ready yet. Wait until we reach the object's sweep group.
          queuePos--;
          return QueueSuspended;
        }
      }

      if (markColor() == MarkColor::Gray && zone->isGCMarkingBlackOnly()) {
        // Have not yet reached the point where we can mark this object, so
        // continue with the GC.
        queuePos--;
        return QueueSuspended;
      }

      // Mark the object and push it onto the stack.
      markAndTraverse(obj);

      if (isMarkStackEmpty()) {
        if (obj->asTenured().arena()->onDelayedMarkingList()) {
          AutoEnterOOMUnsafeRegion oomUnsafe;
          oomUnsafe.crash("mark queue OOM");
        }
      }

      // Process just the one object that is now on top of the mark stack,
      // possibly pushing more stuff onto the stack.
      if (isMarkStackEmpty()) {
        MOZ_ASSERT(obj->asTenured().arena()->onDelayedMarkingList());
        // If we overflow the stack here and delay marking, then we won't be
        // testing what we think we're testing.
        AutoEnterOOMUnsafeRegion oomUnsafe;
        oomUnsafe.crash("Overflowed stack while marking test queue");
      }

      SliceBudget unlimited = SliceBudget::unlimited();
      processMarkStackTop(unlimited);
    } else if (val.isString()) {
      JSLinearString* str = &val.toString()->asLinear();
      if (js::StringEqualsLiteral(str, "yield") && gcrt.isIncrementalGc()) {
        return QueueYielded;
      } else if (js::StringEqualsLiteral(str, "enter-weak-marking-mode") ||
                 js::StringEqualsLiteral(str, "abort-weak-marking-mode")) {
        if (state == MarkingState::RegularMarking) {
          // We can't enter weak marking mode at just any time, so instead
          // we'll stop processing the queue and continue on with the GC. Once
          // we enter weak marking mode, we can continue to the rest of the
          // queue. Note that we will also suspend for aborting, and then abort
          // the earliest following weak marking mode.
          queuePos--;
          return QueueSuspended;
        }
        if (js::StringEqualsLiteral(str, "abort-weak-marking-mode")) {
          abortLinearWeakMarking();
        }
      } else if (js::StringEqualsLiteral(str, "drain")) {
        auto unlimited = SliceBudget::unlimited();
        MOZ_RELEASE_ASSERT(
            markUntilBudgetExhausted(unlimited, DontReportMarkTime));
      } else if (js::StringEqualsLiteral(str, "set-color-gray")) {
        queueMarkColor = mozilla::Some(MarkColor::Gray);
        if (gcrt.state() != State::Sweep) {
          // Cannot mark gray yet, so continue with the GC.
          queuePos--;
          return QueueSuspended;
        }
        setMarkColor(MarkColor::Gray);
      } else if (js::StringEqualsLiteral(str, "set-color-black")) {
        queueMarkColor = mozilla::Some(MarkColor::Black);
        setMarkColor(MarkColor::Black);
      } else if (js::StringEqualsLiteral(str, "unset-color")) {
        queueMarkColor.reset();
      }
    }
  }
#endif

  return QueueComplete;
}

static gcstats::PhaseKind GrayMarkingPhaseForCurrentPhase(
    const gcstats::Statistics& stats) {
  using namespace gcstats;
  switch (stats.currentPhaseKind()) {
    case PhaseKind::SWEEP_MARK:
      return PhaseKind::SWEEP_MARK_GRAY;
    case PhaseKind::SWEEP_MARK_WEAK:
      return PhaseKind::SWEEP_MARK_GRAY_WEAK;
    default:
      MOZ_CRASH("Unexpected current phase");
  }
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

  // This method leaves the mark color as it found it.
  AutoSetMarkColor autoSetBlack(*this, MarkColor::Black);

  while (!isDrained()) {
    if (!traceBarrieredCells(budget)) {
      return false;
    }

    while (hasBlackEntries()) {
      MOZ_ASSERT(markColor() == MarkColor::Black);
      processMarkStackTop(budget);
      if (budget.isOverBudget()) {
        return false;
      }
    }

    if (hasGrayEntries()) {
      mozilla::Maybe<gcstats::AutoPhase> ap;
      if (reportTime) {
        auto& stats = runtime()->gc.stats();
        ap.emplace(stats, GrayMarkingPhaseForCurrentPhase(stats));
      }

      AutoSetMarkColor autoSetGray(*this, MarkColor::Gray);
      do {
        processMarkStackTop(budget);
        if (budget.isOverBudget()) {
          return false;
        }
      } while (hasGrayEntries());
    }

    // Do all normal marking before any delayed marking.
    //
    // We can end up marking black during gray marking in the following case: a
    // WeakMap has a CCW key whose delegate (target) is black, and during gray
    // marking we mark the map (gray). The delegate's color will be propagated
    // to the key. (And we can't avoid this by marking the key gray, because
    // even though the value will end up gray in either case, the WeakMap entry
    // must be preserved because the CCW could get collected and then we could
    // re-wrap the delegate and look it up in the map again, and need to get
    // back the original value.)
    MOZ_ASSERT(!hasGrayEntries());
    if (!barrierBuffer().empty() || hasBlackEntries()) {
      continue;
    }

    // Mark children of things that caused too deep recursion during the
    // above tracing.
    if (hasDelayedChildren() && !markAllDelayedChildren(budget, reportTime)) {
      return false;
    }
  }

  return true;
}

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

inline void GCMarker::processMarkStackTop(SliceBudget& budget) {
  /*
   * This function uses explicit goto and scans objects directly. This allows us
   * to eliminate tail recursion and significantly improve the marking
   * performance, see bug 641025.
   *
   * Note that the mutator can change the size and layout of objects between
   * marking slices, so we must check slots and element ranges read from the
   * stack.
   */

  JSObject* obj;             // The object being scanned.
  SlotsOrElementsKind kind;  // The kind of slot range being scanned, if any.
  HeapSlot* base;            // Slot range base pointer.
  size_t index;              // Index of the next slot to mark.
  size_t end;                // End of slot range to mark.

  gc::MarkStack& stack = currentStack();

  switch (stack.peekTag()) {
    case MarkStack::SlotsOrElementsRangeTag: {
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

          // Account for shifted elements.
          size_t numShifted = nobj->getElementsHeader()->numShiftedElements();
          size_t initlen = nobj->getDenseInitializedLength();
          index = std::max(index, numShifted) - numShifted;
          end = initlen;
          break;
        }
      }

      goto scan_value_range;
    }

    case MarkStack::ObjectTag: {
      obj = stack.popPtr().as<JSObject>();
      AssertShouldMarkInZone(obj);
      goto scan_obj;
    }

    case MarkStack::JitCodeTag: {
      auto code = stack.popPtr().as<jit::JitCode>();
      AutoSetTracingSource asts(this, code);
      return code->traceChildren(this);
    }

    case MarkStack::ScriptTag: {
      auto script = stack.popPtr().as<BaseScript>();
      AutoSetTracingSource asts(this, script);
      return script->traceChildren(this);
    }

    default:
      MOZ_CRASH("Invalid tag in mark stack");
  }
  return;

scan_value_range:
  while (index < end) {
    budget.step();
    if (budget.isOverBudget()) {
      pushValueRange(obj, kind, index, end);
      return;
    }

    const Value& v = base[index];
    index++;

    if (v.isString()) {
      markAndTraverseEdge(obj, v.toString());
    } else if (v.isObject()) {
      JSObject* obj2 = &v.toObject();
#ifdef DEBUG
      if (!obj2) {
        fprintf(stderr,
                "processMarkStackTop found ObjectValue(nullptr) "
                "at %zu Values from end of range in object:\n",
                size_t(end - (index - 1)));
        DumpObject(obj);
      }
#endif
      CheckForCompartmentMismatch(obj, obj2);
      if (mark(obj2)) {
        // Save the rest of this value range for later and start scanning obj2's
        // children.
        pushValueRange(obj, kind, index, end);
        obj = obj2;
        goto scan_obj;
      }
    } else if (v.isSymbol()) {
      markAndTraverseEdge(obj, v.toSymbol());
    } else if (v.isBigInt()) {
      markAndTraverseEdge(obj, v.toBigInt());
    } else if (v.isPrivateGCThing()) {
      // v.toGCCellPtr cannot be inlined, so construct one manually.
      Cell* cell = v.toGCThing();
      markAndTraverseEdge(obj, JS::GCCellPtr(cell, cell->getTraceKind()));
    }
  }
  return;

scan_obj : {
  AssertShouldMarkInZone(obj);

  budget.step();
  if (budget.isOverBudget()) {
    repush(obj);
    return;
  }

  markImplicitEdges(obj);
  markAndTraverseEdge(obj, obj->shape());

  CallTraceHook(this, obj);

  if (!obj->is<NativeObject>()) {
    return;
  }

  NativeObject* nobj = &obj->as<NativeObject>();

  unsigned nslots = nobj->slotSpan();

  do {
    if (nobj->hasEmptyElements()) {
      break;
    }

    base = nobj->getDenseElements();
    kind = SlotsOrElementsKind::Elements;
    index = 0;
    end = nobj->getDenseInitializedLength();

    if (!nslots) {
      goto scan_value_range;
    }
    pushValueRange(nobj, kind, index, end);
  } while (false);

  unsigned nfixed = nobj->numFixedSlots();

  base = nobj->fixedSlots();
  kind = SlotsOrElementsKind::FixedSlots;
  index = 0;

  if (nslots > nfixed) {
    pushValueRange(nobj, kind, index, nfixed);
    kind = SlotsOrElementsKind::DynamicSlots;
    base = nobj->slots_;
    end = nslots - nfixed;
    goto scan_value_range;
  }

  MOZ_ASSERT(nslots <= nobj->numFixedSlots());
  end = nslots;
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

static const size_t ValueRangeWords =
    sizeof(MarkStack::SlotsOrElementsRange) / sizeof(uintptr_t);

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

inline MarkStack::TaggedPtr MarkStack::SlotsOrElementsRange::ptr() const {
  return ptr_;
}

MarkStack::MarkStack(size_t maxCapacity)
    : topIndex_(0),
      maxCapacity_(maxCapacity)
#ifdef DEBUG
      ,
      iteratorCount_(0)
#endif
{
}

MarkStack::~MarkStack() {
  MOZ_ASSERT(isEmpty());
  MOZ_ASSERT(iteratorCount_ == 0);
}

bool MarkStack::init(StackType which, bool incrementalGCEnabled) {
  MOZ_ASSERT(isEmpty());
  return setStackCapacity(which, incrementalGCEnabled);
}

bool MarkStack::setStackCapacity(StackType which, bool incrementalGCEnabled) {
  size_t capacity;

  if (which == AuxiliaryStack) {
    capacity = SMALL_MARK_STACK_BASE_CAPACITY;
  } else if (incrementalGCEnabled) {
    capacity = INCREMENTAL_MARK_STACK_BASE_CAPACITY;
  } else {
    capacity = NON_INCREMENTAL_MARK_STACK_BASE_CAPACITY;
  }

  if (capacity > maxCapacity_) {
    capacity = maxCapacity_;
  }

  return resize(capacity);
}

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

inline MarkStack::TaggedPtr* MarkStack::topPtr() { return &stack()[topIndex_]; }

inline bool MarkStack::pushTaggedPtr(Tag tag, Cell* ptr) {
  if (!ensureSpace(1)) {
    return false;
  }

  *topPtr() = TaggedPtr(tag, ptr);
  topIndex_++;
  return true;
}

template <typename T>
inline bool MarkStack::push(T* ptr) {
  return pushTaggedPtr(MapTypeToMarkStackTag<T*>::value, ptr);
}

inline bool MarkStack::pushTempRope(JSRope* rope) {
  return pushTaggedPtr(TempRopeTag, rope);
}

inline bool MarkStack::push(JSObject* obj, SlotsOrElementsKind kind,
                            size_t start) {
  return push(SlotsOrElementsRange(kind, obj, start));
}

inline bool MarkStack::push(const SlotsOrElementsRange& array) {
  array.assertValid();

  if (!ensureSpace(ValueRangeWords)) {
    return false;
  }

  *reinterpret_cast<SlotsOrElementsRange*>(topPtr()) = array;
  topIndex_ += ValueRangeWords;
  MOZ_ASSERT(position() <= capacity());
  MOZ_ASSERT(TagIsRangeTag(peekTag()));
  return true;
}

inline const MarkStack::TaggedPtr& MarkStack::peekPtr() const {
  return stack()[topIndex_ - 1];
}

inline MarkStack::Tag MarkStack::peekTag() const { return peekPtr().tag(); }

inline MarkStack::TaggedPtr MarkStack::popPtr() {
  MOZ_ASSERT(!isEmpty());
  MOZ_ASSERT(!TagIsRangeTag(peekTag()));
  peekPtr().assertValid();
  topIndex_--;
  return *topPtr();
}

inline MarkStack::SlotsOrElementsRange MarkStack::popSlotsOrElementsRange() {
  MOZ_ASSERT(TagIsRangeTag(peekTag()));
  MOZ_ASSERT(position() >= ValueRangeWords);

  topIndex_ -= ValueRangeWords;
  const auto& array = *reinterpret_cast<SlotsOrElementsRange*>(topPtr());
  array.assertValid();
  return array;
}

inline bool MarkStack::ensureSpace(size_t count) {
  if (MOZ_LIKELY((topIndex_ + count) <= capacity())) {
    return !js::oom::ShouldFailWithOOM();
  }

  return enlarge(count);
}

MOZ_NEVER_INLINE bool MarkStack::enlarge(size_t count) {
  size_t newCapacity = std::min(maxCapacity_.ref(), capacity() * 2);
  if (newCapacity < capacity() + count) {
    return false;
  }

  return resize(newCapacity);
}

bool MarkStack::resize(size_t newCapacity) {
  MOZ_ASSERT(newCapacity != 0);
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
    : stack_(stack), pos_(stack.position()) {
#ifdef DEBUG
  stack.iteratorCount_++;
#endif
}

MarkStackIter::~MarkStackIter() {
#ifdef DEBUG
  MOZ_ASSERT(stack_.iteratorCount_);
  stack_.iteratorCount_--;
#endif
}

inline size_t MarkStackIter::position() const { return pos_; }

inline bool MarkStackIter::done() const { return position() == 0; }

inline MarkStack::TaggedPtr MarkStackIter::peekPtr() const {
  MOZ_ASSERT(!done());
  return stack_.stack()[pos_ - 1];
}

inline MarkStack::Tag MarkStackIter::peekTag() const { return peekPtr().tag(); }

inline void MarkStackIter::nextPtr() {
  MOZ_ASSERT(!done());
  MOZ_ASSERT(!TagIsRangeTag(peekTag()));
  pos_--;
}

inline void MarkStackIter::next() {
  if (TagIsRangeTag(peekTag())) {
    nextArray();
  } else {
    nextPtr();
  }
}

inline void MarkStackIter::nextArray() {
  MOZ_ASSERT(TagIsRangeTag(peekTag()));
  MOZ_ASSERT(position() >= ValueRangeWords);
  pos_ -= ValueRangeWords;
}

/*** GCMarker ***************************************************************/

/*
 * WeakMapTraceAction::Expand: the GC is recomputing the liveness of WeakMap
 * entries by expanding each live WeakMap into its constituent key->value edges,
 * a table of which will be consulted in a later phase whenever marking a
 * potential key.
 */
GCMarker::GCMarker(JSRuntime* rt)
    : JSTracer(rt, JS::TracerKind::Marking,
               JS::TraceOptions(JS::WeakMapTraceAction::Expand,
                                JS::WeakEdgeTraceAction::Skip)),
      stack(),
      auxStack(),
      mainStackColor(MarkColor::Black),
      delayedMarkingList(nullptr),
      delayedMarkingWorkAdded(false),
      state(MarkingState::NotActive),
      incrementalWeakMapMarkingEnabled(
          TuningDefaults::IncrementalWeakMapMarkingEnabled)
#ifdef DEBUG
      ,
      markLaterArenas(0),
      checkAtomMarking(true),
      strictCompartmentChecking(false),
      markQueue(rt),
      queuePos(0)
#endif
{
  setMarkColorUnchecked(MarkColor::Black);
}

bool GCMarker::init() {
  bool incrementalGCEnabled = runtime()->gc.isIncrementalGCEnabled();
  return stack.init(gc::MarkStack::MainStack, incrementalGCEnabled) &&
         auxStack.init(gc::MarkStack::AuxiliaryStack, incrementalGCEnabled);
}

bool GCMarker::isDrained() {
  return barrierBuffer().empty() && isMarkStackEmpty() && !delayedMarkingList;
}

void GCMarker::start() {
  MOZ_ASSERT(state == MarkingState::NotActive);
  state = MarkingState::RegularMarking;
  color = MarkColor::Black;

#ifdef DEBUG
  queuePos = 0;
  queueMarkColor.reset();
#endif

  MOZ_ASSERT(!delayedMarkingList);
  MOZ_ASSERT(markLaterArenas == 0);
}

void GCMarker::stop() {
  MOZ_ASSERT(isDrained());
  MOZ_ASSERT(!delayedMarkingList);
  MOZ_ASSERT(markLaterArenas == 0);

  if (state == MarkingState::NotActive) {
    return;
  }
  state = MarkingState::NotActive;

  barrierBuffer().clearAndFree();
  stack.clear();
  auxStack.clear();
  setMainStackColor(MarkColor::Black);
  AutoEnterOOMUnsafeRegion oomUnsafe;
  for (GCZonesIter zone(runtime()); !zone.done(); zone.next()) {
    if (!zone->gcEphemeronEdges().clear()) {
      oomUnsafe.crash("clearing weak keys in GCMarker::stop()");
    }
    if (!zone->gcNurseryEphemeronEdges().clear()) {
      oomUnsafe.crash("clearing (nursery) weak keys in GCMarker::stop()");
    }
  }
}

template <typename F>
inline void GCMarker::forEachDelayedMarkingArena(F&& f) {
  Arena* arena = delayedMarkingList;
  Arena* next;
  while (arena) {
    next = arena->getNextDelayedMarking();
    f(arena);
    arena = next;
  }
}

void GCMarker::reset() {
  color = MarkColor::Black;

  barrierBuffer().clearAndFree();
  stack.clear();
  auxStack.clear();
  setMainStackColor(MarkColor::Black);
  MOZ_ASSERT(isMarkStackEmpty());

  forEachDelayedMarkingArena([&](Arena* arena) {
    MOZ_ASSERT(arena->onDelayedMarkingList());
    arena->clearDelayedMarkingState();
#ifdef DEBUG
    MOZ_ASSERT(markLaterArenas);
    markLaterArenas--;
#endif
  });
  delayedMarkingList = nullptr;

  MOZ_ASSERT(isDrained());
  MOZ_ASSERT(!markLaterArenas);
}

void GCMarker::setMarkColor(gc::MarkColor newColor) {
  if (color != newColor) {
    MOZ_ASSERT(runtime()->gc.state() == State::Sweep);
    setMarkColorUnchecked(newColor);
  }
}

void GCMarker::setMarkColorUnchecked(gc::MarkColor newColor) {
  color = newColor;
  currentStackPtr = &getStack(color);
}

void GCMarker::setMainStackColor(gc::MarkColor newColor) {
  if (newColor != mainStackColor.ref()) {
    MOZ_ASSERT(isMarkStackEmpty());
    mainStackColor = newColor;
    setMarkColorUnchecked(color);
  }
}

template <typename T>
inline void GCMarker::pushTaggedPtr(T* ptr) {
  checkZone(ptr);
  if (!currentStack().push(ptr)) {
    delayMarkingChildrenOnOOM(ptr);
  }
}

void GCMarker::pushValueRange(JSObject* obj, SlotsOrElementsKind kind,
                              size_t start, size_t end) {
  checkZone(obj);
  MOZ_ASSERT(obj->is<NativeObject>());
  MOZ_ASSERT(start <= end);

  if (start == end) {
    return;
  }

  if (!currentStack().push(obj, kind, start)) {
    delayMarkingChildrenOnOOM(obj);
  }
}

void GCMarker::repush(JSObject* obj) {
  MOZ_ASSERT(obj->asTenured().isMarkedAtLeast(markColor()));
  pushTaggedPtr(obj);
}

bool GCMarker::enterWeakMarkingMode() {
  MOZ_ASSERT(weakMapAction() == JS::WeakMapTraceAction::Expand);
  MOZ_ASSERT(state != MarkingState::WeakMarking);
  if (state == MarkingState::IterativeMarking) {
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
  state = MarkingState::WeakMarking;

  // If there was an 'enter-weak-marking-mode' token in the queue, then it
  // and everything after it will still be in the queue so we can process
  // them now.
  while (processMarkQueue() == QueueYielded) {
  };

  return true;
}

IncrementalProgress JS::Zone::enterWeakMarkingMode(GCMarker* marker,
                                                   SliceBudget& budget) {
  MOZ_ASSERT(marker->isWeakMarking());

  if (!marker->incrementalWeakMapMarkingEnabled) {
    for (WeakMapBase* m : gcWeakMapList()) {
      if (m->mapColor) {
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

  // An OrderedHashMap::Range stays valid even when the underlying table
  // (zone->gcEphemeronEdges) is mutated, which is useful here since we may add
  // additional entries while iterating over the Range.
  gc::EphemeronEdgeTable::Range r = gcEphemeronEdges().all();
  while (!r.empty()) {
    gc::Cell* src = r.front().key;
    gc::CellColor srcColor =
        gc::detail::GetEffectiveColor(marker->runtime(), src);
    if (srcColor) {
      MOZ_ASSERT(src == r.front().key);
      auto& edges = r.front().value;
      r.popFront();  // Pop before any mutations happen.
      if (edges.length() > 0) {
        gc::AutoSetMarkColor autoColor(*marker, srcColor);
        marker->markEphemeronEdges(edges);
        budget.step(edges.length());
        if (budget.isOverBudget()) {
          return NotFinished;
        }
      }
    } else {
      r.popFront();
    }
  }

  return IncrementalProgress::Finished;
}

void GCMarker::leaveWeakMarkingMode() {
  MOZ_ASSERT(state == MarkingState::WeakMarking ||
             state == MarkingState::IterativeMarking);

  if (state != MarkingState::IterativeMarking) {
    state = MarkingState::RegularMarking;
  }

  // The gcEphemeronEdges table is still populated and may be used during a
  // future weak marking mode within this GC.
}

MOZ_NEVER_INLINE void GCMarker::delayMarkingChildrenOnOOM(Cell* cell) {
  delayMarkingChildren(cell);
}

void GCMarker::delayMarkingChildren(Cell* cell) {
  Arena* arena = cell->asTenured().arena();
  if (!arena->onDelayedMarkingList()) {
    arena->setNextDelayedMarkingArena(delayedMarkingList);
    delayedMarkingList = arena;
#ifdef DEBUG
    markLaterArenas++;
#endif
  }
  JS::TraceKind kind = MapAllocToTraceKind(arena->getAllocKind());
  MarkColor colorToMark =
      TraceKindCanBeMarkedGray(kind) ? color : MarkColor::Black;
  if (!arena->hasDelayedMarking(colorToMark)) {
    arena->setHasDelayedMarking(colorToMark, true);
    delayedMarkingWorkAdded = true;
  }
}

void GCMarker::markDelayedChildren(Arena* arena, MarkColor color) {
  JS::TraceKind kind = MapAllocToTraceKind(arena->getAllocKind());
  MOZ_ASSERT_IF(color == MarkColor::Gray, TraceKindCanBeMarkedGray(kind));

  AutoSetMarkColor setColor(*this, color);
  for (ArenaCellIterUnderGC cell(arena); !cell.done(); cell.next()) {
    if (cell->isMarked(color)) {
      JS::TraceChildren(this, JS::GCCellPtr(cell, kind));
    }
  }
}

/*
 * Process arenas from |delayedMarkingList| by marking the unmarked children of
 * marked cells of color |color|. Return early if the |budget| is exceeded.
 *
 * This is called twice, first to mark gray children and then to mark black
 * children.
 */
bool GCMarker::processDelayedMarkingList(MarkColor color, SliceBudget& budget) {
  // Marking delayed children may add more arenas to the list, including arenas
  // we are currently processing or have previously processed. Handle this by
  // clearing a flag on each arena before marking its children. This flag will
  // be set again if the arena is re-added. Iterate the list until no new arenas
  // were added.

  do {
    delayedMarkingWorkAdded = false;
    for (Arena* arena = delayedMarkingList; arena;
         arena = arena->getNextDelayedMarking()) {
      if (!arena->hasDelayedMarking(color)) {
        continue;
      }
      arena->setHasDelayedMarking(color, false);
      markDelayedChildren(arena, color);
      budget.step(150);
      if (budget.isOverBudget()) {
        return false;
      }
    }
  } while (delayedMarkingWorkAdded);

  return true;
}

bool GCMarker::markAllDelayedChildren(SliceBudget& budget,
                                      ShouldReportMarkTime reportTime) {
  MOZ_ASSERT(!hasBlackEntries());
  MOZ_ASSERT(markColor() == MarkColor::Black);

  GCRuntime& gc = runtime()->gc;
  mozilla::Maybe<gcstats::AutoPhase> ap;
  if (reportTime) {
    ap.emplace(gc.stats(), gcstats::PhaseKind::MARK_DELAYED);
  }

  // We have a list of arenas containing marked cells with unmarked children
  // where we ran out of stack space during marking.
  //
  // Both black and gray cells in these arenas may have unmarked children, and
  // we must mark gray children first as gray entries always sit before black
  // entries on the mark stack. Therefore the list is processed in two stages.

  MOZ_ASSERT(delayedMarkingList);

  bool finished;
  finished = processDelayedMarkingList(MarkColor::Gray, budget);
  rebuildDelayedMarkingList();
  if (!finished) {
    return false;
  }

  finished = processDelayedMarkingList(MarkColor::Black, budget);
  rebuildDelayedMarkingList();

  MOZ_ASSERT_IF(finished, !delayedMarkingList);
  MOZ_ASSERT_IF(finished, !markLaterArenas);

  return finished;
}

void GCMarker::rebuildDelayedMarkingList() {
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

inline void GCMarker::appendToDelayedMarkingList(Arena** listTail,
                                                 Arena* arena) {
  if (*listTail) {
    (*listTail)->updateNextDelayedMarkingArena(arena);
  } else {
    delayedMarkingList = arena;
  }
  *listTail = arena;
}

#ifdef DEBUG
void GCMarker::checkZone(void* p) {
  MOZ_ASSERT(state != MarkingState::NotActive);
  DebugOnly<Cell*> cell = static_cast<Cell*>(p);
  MOZ_ASSERT_IF(cell->isTenured(),
                cell->asTenured().zone()->isCollectingFromAnyThread());
}
#endif

size_t GCMarker::sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) const {
  size_t size = stack.sizeOfExcludingThis(mallocSizeOf);
  size += auxStack.sizeOfExcludingThis(mallocSizeOf);
  for (ZonesIter zone(runtime(), WithAtoms); !zone.done(); zone.next()) {
    size += zone->gcGrayRoots().SizeOfExcludingThis(mallocSizeOf);
  }
  return size;
}

/*** Tenuring Tracer ********************************************************/

static inline void UpdateAllocSiteOnTenure(Cell* cell) {
  AllocSite* site = NurseryCellHeader::from(cell)->allocSite();
  site->incTenuredCount();
}

JSObject* TenuringTracer::onObjectEdge(JSObject* obj) {
  if (!IsInsideNursery(obj)) {
    return obj;
  }

  if (obj->isForwarded()) {
    const gc::RelocationOverlay* overlay = gc::RelocationOverlay::fromCell(obj);
    return static_cast<JSObject*>(overlay->forwardingAddress());
  }

  UpdateAllocSiteOnTenure(obj);

  // Take a fast path for tenuring a plain object which is by far the most
  // common case.
  if (obj->is<PlainObject>()) {
    return movePlainObjectToTenured(&obj->as<PlainObject>());
  }

  return moveToTenuredSlow(obj);
}

JSString* TenuringTracer::onStringEdge(JSString* str) {
  if (!IsInsideNursery(str)) {
    return str;
  }

  if (str->isForwarded()) {
    const gc::RelocationOverlay* overlay = gc::RelocationOverlay::fromCell(str);
    return static_cast<JSString*>(overlay->forwardingAddress());
  }

  UpdateAllocSiteOnTenure(str);

  return moveToTenured(str);
}

JS::BigInt* TenuringTracer::onBigIntEdge(JS::BigInt* bi) {
  if (!IsInsideNursery(bi)) {
    return bi;
  }

  if (bi->isForwarded()) {
    const gc::RelocationOverlay* overlay = gc::RelocationOverlay::fromCell(bi);
    return static_cast<JS::BigInt*>(overlay->forwardingAddress());
  }

  UpdateAllocSiteOnTenure(bi);

  return moveToTenured(bi);
}

JS::Symbol* TenuringTracer::onSymbolEdge(JS::Symbol* sym) { return sym; }
js::BaseScript* TenuringTracer::onScriptEdge(BaseScript* script) {
  return script;
}
js::Shape* TenuringTracer::onShapeEdge(Shape* shape) { return shape; }
js::RegExpShared* TenuringTracer::onRegExpSharedEdge(RegExpShared* shared) {
  return shared;
}
js::BaseShape* TenuringTracer::onBaseShapeEdge(BaseShape* base) { return base; }
js::GetterSetter* TenuringTracer::onGetterSetterEdge(GetterSetter* gs) {
  return gs;
}
js::PropMap* TenuringTracer::onPropMapEdge(PropMap* map) { return map; }
js::jit::JitCode* TenuringTracer::onJitCodeEdge(jit::JitCode* code) {
  return code;
}
js::Scope* TenuringTracer::onScopeEdge(Scope* scope) { return scope; }

void TenuringTracer::traverse(JS::Value* thingp) {
  MOZ_ASSERT(!nursery().isInside(thingp));

  Value value = *thingp;
  CheckTracedThing(this, value);

  // We only care about a few kinds of GC thing here and this generates much
  // tighter code than using MapGCThingTyped.
  Value post;
  if (value.isObject()) {
    post = JS::ObjectValue(*onObjectEdge(&value.toObject()));
  } else if (value.isString()) {
    post = JS::StringValue(onStringEdge(value.toString()));
  } else if (value.isBigInt()) {
    post = JS::BigIntValue(onBigIntEdge(value.toBigInt()));
  } else {
    return;
  }

  if (post != value) {
    *thingp = post;
  }
}

template <typename T>
void js::gc::StoreBuffer::MonoTypeBuffer<T>::trace(TenuringTracer& mover) {
  mozilla::ReentrancyGuard g(*owner_);
  MOZ_ASSERT(owner_->isEnabled());
  if (last_) {
    last_.trace(mover);
  }
  for (typename StoreSet::Range r = stores_.all(); !r.empty(); r.popFront()) {
    r.front().trace(mover);
  }
}

namespace js {
namespace gc {
template void StoreBuffer::MonoTypeBuffer<StoreBuffer::ValueEdge>::trace(
    TenuringTracer&);
template void StoreBuffer::MonoTypeBuffer<StoreBuffer::SlotsEdge>::trace(
    TenuringTracer&);
template struct StoreBuffer::MonoTypeBuffer<StoreBuffer::StringPtrEdge>;
template struct StoreBuffer::MonoTypeBuffer<StoreBuffer::BigIntPtrEdge>;
template struct StoreBuffer::MonoTypeBuffer<StoreBuffer::ObjectPtrEdge>;
}  // namespace gc
}  // namespace js

void js::gc::StoreBuffer::SlotsEdge::trace(TenuringTracer& mover) const {
  NativeObject* obj = object();
  MOZ_ASSERT(IsCellPointerValid(obj));

  // Beware JSObject::swap exchanging a native object for a non-native one.
  if (!obj->is<NativeObject>()) {
    return;
  }

  MOZ_ASSERT(!IsInsideNursery(obj), "obj shouldn't live in nursery.");

  if (kind() == ElementKind) {
    uint32_t initLen = obj->getDenseInitializedLength();
    uint32_t numShifted = obj->getElementsHeader()->numShiftedElements();
    uint32_t clampedStart = start_;
    clampedStart = numShifted < clampedStart ? clampedStart - numShifted : 0;
    clampedStart = std::min(clampedStart, initLen);
    uint32_t clampedEnd = start_ + count_;
    clampedEnd = numShifted < clampedEnd ? clampedEnd - numShifted : 0;
    clampedEnd = std::min(clampedEnd, initLen);
    MOZ_ASSERT(clampedStart <= clampedEnd);
    mover.traceSlots(
        static_cast<HeapSlot*>(obj->getDenseElements() + clampedStart)
            ->unbarrieredAddress(),
        clampedEnd - clampedStart);
  } else {
    uint32_t start = std::min(start_, obj->slotSpan());
    uint32_t end = std::min(start_ + count_, obj->slotSpan());
    MOZ_ASSERT(start <= end);
    mover.traceObjectSlots(obj, start, end);
  }
}

static inline void TraceWholeCell(TenuringTracer& mover, JSObject* object) {
  MOZ_ASSERT_IF(object->storeBuffer(),
                !object->storeBuffer()->markingNondeduplicatable);
  mover.traceObject(object);
}

// Non-deduplicatable marking is necessary because of the following 2 reasons:
//
// 1. Tenured string chars cannot be updated:
//
//    If any of the tenured string's bases were deduplicated during tenuring,
//    the tenured string's chars pointer would need to be adjusted. This would
//    then require updating any other tenured strings that are dependent on the
//    first tenured string, and we have no way to find them without scanning
//    the entire tenured heap.
//
// 2. Tenured string cannot store its nursery base or base's chars:
//
//    Tenured strings have no place to stash a pointer to their nursery base or
//    its chars. You need to be able to traverse any dependent string's chain
//    of bases up to a nursery "root base" that points to the malloced chars
//    that the dependent strings started out pointing to, so that you can
//    calculate the offset of any dependent string and update the ptr+offset if
//    the root base gets deduplicated to a different allocation. Tenured
//    strings in this base chain will stop you from reaching the nursery
//    version of the root base; you can only get to the tenured version, and it
//    has no place to store the original chars pointer.
static inline void PreventDeduplicationOfReachableStrings(JSString* str) {
  MOZ_ASSERT(str->isTenured());
  MOZ_ASSERT(!str->isForwarded());

  JSLinearString* baseOrRelocOverlay = str->nurseryBaseOrRelocOverlay();

  // Walk along the chain of dependent strings' base string pointers
  // to mark them all non-deduplicatable.
  while (true) {
    // baseOrRelocOverlay can be one of the three cases:
    // 1. forwarded nursery string:
    //    The forwarded string still retains the flag that can tell whether
    //    this string is a dependent string with a base. Its
    //    StringRelocationOverlay holds a saved pointer to its base in the
    //    nursery.
    // 2. not yet forwarded nursery string:
    //    Retrieve the base field directly from the string.
    // 3. tenured string:
    //    The nursery base chain ends here, so stop traversing.
    if (baseOrRelocOverlay->isForwarded()) {
      JSLinearString* tenuredBase = Forwarded(baseOrRelocOverlay);
      if (!tenuredBase->hasBase()) {
        break;
      }
      baseOrRelocOverlay = StringRelocationOverlay::fromCell(baseOrRelocOverlay)
                               ->savedNurseryBaseOrRelocOverlay();
    } else {
      JSLinearString* base = baseOrRelocOverlay;
      if (base->isTenured()) {
        break;
      }
      if (base->isDeduplicatable()) {
        base->setNonDeduplicatable();
      }
      if (!base->hasBase()) {
        break;
      }
      baseOrRelocOverlay = base->nurseryBaseOrRelocOverlay();
    }
  }
}

static inline void TraceWholeCell(TenuringTracer& mover, JSString* str) {
  MOZ_ASSERT_IF(str->storeBuffer(),
                str->storeBuffer()->markingNondeduplicatable);

  // Mark all strings reachable from the tenured string `str` as
  // non-deduplicatable. These strings are the bases of the tenured dependent
  // string.
  if (str->hasBase()) {
    PreventDeduplicationOfReachableStrings(str);
  }

  str->traceChildren(&mover);
}

static inline void TraceWholeCell(TenuringTracer& mover, BaseScript* script) {
  script->traceChildren(&mover);
}

static inline void TraceWholeCell(TenuringTracer& mover,
                                  jit::JitCode* jitcode) {
  jitcode->traceChildren(&mover);
}

template <typename T>
static void TraceBufferedCells(TenuringTracer& mover, Arena* arena,
                               ArenaCellSet* cells) {
  for (size_t i = 0; i < MaxArenaCellIndex; i += cells->BitsPerWord) {
    ArenaCellSet::WordT bitset = cells->getWord(i / cells->BitsPerWord);
    while (bitset) {
      size_t bit = i + js::detail::CountTrailingZeroes(bitset);
      auto cell =
          reinterpret_cast<T*>(uintptr_t(arena) + ArenaCellIndexBytes * bit);
      TraceWholeCell(mover, cell);
      bitset &= bitset - 1;  // Clear the low bit.
    }
  }
}

void ArenaCellSet::trace(TenuringTracer& mover) {
  for (ArenaCellSet* cells = this; cells; cells = cells->next) {
    cells->check();

    Arena* arena = cells->arena;
    arena->bufferedCells() = &ArenaCellSet::Empty;

    JS::TraceKind kind = MapAllocToTraceKind(arena->getAllocKind());
    switch (kind) {
      case JS::TraceKind::Object:
        TraceBufferedCells<JSObject>(mover, arena, cells);
        break;
      case JS::TraceKind::String:
        TraceBufferedCells<JSString>(mover, arena, cells);
        break;
      case JS::TraceKind::Script:
        TraceBufferedCells<BaseScript>(mover, arena, cells);
        break;
      case JS::TraceKind::JitCode:
        TraceBufferedCells<jit::JitCode>(mover, arena, cells);
        break;
      default:
        MOZ_CRASH("Unexpected trace kind");
    }
  }
}

void js::gc::StoreBuffer::WholeCellBuffer::trace(TenuringTracer& mover) {
  MOZ_ASSERT(owner_->isEnabled());

#ifdef DEBUG
  // Verify that all string whole cells are traced first before any other
  // strings are visited for any reason.
  MOZ_ASSERT(!owner_->markingNondeduplicatable);
  owner_->markingNondeduplicatable = true;
#endif
  // Trace all of the strings to mark the non-deduplicatable bits, then trace
  // all other whole cells.
  if (stringHead_) {
    stringHead_->trace(mover);
  }
#ifdef DEBUG
  owner_->markingNondeduplicatable = false;
#endif
  if (nonStringHead_) {
    nonStringHead_->trace(mover);
  }

  stringHead_ = nonStringHead_ = nullptr;
}

template <typename T>
void js::gc::StoreBuffer::CellPtrEdge<T>::trace(TenuringTracer& mover) const {
  static_assert(std::is_base_of_v<Cell, T>, "T must be a Cell type");
  static_assert(!std::is_base_of_v<TenuredCell, T>,
                "T must not be a tenured Cell type");

  T* thing = *edge;
  if (!thing) {
    return;
  }

  MOZ_ASSERT(IsCellPointerValid(thing));
  MOZ_ASSERT(thing->getTraceKind() == JS::MapTypeToTraceKind<T>::kind);

  if (std::is_same_v<JSString, T>) {
    // Nursery string deduplication requires all tenured string -> nursery
    // string edges to be registered with the whole cell buffer in order to
    // correctly set the non-deduplicatable bit.
    MOZ_ASSERT(!mover.runtime()->gc.isPointerWithinTenuredCell(
        edge, JS::TraceKind::String));
  }

  *edge = DispatchToOnEdge(&mover, thing);
}

void js::gc::StoreBuffer::ValueEdge::trace(TenuringTracer& mover) const {
  if (deref()) {
    mover.traverse(edge);
  }
}

// Visit all object children of the object and trace them.
void js::TenuringTracer::traceObject(JSObject* obj) {
  CallTraceHook(this, obj);

  if (!obj->is<NativeObject>()) {
    return;
  }

  NativeObject* nobj = &obj->as<NativeObject>();
  if (!nobj->hasEmptyElements()) {
    HeapSlotArray elements = nobj->getDenseElements();
    Value* elems = elements.begin()->unbarrieredAddress();
    traceSlots(elems, elems + nobj->getDenseInitializedLength());
  }

  traceObjectSlots(nobj, 0, nobj->slotSpan());
}

void js::TenuringTracer::traceObjectSlots(NativeObject* nobj, uint32_t start,
                                          uint32_t end) {
  HeapSlot* fixedStart;
  HeapSlot* fixedEnd;
  HeapSlot* dynStart;
  HeapSlot* dynEnd;
  nobj->getSlotRange(start, end, &fixedStart, &fixedEnd, &dynStart, &dynEnd);
  if (fixedStart) {
    traceSlots(fixedStart->unbarrieredAddress(),
               fixedEnd->unbarrieredAddress());
  }
  if (dynStart) {
    traceSlots(dynStart->unbarrieredAddress(), dynEnd->unbarrieredAddress());
  }
}

void js::TenuringTracer::traceSlots(Value* vp, Value* end) {
  for (; vp != end; ++vp) {
    traverse(vp);
  }
}

inline void js::TenuringTracer::traceSlots(JS::Value* vp, uint32_t nslots) {
  traceSlots(vp, vp + nslots);
}

void js::TenuringTracer::traceString(JSString* str) {
  str->traceChildren(this);
}

void js::TenuringTracer::traceBigInt(JS::BigInt* bi) {
  bi->traceChildren(this);
}

#ifdef DEBUG
static inline uintptr_t OffsetFromChunkStart(void* p) {
  return uintptr_t(p) & gc::ChunkMask;
}
static inline ptrdiff_t OffsetToChunkEnd(void* p) {
  return ChunkSize - (uintptr_t(p) & gc::ChunkMask);
}
#endif

/* Insert the given relocation entry into the list of things to visit. */
inline void js::TenuringTracer::insertIntoObjectFixupList(
    RelocationOverlay* entry) {
  *objTail = entry;
  objTail = &entry->nextRef();
  *objTail = nullptr;
}

template <typename T>
inline T* js::TenuringTracer::allocTenured(Zone* zone, AllocKind kind) {
  return static_cast<T*>(static_cast<Cell*>(AllocateCellInGC(zone, kind)));
}

JSString* js::TenuringTracer::allocTenuredString(JSString* src, Zone* zone,
                                                 AllocKind dstKind) {
  JSString* dst = allocTenured<JSString>(zone, dstKind);
  tenuredSize += moveStringToTenured(dst, src, dstKind);
  tenuredCells++;

  return dst;
}

JSObject* js::TenuringTracer::moveToTenuredSlow(JSObject* src) {
  MOZ_ASSERT(IsInsideNursery(src));
  MOZ_ASSERT(!src->nurseryZone()->usedByHelperThread());
  MOZ_ASSERT(!src->is<PlainObject>());

  AllocKind dstKind = src->allocKindForTenure(nursery());
  auto dst = allocTenured<JSObject>(src->nurseryZone(), dstKind);

  size_t srcSize = Arena::thingSize(dstKind);
  size_t dstSize = srcSize;

  /*
   * Arrays do not necessarily have the same AllocKind between src and dst.
   * We deal with this by copying elements manually, possibly re-inlining
   * them if there is adequate room inline in dst.
   *
   * For Arrays we're reducing tenuredSize to the smaller srcSize
   * because moveElementsToTenured() accounts for all Array elements,
   * even if they are inlined.
   */
  if (src->is<ArrayObject>()) {
    dstSize = srcSize = sizeof(NativeObject);
  } else if (src->is<TypedArrayObject>()) {
    TypedArrayObject* tarray = &src->as<TypedArrayObject>();
    // Typed arrays with inline data do not necessarily have the same
    // AllocKind between src and dst. The nursery does not allocate an
    // inline data buffer that has the same size as the slow path will do.
    // In the slow path, the Typed Array Object stores the inline data
    // in the allocated space that fits the AllocKind. In the fast path,
    // the nursery will allocate another buffer that is directly behind the
    // minimal JSObject. That buffer size plus the JSObject size is not
    // necessarily as large as the slow path's AllocKind size.
    if (tarray->hasInlineElements()) {
      AllocKind srcKind = GetGCObjectKind(TypedArrayObject::FIXED_DATA_START);
      size_t headerSize = Arena::thingSize(srcKind);
      srcSize = headerSize + tarray->byteLength();
    }
  }

  tenuredSize += dstSize;
  tenuredCells++;

  // Copy the Cell contents.
  MOZ_ASSERT(OffsetFromChunkStart(src) >= sizeof(ChunkBase));
  MOZ_ASSERT(OffsetToChunkEnd(src) >= ptrdiff_t(srcSize));
  js_memcpy(dst, src, srcSize);

  // Move the slots and elements, if we need to.
  if (src->is<NativeObject>()) {
    NativeObject* ndst = &dst->as<NativeObject>();
    NativeObject* nsrc = &src->as<NativeObject>();
    tenuredSize += moveSlotsToTenured(ndst, nsrc);
    tenuredSize += moveElementsToTenured(ndst, nsrc, dstKind);

    // There is a pointer into a dictionary mode object from the head of its
    // shape list. This is updated in Nursery::sweepDictionaryModeObjects().
  }

  JSObjectMovedOp op = dst->getClass()->extObjectMovedOp();
  MOZ_ASSERT_IF(src->is<ProxyObject>(), op == proxy_ObjectMoved);
  if (op) {
    // Tell the hazard analysis that the object moved hook can't GC.
    JS::AutoSuppressGCAnalysis nogc;
    tenuredSize += op(dst, src);
  } else {
    MOZ_ASSERT_IF(src->getClass()->hasFinalize(),
                  CanNurseryAllocateFinalizedClass(src->getClass()));
  }

  RelocationOverlay* overlay = RelocationOverlay::forwardCell(src, dst);
  insertIntoObjectFixupList(overlay);

  gcprobes::PromoteToTenured(src, dst);
  return dst;
}

inline JSObject* js::TenuringTracer::movePlainObjectToTenured(
    PlainObject* src) {
  // Fast path version of moveToTenuredSlow() for specialized for PlainObject.

  MOZ_ASSERT(IsInsideNursery(src));
  MOZ_ASSERT(!src->nurseryZone()->usedByHelperThread());

  AllocKind dstKind = src->allocKindForTenure();
  auto dst = allocTenured<PlainObject>(src->nurseryZone(), dstKind);

  size_t srcSize = Arena::thingSize(dstKind);
  tenuredSize += srcSize;
  tenuredCells++;

  // Copy the Cell contents.
  MOZ_ASSERT(OffsetFromChunkStart(src) >= sizeof(ChunkBase));
  MOZ_ASSERT(OffsetToChunkEnd(src) >= ptrdiff_t(srcSize));
  js_memcpy(dst, src, srcSize);

  // Move the slots and elements.
  tenuredSize += moveSlotsToTenured(dst, src);
  tenuredSize += moveElementsToTenured(dst, src, dstKind);

  MOZ_ASSERT(!dst->getClass()->extObjectMovedOp());

  RelocationOverlay* overlay = RelocationOverlay::forwardCell(src, dst);
  insertIntoObjectFixupList(overlay);

  gcprobes::PromoteToTenured(src, dst);
  return dst;
}

size_t js::TenuringTracer::moveSlotsToTenured(NativeObject* dst,
                                              NativeObject* src) {
  /* Fixed slots have already been copied over. */
  if (!src->hasDynamicSlots()) {
    return 0;
  }

  Zone* zone = src->nurseryZone();
  size_t count = src->numDynamicSlots();

  if (!nursery().isInside(src->slots_)) {
    AddCellMemory(dst, ObjectSlots::allocSize(count), MemoryUse::ObjectSlots);
    nursery().removeMallocedBufferDuringMinorGC(src->getSlotsHeader());
    return 0;
  }

  {
    AutoEnterOOMUnsafeRegion oomUnsafe;
    HeapSlot* allocation =
        zone->pod_malloc<HeapSlot>(ObjectSlots::allocCount(count));
    if (!allocation) {
      oomUnsafe.crash(ObjectSlots::allocSize(count),
                      "Failed to allocate slots while tenuring.");
    }

    ObjectSlots* slotsHeader = new (allocation)
        ObjectSlots(count, src->getSlotsHeader()->dictionarySlotSpan());
    dst->slots_ = slotsHeader->slots();
  }

  AddCellMemory(dst, ObjectSlots::allocSize(count), MemoryUse::ObjectSlots);

  PodCopy(dst->slots_, src->slots_, count);
  nursery().setSlotsForwardingPointer(src->slots_, dst->slots_, count);

  return count * sizeof(HeapSlot);
}

size_t js::TenuringTracer::moveElementsToTenured(NativeObject* dst,
                                                 NativeObject* src,
                                                 AllocKind dstKind) {
  if (src->hasEmptyElements()) {
    return 0;
  }

  Zone* zone = src->nurseryZone();

  ObjectElements* srcHeader = src->getElementsHeader();
  size_t nslots = srcHeader->numAllocatedElements();

  void* srcAllocatedHeader = src->getUnshiftedElementsHeader();

  /* TODO Bug 874151: Prefer to put element data inline if we have space. */
  if (!nursery().isInside(srcAllocatedHeader)) {
    MOZ_ASSERT(src->elements_ == dst->elements_);
    nursery().removeMallocedBufferDuringMinorGC(srcAllocatedHeader);

    AddCellMemory(dst, nslots * sizeof(HeapSlot), MemoryUse::ObjectElements);

    return 0;
  }

  // Shifted elements are copied too.
  uint32_t numShifted = srcHeader->numShiftedElements();

  /* Unlike other objects, Arrays can have fixed elements. */
  if (src->is<ArrayObject>() && nslots <= GetGCKindSlots(dstKind)) {
    dst->as<ArrayObject>().setFixedElements();
    js_memcpy(dst->getElementsHeader(), srcAllocatedHeader,
              nslots * sizeof(HeapSlot));
    dst->elements_ += numShifted;
    nursery().setElementsForwardingPointer(srcHeader, dst->getElementsHeader(),
                                           srcHeader->capacity);
    return nslots * sizeof(HeapSlot);
  }

  MOZ_ASSERT(nslots >= 2);

  ObjectElements* dstHeader;
  {
    AutoEnterOOMUnsafeRegion oomUnsafe;
    dstHeader =
        reinterpret_cast<ObjectElements*>(zone->pod_malloc<HeapSlot>(nslots));
    if (!dstHeader) {
      oomUnsafe.crash(sizeof(HeapSlot) * nslots,
                      "Failed to allocate elements while tenuring.");
    }
  }

  AddCellMemory(dst, nslots * sizeof(HeapSlot), MemoryUse::ObjectElements);

  js_memcpy(dstHeader, srcAllocatedHeader, nslots * sizeof(HeapSlot));
  dst->elements_ = dstHeader->elements() + numShifted;
  nursery().setElementsForwardingPointer(srcHeader, dst->getElementsHeader(),
                                         srcHeader->capacity);
  return nslots * sizeof(HeapSlot);
}

inline void js::TenuringTracer::insertIntoStringFixupList(
    StringRelocationOverlay* entry) {
  *stringTail = entry;
  stringTail = &entry->nextRef();
  *stringTail = nullptr;
}

JSString* js::TenuringTracer::moveToTenured(JSString* src) {
  MOZ_ASSERT(IsInsideNursery(src));
  MOZ_ASSERT(!src->nurseryZone()->usedByHelperThread());
  MOZ_ASSERT(!src->isExternal());

  AllocKind dstKind = src->getAllocKind();
  Zone* zone = src->nurseryZone();

  // If this string is in the StringToAtomCache, try to deduplicate it by using
  // the atom. Don't do this for dependent strings because they're more
  // complicated. See StringRelocationOverlay and DeduplicationStringHasher
  // comments.
  if (src->inStringToAtomCache() && src->isDeduplicatable() &&
      !src->hasBase()) {
    JSLinearString* linear = &src->asLinear();
    JSAtom* atom = runtime()->caches().stringToAtomCache.lookup(linear);
    MOZ_ASSERT(atom, "Why was the cache purged before minor GC?");

    // Only deduplicate if both strings have the same encoding, to not confuse
    // dependent strings.
    if (src->hasTwoByteChars() == atom->hasTwoByteChars()) {
      // The StringToAtomCache isn't used for inline strings (due to the minimum
      // length) so canOwnDependentChars must be true for both src and atom.
      // This means if there are dependent strings floating around using str's
      // chars, they will be able to use the chars from the atom.
      static_assert(StringToAtomCache::MinStringLength >
                    JSFatInlineString::MAX_LENGTH_LATIN1);
      static_assert(StringToAtomCache::MinStringLength >
                    JSFatInlineString::MAX_LENGTH_TWO_BYTE);
      MOZ_ASSERT(src->canOwnDependentChars());
      MOZ_ASSERT(atom->canOwnDependentChars());

      StringRelocationOverlay::forwardCell(src, atom);
      gcprobes::PromoteToTenured(src, atom);
      return atom;
    }
  }

  JSString* dst;

  // A live nursery string can only get deduplicated when:
  // 1. Its length is smaller than MAX_DEDUPLICATABLE_STRING_LENGTH:
  //    Hashing a long string can affect performance.
  // 2. It is linear:
  //    Deduplicating every node in it would end up doing O(n^2) hashing work.
  // 3. It is deduplicatable:
  //    The JSString NON_DEDUP_BIT flag is unset.
  // 4. It matches an entry in stringDeDupSet.

  if (src->length() < MAX_DEDUPLICATABLE_STRING_LENGTH && src->isLinear() &&
      src->isDeduplicatable() && nursery().stringDeDupSet.isSome()) {
    if (auto p = nursery().stringDeDupSet->lookup(src)) {
      // Deduplicate to the looked-up string!
      dst = *p;
      zone->stringStats.ref().noteDeduplicated(src->length(), src->allocSize());
      StringRelocationOverlay::forwardCell(src, dst);
      gcprobes::PromoteToTenured(src, dst);
      return dst;
    }

    dst = allocTenuredString(src, zone, dstKind);

    if (!nursery().stringDeDupSet->putNew(dst)) {
      // When there is oom caused by the stringDeDupSet, stop deduplicating
      // strings.
      nursery().stringDeDupSet.reset();
    }
  } else {
    dst = allocTenuredString(src, zone, dstKind);
    dst->clearNonDeduplicatable();
  }

  zone->stringStats.ref().noteTenured(src->allocSize());

  auto* overlay = StringRelocationOverlay::forwardCell(src, dst);
  MOZ_ASSERT(dst->isDeduplicatable());

  if (dst->hasBase() || dst->isRope()) {
    // dst or one of its leaves might have a base that will be deduplicated.
    // Insert the overlay into the fixup list to relocate it later.
    insertIntoStringFixupList(overlay);
  }

  gcprobes::PromoteToTenured(src, dst);
  return dst;
}

template <typename CharT>
void js::Nursery::relocateDependentStringChars(
    JSDependentString* tenuredDependentStr, JSLinearString* baseOrRelocOverlay,
    size_t* offset, bool* rootBaseNotYetForwarded, JSLinearString** rootBase) {
  MOZ_ASSERT(*offset == 0);
  MOZ_ASSERT(*rootBaseNotYetForwarded == false);
  MOZ_ASSERT(*rootBase == nullptr);

  JS::AutoCheckCannotGC nogc;

  const CharT* dependentStrChars =
      tenuredDependentStr->nonInlineChars<CharT>(nogc);

  // Traverse the dependent string nursery base chain to find the base that
  // it's using chars from.
  while (true) {
    if (baseOrRelocOverlay->isForwarded()) {
      JSLinearString* tenuredBase = Forwarded(baseOrRelocOverlay);
      StringRelocationOverlay* relocOverlay =
          StringRelocationOverlay::fromCell(baseOrRelocOverlay);

      if (!tenuredBase->hasBase()) {
        // The nursery root base is relocOverlay, it is tenured to tenuredBase.
        // Relocate tenuredDependentStr chars and reassign the tenured root base
        // as its base.
        JSLinearString* tenuredRootBase = tenuredBase;
        const CharT* rootBaseChars = relocOverlay->savedNurseryChars<CharT>();
        *offset = dependentStrChars - rootBaseChars;
        MOZ_ASSERT(*offset < tenuredRootBase->length());
        tenuredDependentStr->relocateNonInlineChars<const CharT*>(
            tenuredRootBase->nonInlineChars<CharT>(nogc), *offset);
        tenuredDependentStr->setBase(tenuredRootBase);
        return;
      }

      baseOrRelocOverlay = relocOverlay->savedNurseryBaseOrRelocOverlay();

    } else {
      JSLinearString* base = baseOrRelocOverlay;

      if (!base->hasBase()) {
        // The root base is not forwarded yet, it is simply base.
        *rootBase = base;

        // The root base can be in either the nursery or the tenured heap.
        // dependentStr chars needs to be relocated after traceString if the
        // root base is in the nursery.
        if (!(*rootBase)->isTenured()) {
          *rootBaseNotYetForwarded = true;
          const CharT* rootBaseChars = (*rootBase)->nonInlineChars<CharT>(nogc);
          *offset = dependentStrChars - rootBaseChars;
          MOZ_ASSERT(*offset < base->length(), "Tenured root base");
        }

        tenuredDependentStr->setBase(*rootBase);

        return;
      }

      baseOrRelocOverlay = base->nurseryBaseOrRelocOverlay();
    }
  }
}

JS::BigInt* js::TenuringTracer::moveToTenured(JS::BigInt* src) {
  MOZ_ASSERT(IsInsideNursery(src));
  MOZ_ASSERT(!src->nurseryZone()->usedByHelperThread());

  AllocKind dstKind = src->getAllocKind();
  Zone* zone = src->nurseryZone();
  zone->tenuredBigInts++;

  JS::BigInt* dst = allocTenured<JS::BigInt>(zone, dstKind);
  tenuredSize += moveBigIntToTenured(dst, src, dstKind);
  tenuredCells++;

  RelocationOverlay::forwardCell(src, dst);

  gcprobes::PromoteToTenured(src, dst);
  return dst;
}

void js::Nursery::collectToObjectFixedPoint(TenuringTracer& mover) {
  for (RelocationOverlay* p = mover.objHead; p; p = p->next()) {
    auto* obj = static_cast<JSObject*>(p->forwardingAddress());
    mover.traceObject(obj);
  }
}

void js::Nursery::collectToStringFixedPoint(TenuringTracer& mover) {
  for (StringRelocationOverlay* p = mover.stringHead; p; p = p->next()) {
    auto* tenuredStr = static_cast<JSString*>(p->forwardingAddress());
    // To ensure the NON_DEDUP_BIT was reset properly.
    MOZ_ASSERT(tenuredStr->isDeduplicatable());

    // The nursery root base might not be forwarded before
    // traceString(tenuredStr). traceString(tenuredStr) will forward the root
    // base if that's the case. Dependent string chars needs to be relocated
    // after traceString if root base was not forwarded.
    size_t offset = 0;
    bool rootBaseNotYetForwarded = false;
    JSLinearString* rootBase = nullptr;

    if (tenuredStr->isDependent()) {
      if (tenuredStr->hasTwoByteChars()) {
        relocateDependentStringChars<char16_t>(
            &tenuredStr->asDependent(), p->savedNurseryBaseOrRelocOverlay(),
            &offset, &rootBaseNotYetForwarded, &rootBase);
      } else {
        relocateDependentStringChars<JS::Latin1Char>(
            &tenuredStr->asDependent(), p->savedNurseryBaseOrRelocOverlay(),
            &offset, &rootBaseNotYetForwarded, &rootBase);
      }
    }

    mover.traceString(tenuredStr);

    if (rootBaseNotYetForwarded) {
      MOZ_ASSERT(rootBase->isForwarded(),
                 "traceString() should make it forwarded");
      JS::AutoCheckCannotGC nogc;

      JSLinearString* tenuredRootBase = Forwarded(rootBase);
      MOZ_ASSERT(offset < tenuredRootBase->length());

      if (tenuredStr->hasTwoByteChars()) {
        tenuredStr->asDependent().relocateNonInlineChars<const char16_t*>(
            tenuredRootBase->twoByteChars(nogc), offset);
      } else {
        tenuredStr->asDependent().relocateNonInlineChars<const JS::Latin1Char*>(
            tenuredRootBase->latin1Chars(nogc), offset);
      }
      tenuredStr->setBase(tenuredRootBase);
    }
  }
}

size_t js::TenuringTracer::moveStringToTenured(JSString* dst, JSString* src,
                                               AllocKind dstKind) {
  size_t size = Arena::thingSize(dstKind);

  // At the moment, strings always have the same AllocKind between src and
  // dst. This may change in the future.
  MOZ_ASSERT(dst->asTenured().getAllocKind() == src->getAllocKind());

  // Copy the Cell contents.
  MOZ_ASSERT(OffsetToChunkEnd(src) >= ptrdiff_t(size));
  js_memcpy(dst, src, size);

  if (src->ownsMallocedChars()) {
    void* chars = src->asLinear().nonInlineCharsRaw();
    nursery().removeMallocedBufferDuringMinorGC(chars);
    AddCellMemory(dst, dst->asLinear().allocSize(), MemoryUse::StringContents);
  }

  return size;
}

size_t js::TenuringTracer::moveBigIntToTenured(JS::BigInt* dst, JS::BigInt* src,
                                               AllocKind dstKind) {
  size_t size = Arena::thingSize(dstKind);

  // At the moment, BigInts always have the same AllocKind between src and
  // dst. This may change in the future.
  MOZ_ASSERT(dst->asTenured().getAllocKind() == src->getAllocKind());

  // Copy the Cell contents.
  MOZ_ASSERT(OffsetToChunkEnd(src) >= ptrdiff_t(size));
  js_memcpy(dst, src, size);

  MOZ_ASSERT(dst->zone() == src->nurseryZone());

  if (src->hasHeapDigits()) {
    size_t length = dst->digitLength();
    if (!nursery().isInside(src->heapDigits_)) {
      nursery().removeMallocedBufferDuringMinorGC(src->heapDigits_);
    } else {
      Zone* zone = src->nurseryZone();
      {
        AutoEnterOOMUnsafeRegion oomUnsafe;
        dst->heapDigits_ = zone->pod_malloc<JS::BigInt::Digit>(length);
        if (!dst->heapDigits_) {
          oomUnsafe.crash(sizeof(JS::BigInt::Digit) * length,
                          "Failed to allocate digits while tenuring.");
        }
      }

      PodCopy(dst->heapDigits_, src->heapDigits_, length);
      nursery().setDirectForwardingPointer(src->heapDigits_, dst->heapDigits_);

      size += length * sizeof(JS::BigInt::Digit);
    }

    AddCellMemory(dst, length * sizeof(JS::BigInt::Digit),
                  MemoryUse::BigIntDigits);
  }

  return size;
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
  if (thing->isPermanentAndMayBeShared()) {
    return;
  }

  // Allow the current thread access if it is sweeping or in sweep-marking, but
  // try to check the zone. Some threads have access to all zones when sweeping.
  JSContext* cx = TlsContext.get();
  MOZ_ASSERT(cx->gcUse != JSContext::GCUse::Finalizing);
  if (cx->gcUse == JSContext::GCUse::Sweeping ||
      cx->gcUse == JSContext::GCUse::Marking) {
    Zone* zone = thing->zoneFromAnyThread();
    MOZ_ASSERT_IF(cx->gcSweepZone,
                  cx->gcSweepZone == zone || zone->isAtomsZone());
    return;
  }

  // Otherwise only allow access from the main thread or this zone's associated
  // thread.
  MOZ_ASSERT(CurrentThreadCanAccessRuntime(thing->runtimeFromAnyThread()) ||
             CurrentThreadCanAccessZone(thing->zoneFromAnyThread()));
#endif
}

template <typename T>
static inline bool ShouldCheckMarkState(JSRuntime* rt, T** thingp) {
  MOZ_ASSERT(thingp);
  CheckIsMarkedThing(*thingp);
  MOZ_ASSERT(!IsInsideNursery(*thingp));

  TenuredCell& thing = (*thingp)->asTenured();
  Zone* zone = thing.zoneFromAnyThread();

  if (zone->gcState() <= Zone::Prepare || zone->isGCFinished()) {
    return false;
  }

  if (zone->isGCCompacting() && IsForwarded(*thingp)) {
    *thingp = Forwarded(*thingp);
    return false;
  }

  return true;
}

template <typename T>
bool js::gc::IsMarkedInternal(JSRuntime* rt, T** thingp) {
  // Don't depend on the mark state of other cells during finalization.
  MOZ_ASSERT(!CurrentThreadIsGCFinalizing());

  T* thing = *thingp;
  if (IsOwnedByOtherRuntime(rt, thing)) {
    return true;
  }

  if (!thing->isTenured()) {
    MOZ_ASSERT(CurrentThreadCanAccessRuntime(rt));
    auto** cellp = reinterpret_cast<Cell**>(thingp);
    return Nursery::getForwardedPointer(cellp);
  }

  if (!ShouldCheckMarkState(rt, thingp)) {
    return true;
  }

  return (*thingp)->asTenured().isMarkedAny();
}

template <typename T>
bool js::gc::IsAboutToBeFinalizedInternal(T** thingp) {
  // Don't depend on the mark state of other cells during finalization.
  MOZ_ASSERT(!CurrentThreadIsGCFinalizing());

  MOZ_ASSERT(thingp);
  T* thing = *thingp;
  CheckIsMarkedThing(thing);
  JSRuntime* rt = thing->runtimeFromAnyThread();

  /* Permanent atoms are never finalized by non-owning runtimes. */
  if (thing->isPermanentAndMayBeShared() && TlsContext.get()->runtime() != rt) {
    return false;
  }

  if (!thing->isTenured()) {
    return JS::RuntimeHeapIsMinorCollecting() &&
           !Nursery::getForwardedPointer(reinterpret_cast<Cell**>(thingp));
  }

  Zone* zone = thing->asTenured().zoneFromAnyThread();
  if (zone->isGCSweeping()) {
    return !thing->asTenured().isMarkedAny();
  }

  if (zone->isGCCompacting() && IsForwarded(thing)) {
    *thingp = Forwarded(thing);
    return false;
  }

  return false;
}

template <typename T>
bool js::gc::IsAboutToBeFinalizedInternal(T* thingp) {
  bool dying = false;
  auto thing = MapGCThingTyped(*thingp, [&dying](auto t) {
    dying = IsAboutToBeFinalizedInternal(&t);
    return TaggedPtr<T>::wrap(t);
  });
  if (thing.isSome() && thing.value() != *thingp) {
    *thingp = thing.value();
  }
  return dying;
}

template <typename T>
inline T* SweepingTracer::onEdge(T* thing) {
  CheckIsMarkedThing(thing);

  JSRuntime* rt = thing->runtimeFromAnyThread();

  if (thing->isPermanentAndMayBeShared() && runtime() != rt) {
    return thing;
  }

  // TODO: We should assert the zone of the tenured cell is in Sweeping state,
  // however we need to fix atoms and JitcodeGlobalTable first.
  // Bug 1501334 : IsAboutToBeFinalized doesn't work for atoms
  // Bug 1071218 : Refactor Debugger::sweepAll and
  //               JitRuntime::SweepJitcodeGlobalTable to work per sweep group
  if (!thing->isMarkedAny()) {
    return nullptr;
  }

  return thing;
}

JSObject* SweepingTracer::onObjectEdge(JSObject* obj) { return onEdge(obj); }
Shape* SweepingTracer::onShapeEdge(Shape* shape) { return onEdge(shape); }
JSString* SweepingTracer::onStringEdge(JSString* string) {
  return onEdge(string);
}
js::BaseScript* SweepingTracer::onScriptEdge(js::BaseScript* script) {
  return onEdge(script);
}
BaseShape* SweepingTracer::onBaseShapeEdge(BaseShape* base) {
  return onEdge(base);
}
GetterSetter* SweepingTracer::onGetterSetterEdge(GetterSetter* gs) {
  return onEdge(gs);
}
PropMap* SweepingTracer::onPropMapEdge(PropMap* map) { return onEdge(map); }
jit::JitCode* SweepingTracer::onJitCodeEdge(jit::JitCode* jit) {
  return onEdge(jit);
}
Scope* SweepingTracer::onScopeEdge(Scope* scope) { return onEdge(scope); }
RegExpShared* SweepingTracer::onRegExpSharedEdge(RegExpShared* shared) {
  return onEdge(shared);
}
BigInt* SweepingTracer::onBigIntEdge(BigInt* bi) { return onEdge(bi); }
JS::Symbol* SweepingTracer::onSymbolEdge(JS::Symbol* sym) {
  return onEdge(sym);
}

namespace js {
namespace gc {

template <typename T>
JS_PUBLIC_API bool EdgeNeedsSweep(JS::Heap<T>* thingp) {
  return IsAboutToBeFinalizedInternal(ConvertToBase(thingp->unsafeGet()));
}

template <typename T>
JS_PUBLIC_API bool EdgeNeedsSweepUnbarrieredSlow(T* thingp) {
  return IsAboutToBeFinalizedInternal(ConvertToBase(thingp));
}

// Instantiate a copy of the Tracing templates for each public GC type.
#define INSTANTIATE_ALL_VALID_HEAP_TRACE_FUNCTIONS(type)             \
  template JS_PUBLIC_API bool EdgeNeedsSweep<type>(JS::Heap<type>*); \
  template JS_PUBLIC_API bool EdgeNeedsSweepUnbarrieredSlow<type>(type*);
JS_FOR_EACH_PUBLIC_GC_POINTER_TYPE(INSTANTIATE_ALL_VALID_HEAP_TRACE_FUNCTIONS)
JS_FOR_EACH_PUBLIC_TAGGED_GC_POINTER_TYPE(
    INSTANTIATE_ALL_VALID_HEAP_TRACE_FUNCTIONS)

#define INSTANTIATE_INTERNAL_IS_MARKED_FUNCTION(type) \
  template bool IsMarkedInternal(JSRuntime* rt, type* thing);

#define INSTANTIATE_INTERNAL_IATBF_FUNCTION(type) \
  template bool IsAboutToBeFinalizedInternal(type* thingp);

#define INSTANTIATE_INTERNAL_MARKING_FUNCTIONS_FROM_TRACEKIND(_1, type, _2, \
                                                              _3)           \
  INSTANTIATE_INTERNAL_IS_MARKED_FUNCTION(type*)                            \
  INSTANTIATE_INTERNAL_IATBF_FUNCTION(type*)

JS_FOR_EACH_TRACEKIND(INSTANTIATE_INTERNAL_MARKING_FUNCTIONS_FROM_TRACEKIND)

JS_FOR_EACH_PUBLIC_TAGGED_GC_POINTER_TYPE(INSTANTIATE_INTERNAL_IATBF_FUNCTION)

#undef INSTANTIATE_INTERNAL_IS_MARKED_FUNCTION
#undef INSTANTIATE_INTERNAL_IATBF_FUNCTION
#undef INSTANTIATE_INTERNAL_MARKING_FUNCTIONS_FROM_TRACEKIND

} /* namespace gc */
} /* namespace js */

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
  void onChild(const JS::GCCellPtr& thing) override {
    MOZ_ASSERT(!thing.asCell()->isMarkedGray());
  }
};
#endif

class UnmarkGrayTracer final : public JS::CallbackTracer {
 public:
  // We set weakMapAction to WeakMapTraceAction::Skip because the cycle
  // collector will fix up any color mismatches involving weakmaps when it runs.
  explicit UnmarkGrayTracer(JSRuntime* rt)
      : JS::CallbackTracer(rt, JS::TracerKind::UnmarkGray,
                           JS::WeakMapTraceAction::Skip),
        unmarkedAny(false),
        oom(false),
        stack(rt->gc.unmarkGrayStack) {}

  void unmark(JS::GCCellPtr cell);

  // Whether we unmarked anything.
  bool unmarkedAny;

  // Whether we ran out of memory.
  bool oom;

 private:
  // Stack of cells to traverse.
  Vector<JS::GCCellPtr, 0, SystemAllocPolicy>& stack;

  void onChild(const JS::GCCellPtr& thing) override;
};

void UnmarkGrayTracer::onChild(const JS::GCCellPtr& thing) {
  Cell* cell = thing.asCell();

  // Cells in the nursery cannot be gray, and nor can certain kinds of tenured
  // cells. These must necessarily point only to black edges.
  if (!cell->isTenured() ||
      !TraceKindCanBeMarkedGray(cell->asTenured().getTraceKind())) {
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
      // Skip disptaching on known tracer type.
      BarrierTracer* trc = &runtime()->gc.barrierTracer;
      trc->performBarrier(thing);
      unmarkedAny = true;
    }
    return;
  }

  if (!tenured.isMarkedGray()) {
    return;
  }

  tenured.markBlack();
  unmarkedAny = true;

  if (!stack.append(thing)) {
    oom = true;
  }
}

void UnmarkGrayTracer::unmark(JS::GCCellPtr cell) {
  MOZ_ASSERT(stack.empty());

  onChild(cell);

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

bool js::gc::UnmarkGrayGCThingUnchecked(JSRuntime* rt, JS::GCCellPtr thing) {
  MOZ_ASSERT(thing);
  MOZ_ASSERT(thing.asCell()->isMarkedGray());

  AutoGeckoProfilerEntry profilingStackFrame(
      TlsContext.get(), "UnmarkGrayGCThing",
      JS::ProfilingCategoryPair::GCCC_UnmarkGray);

  UnmarkGrayTracer unmarker(rt);
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

  gcstats::AutoPhase outerPhase(rt->gc.stats(), gcstats::PhaseKind::BARRIER);
  gcstats::AutoPhase innerPhase(rt->gc.stats(),
                                gcstats::PhaseKind::UNMARK_GRAY);
  return UnmarkGrayGCThingUnchecked(rt, thing);
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

#ifdef DEBUG
static bool CellHasChildren(JS::GCCellPtr cell) {
  struct Tracer : public JS::CallbackTracer {
    bool hasChildren = false;
    explicit Tracer(JSRuntime* runtime) : JS::CallbackTracer(runtime) {}
    void onChild(const JS::GCCellPtr& thing) { hasChildren = true; }
  };

  Tracer trc(cell.asCell()->runtimeFromMainThread());
  JS::TraceChildren(&trc, cell);
  return trc.hasChildren;
}
#endif

static bool CellMayHaveChildren(JS::GCCellPtr cell) {
  bool mayHaveChildren;

  switch (cell.kind()) {
    case JS::TraceKind::BigInt:
      mayHaveChildren = false;
      break;

    case JS::TraceKind::String: {
      JSString* string = &cell.as<JSString>();
      mayHaveChildren = string->hasBase() || string->isRope();
      break;
    }

    default:
      mayHaveChildren = true;
      break;
  }

  MOZ_ASSERT_IF(!mayHaveChildren, !CellHasChildren(cell));
  return mayHaveChildren;
}

/* static */
BarrierTracer* BarrierTracer::fromTracer(JSTracer* trc) {
  MOZ_ASSERT(trc->kind() == JS::TracerKind::Barrier);
  return static_cast<BarrierTracer*>(trc->asGenericTracer());
}

BarrierTracer::BarrierTracer(JSRuntime* rt)
    : GenericTracer(rt, JS::TracerKind::Barrier, JS::WeakEdgeTraceAction::Skip),
      marker(rt->gc.marker) {}

JSObject* BarrierTracer::onObjectEdge(JSObject* obj) {
  PreWriteBarrier(obj);
  return obj;
}
Shape* BarrierTracer::onShapeEdge(Shape* shape) {
  PreWriteBarrier(shape);
  return shape;
}
JSString* BarrierTracer::onStringEdge(JSString* string) {
  PreWriteBarrier(string);
  return string;
}
js::BaseScript* BarrierTracer::onScriptEdge(js::BaseScript* script) {
  PreWriteBarrier(script);
  return script;
}
BaseShape* BarrierTracer::onBaseShapeEdge(BaseShape* base) {
  PreWriteBarrier(base);
  return base;
}
GetterSetter* BarrierTracer::onGetterSetterEdge(GetterSetter* gs) {
  PreWriteBarrier(gs);
  return gs;
}
PropMap* BarrierTracer::onPropMapEdge(PropMap* map) {
  PreWriteBarrier(map);
  return map;
}
Scope* BarrierTracer::onScopeEdge(Scope* scope) {
  PreWriteBarrier(scope);
  return scope;
}
RegExpShared* BarrierTracer::onRegExpSharedEdge(RegExpShared* shared) {
  PreWriteBarrier(shared);
  return shared;
}
BigInt* BarrierTracer::onBigIntEdge(BigInt* bi) {
  PreWriteBarrier(bi);
  return bi;
}
JS::Symbol* BarrierTracer::onSymbolEdge(JS::Symbol* sym) {
  PreWriteBarrier(sym);
  return sym;
}
jit::JitCode* BarrierTracer::onJitCodeEdge(jit::JitCode* jit) {
  PreWriteBarrier(jit);
  return jit;
}

// If the barrier buffer grows too large, trace all barriered things at that
// point.
constexpr static size_t MaxBarrierBufferSize = 4096;

void BarrierTracer::performBarrier(JS::GCCellPtr cell) {
  MOZ_ASSERT(CurrentThreadCanAccessRuntime(runtime()));
  MOZ_ASSERT(!runtime()->gc.isBackgroundMarking());
  MOZ_ASSERT(!cell.asCell()->isForwarded());

  // Mark the cell here to prevent us recording it again.
  if (!cell.asCell()->asTenured().markIfUnmarked()) {
    return;
  }

  // NOTE: This assumes that cells that don't have children do not require their
  // traceChildren method to be called.
  bool requiresTracing = CellMayHaveChildren(cell);
  if (!requiresTracing) {
    return;
  }

  BarrierBuffer& buffer = marker.barrierBuffer();
  if (buffer.length() >= MaxBarrierBufferSize || !buffer.append(cell)) {
    handleBufferFull(cell);
  }
}

void BarrierTracer::handleBufferFull(JS::GCCellPtr cell) {
  SliceBudget budget = SliceBudget::unlimited();
  marker.traceBarrieredCells(budget);
  marker.traceBarrieredCell(cell);
}

bool GCMarker::traceBarrieredCells(SliceBudget& budget) {
  MOZ_ASSERT(CurrentThreadCanAccessRuntime(runtime()) ||
             CurrentThreadIsGCMarking());
  MOZ_ASSERT(markColor() == MarkColor::Black);

  AutoGeckoProfilerEntry profilingStackFrame(
      TlsContext.get(), "GCMarker::traceBarrieredCells",
      JS::ProfilingCategoryPair::GCCC_Barrier);

  BarrierBuffer& buffer = barrierBuffer();
  while (!buffer.empty()) {
    traceBarrieredCell(buffer.popCopy());
    budget.step();
    if (budget.isOverBudget()) {
      return false;
    }
  }

  return true;
}

void GCMarker::traceBarrieredCell(JS::GCCellPtr cell) {
  MOZ_ASSERT(cell.asCell()->isTenured());
  MOZ_ASSERT(cell.asCell()->isMarkedBlack());
  MOZ_ASSERT(!cell.asCell()->isForwarded());

  ApplyGCThingTyped(cell, [this](auto thing) {
    MOZ_ASSERT(ShouldMark(this, thing));
    MOZ_ASSERT(thing->isMarkedBlack());

    if constexpr (std::is_same_v<decltype(thing), JSString*>) {
      if (thing->isRope() && thing->asRope().isBeingFlattened()) {
        // This string is an interior node of a rope that is currently being
        // flattened. The flattening process invokes the barrier on all nodes in
        // the tree, so interior nodes need not be traversed.
        return;
      }
    }

    CheckTracedThing(this, thing);
    AutoClearTracingSource acts(this);

    traverse(thing);
  });
}

namespace js {
namespace debug {

MarkInfo GetMarkInfo(Cell* rawCell) {
  if (!rawCell->isTenured()) {
    return MarkInfo::NURSERY;
  }

  TenuredCell* cell = &rawCell->asTenured();
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

}  // namespace debug
}  // namespace js
