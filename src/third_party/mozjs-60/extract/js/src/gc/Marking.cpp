/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "gc/Marking-inl.h"

#include "mozilla/DebugOnly.h"
#include "mozilla/IntegerRange.h"
#include "mozilla/ReentrancyGuard.h"
#include "mozilla/ScopeExit.h"
#include "mozilla/TypeTraits.h"

#include "jsfriendapi.h"

#include "builtin/ModuleObject.h"
#include "gc/GCInternals.h"
#include "gc/Policy.h"
#include "jit/IonCode.h"
#include "js/SliceBudget.h"
#include "vm/ArgumentsObject.h"
#include "vm/ArrayObject.h"
#include "vm/Debugger.h"
#include "vm/EnvironmentObject.h"
#include "vm/RegExpObject.h"
#include "vm/RegExpShared.h"
#include "vm/Scope.h"
#include "vm/Shape.h"
#include "vm/SymbolType.h"
#include "vm/TypedArrayObject.h"
#include "vm/UnboxedObject.h"
#include "wasm/WasmJS.h"

#include "gc/GC-inl.h"
#include "gc/Nursery-inl.h"
#include "gc/PrivateIterators-inl.h"
#include "vm/JSCompartment-inl.h"
#include "vm/NativeObject-inl.h"
#include "vm/StringType-inl.h"
#include "vm/UnboxedObject-inl.h"

using namespace js;
using namespace js::gc;

using JS::MapTypeToTraceKind;

using mozilla::DebugOnly;
using mozilla::IntegerRange;
using mozilla::IsBaseOf;
using mozilla::IsSame;
using mozilla::PodCopy;

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
//                                                                                              //
//   .---------.    .---------.    .--------------------------.       .----------.              //
//   |TraceEdge|    |TraceRoot|    |TraceManuallyBarrieredEdge|  ...  |TraceRange|   ... etc.   //
//   '---------'    '---------'    '--------------------------'       '----------'              //
//        \              \                        /                        /                    //
//         \              \  .----------------.  /                        /                     //
//          o------------->o-|DispatchToTracer|-o<-----------------------o                      //
//                           '----------------'                                                 //
//                              /          \                                                    //
//                             /            \                                                   //
//                       .---------.   .----------.         .-----------------.                 //
//                       |DoMarking|   |DoCallback|-------> |<JSTraceCallback>|----------->     //
//                       '---------'   '----------'         '-----------------'                 //
//                            |                                                                 //
//                            |                                                                 //
//                        .--------.                                                            //
//      o---------------->|traverse| .                                                          //
//     /_\                '--------'   ' .                                                      //
//      |                     .     .      ' .                                                  //
//      |                     .       .        ' .                                              //
//      |                     .         .          ' .                                          //
//      |             .-----------.    .-----------.   ' .     .--------------------.           //
//      |             |markAndScan|    |markAndPush|       ' - |markAndTraceChildren|---->      //
//      |             '-----------'    '-----------'           '--------------------'           //
//      |                   |                  \                                                //
//      |                   |                   \                                               //
//      |       .----------------------.     .----------------.                                 //
//      |       |T::eagerlyMarkChildren|     |pushMarkStackTop|<===Oo                           //
//      |       '----------------------'     '----------------'    ||                           //
//      |                  |                         ||            ||                           //
//      |                  |                         ||            ||                           //
//      |                  |                         ||            ||                           //
//      o<-----------------o<========================OO============Oo                           //
//                                                                                              //
//                                                                                              //
//   Legend:                                                                                    //
//     ------  Direct calls                                                                     //
//     . . .   Static dispatch                                                                  //
//     ======  Dispatch through a manual stack.                                                 //
//                                                                                              //


/*** Tracing Invariants **************************************************************************/

#if defined(DEBUG)
template<typename T>
static inline bool
IsThingPoisoned(T* thing)
{
    const uint8_t poisonBytes[] = {
        JS_FRESH_NURSERY_PATTERN,
        JS_SWEPT_NURSERY_PATTERN,
        JS_ALLOCATED_NURSERY_PATTERN,
        JS_FRESH_TENURED_PATTERN,
        JS_MOVED_TENURED_PATTERN,
        JS_SWEPT_TENURED_PATTERN,
        JS_ALLOCATED_TENURED_PATTERN,
        JS_SWEPT_CODE_PATTERN
    };
    const int numPoisonBytes = sizeof(poisonBytes) / sizeof(poisonBytes[0]);
    uint32_t* p = reinterpret_cast<uint32_t*>(reinterpret_cast<FreeSpan*>(thing) + 1);
    // Note: all free patterns are odd to make the common, not-poisoned case a single test.
    if ((*p & 1) == 0)
        return false;
    for (int i = 0; i < numPoisonBytes; ++i) {
        const uint8_t pb = poisonBytes[i];
        const uint32_t pw = pb | (pb << 8) | (pb << 16) | (pb << 24);
        if (*p == pw)
            return true;
    }
    return false;
}

static bool
IsMovingTracer(JSTracer *trc)
{
    return trc->isCallbackTracer() &&
           trc->asCallbackTracer()->getTracerKind() == JS::CallbackTracer::TracerKind::Moving;
}
#endif

bool ThingIsPermanentAtomOrWellKnownSymbol(JSString* str) {
    return str->isPermanentAtom();
}
bool ThingIsPermanentAtomOrWellKnownSymbol(JSFlatString* str) {
    return str->isPermanentAtom();
}
bool ThingIsPermanentAtomOrWellKnownSymbol(JSLinearString* str) {
    return str->isPermanentAtom();
}
bool ThingIsPermanentAtomOrWellKnownSymbol(JSAtom* atom) {
    return atom->isPermanent();
}
bool ThingIsPermanentAtomOrWellKnownSymbol(PropertyName* name) {
    return name->isPermanent();
}
bool ThingIsPermanentAtomOrWellKnownSymbol(JS::Symbol* sym) {
    return sym->isWellKnownSymbol();
}

template <typename T>
static inline bool
IsOwnedByOtherRuntime(JSRuntime* rt, T thing)
{
    bool other = thing->runtimeFromAnyThread() != rt;
    MOZ_ASSERT_IF(other,
                  ThingIsPermanentAtomOrWellKnownSymbol(thing) ||
                  thing->zoneFromAnyThread()->isSelfHostingZone());
    return other;
}

template<typename T>
void
js::CheckTracedThing(JSTracer* trc, T* thing)
{
#ifdef DEBUG
    MOZ_ASSERT(trc);
    MOZ_ASSERT(thing);

    if (!trc->checkEdges())
        return;

    if (IsForwarded(thing))
        thing = Forwarded(thing);

    /* This function uses data that's not available in the nursery. */
    if (IsInsideNursery(thing))
        return;

    MOZ_ASSERT_IF(!IsMovingTracer(trc) && !trc->isTenuringTracer(), !IsForwarded(thing));

    /*
     * Permanent atoms and things in the self-hosting zone are not associated
     * with this runtime, but will be ignored during marking.
     */
    if (IsOwnedByOtherRuntime(trc->runtime(), thing))
        return;

    Zone* zone = thing->zoneFromAnyThread();
    JSRuntime* rt = trc->runtime();

    if (!IsMovingTracer(trc) && !IsBufferGrayRootsTracer(trc) && !IsClearEdgesTracer(trc)) {
        MOZ_ASSERT(CurrentThreadCanAccessZone(zone));
        MOZ_ASSERT(CurrentThreadCanAccessRuntime(rt));
    }

    MOZ_ASSERT(zone->runtimeFromAnyThread() == trc->runtime());

    // It shouldn't be possible to trace into zones used by helper threads.
    MOZ_ASSERT(!zone->usedByHelperThread());

    MOZ_ASSERT(thing->isAligned());
    MOZ_ASSERT(MapTypeToTraceKind<typename mozilla::RemovePointer<T>::Type>::kind ==
               thing->getTraceKind());

    /*
     * Do not check IsMarkingTracer directly -- it should only be used in paths
     * where we cannot be the gray buffering tracer.
     */
    bool isGcMarkingTracer = trc->isMarkingTracer();

    MOZ_ASSERT_IF(zone->requireGCTracer(),
                  isGcMarkingTracer || IsBufferGrayRootsTracer(trc) || IsUnmarkGrayTracer(trc));

    if (isGcMarkingTracer) {
        GCMarker* gcMarker = GCMarker::fromTracer(trc);
        MOZ_ASSERT_IF(gcMarker->shouldCheckCompartments(),
                      zone->isCollecting() || zone->isAtomsZone());

        MOZ_ASSERT_IF(gcMarker->markColor() == MarkColor::Gray,
                      !zone->isGCMarkingBlack() || zone->isAtomsZone());

        MOZ_ASSERT(!(zone->isGCSweeping() || zone->isGCFinished() || zone->isGCCompacting()));
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
    MOZ_ASSERT_IF(JS::CurrentThreadIsHeapBusy() &&
                  !zone->isGCCompacting() &&
                  !rt->gc.isBackgroundSweeping(),
                  !IsThingPoisoned(thing) || !InFreeList(thing->asTenured().arena(), thing));
#endif
}

template <typename S>
struct CheckTracedFunctor : public VoidDefaultAdaptor<S> {
    template <typename T> void operator()(T* t, JSTracer* trc) { CheckTracedThing(trc, t); }
};

template<typename T>
void
js::CheckTracedThing(JSTracer* trc, T thing)
{
    DispatchTyped(CheckTracedFunctor<T>(), thing, trc);
}

namespace js {
#define IMPL_CHECK_TRACED_THING(_, type, __) \
    template void CheckTracedThing<type>(JSTracer*, type*);
JS_FOR_EACH_TRACEKIND(IMPL_CHECK_TRACED_THING);
#undef IMPL_CHECK_TRACED_THING
} // namespace js

static bool UnmarkGrayGCThing(JSRuntime* rt, JS::GCCellPtr thing);

static bool
ShouldTraceCrossCompartment(JSTracer* trc, JSObject* src, Cell* cell)
{
    if (!trc->isMarkingTracer())
        return true;

    MarkColor color = GCMarker::fromTracer(trc)->markColor();

    if (!cell->isTenured()) {
        MOZ_ASSERT(color == MarkColor::Black);
        return false;
    }
    TenuredCell& tenured = cell->asTenured();

    JS::Zone* zone = tenured.zone();
    if (!src->zone()->isGCMarking() && !zone->isGCMarking())
        return false;

    if (color == MarkColor::Black) {
        /*
         * Having black->gray edges violates our promise to the cycle
         * collector. This can happen if we're collecting a compartment and it
         * has an edge to an uncollected compartment: it's possible that the
         * source and destination of the cross-compartment edge should be gray,
         * but the source was marked black by the write barrier.
         */
        if (tenured.isMarkedGray()) {
            MOZ_ASSERT(!zone->isCollecting());
            UnmarkGrayGCThing(trc->runtime(), JS::GCCellPtr(cell, cell->getTraceKind()));
        }
        return zone->isGCMarking();
    } else {
        if (zone->isGCMarkingBlack()) {
            /*
             * The destination compartment is being not being marked gray now,
             * but it will be later, so record the cell so it can be marked gray
             * at the appropriate time.
             */
            if (!tenured.isMarkedAny())
                DelayCrossCompartmentGrayMarking(src);
            return false;
        }
        return zone->isGCMarkingGray();
    }
}

static bool
ShouldTraceCrossCompartment(JSTracer* trc, JSObject* src, const Value& val)
{
    return val.isGCThing() && ShouldTraceCrossCompartment(trc, src, val.toGCThing());
}

static void
AssertShouldMarkInZone(Cell* thing)
{
    MOZ_ASSERT(thing->asTenured().zone()->shouldMarkInZone());
}

static void
AssertShouldMarkInZone(JSString* str)
{
#ifdef DEBUG
    Zone* zone = str->zone();
    MOZ_ASSERT(zone->shouldMarkInZone() || zone->isAtomsZone());
#endif
}

static void
AssertShouldMarkInZone(JS::Symbol* sym)
{
#ifdef DEBUG
    Zone* zone = sym->asTenured().zone();
    MOZ_ASSERT(zone->shouldMarkInZone() || zone->isAtomsZone());
#endif
}

static void
AssertRootMarkingPhase(JSTracer* trc)
{
    MOZ_ASSERT_IF(trc->isMarkingTracer(),
                  trc->runtime()->gc.state() == State::NotActive ||
                  trc->runtime()->gc.state() == State::MarkRoots);
}


/*** Tracing Interface ***************************************************************************/

// The second parameter to BaseGCType is derived automatically based on T. The
// relation here is that for any T, the TraceKind will automatically,
// statically select the correct Cell layout for marking. Below, we instantiate
// each override with a declaration of the most derived layout type.
//
// The use of TraceKind::Null for the case where the type is not matched
// generates a compile error as no template instantiated for that kind.
//
// Usage:
//   BaseGCType<T>::type
//
// Examples:
//   BaseGCType<JSFunction>::type => JSObject
//   BaseGCType<UnownedBaseShape>::type => BaseShape
//   etc.
template <typename T, JS::TraceKind =
#define EXPAND_MATCH_TYPE(name, type, _) \
          IsBaseOf<type, T>::value ? JS::TraceKind::name :
JS_FOR_EACH_TRACEKIND(EXPAND_MATCH_TYPE)
#undef EXPAND_MATCH_TYPE
          JS::TraceKind::Null>

struct BaseGCType;
#define IMPL_BASE_GC_TYPE(name, type_, _) \
    template <typename T> struct BaseGCType<T, JS::TraceKind:: name> { typedef type_ type; };
JS_FOR_EACH_TRACEKIND(IMPL_BASE_GC_TYPE);
#undef IMPL_BASE_GC_TYPE

// Our barrier templates are parameterized on the pointer types so that we can
// share the definitions with Value and jsid. Thus, we need to strip the
// pointer before sending the type to BaseGCType and re-add it on the other
// side. As such:
template <typename T> struct PtrBaseGCType { typedef T type; };
template <typename T> struct PtrBaseGCType<T*> { typedef typename BaseGCType<T>::type* type; };

template <typename T>
typename PtrBaseGCType<T>::type*
ConvertToBase(T* thingp)
{
    return reinterpret_cast<typename PtrBaseGCType<T>::type*>(thingp);
}

template <typename T> void DispatchToTracer(JSTracer* trc, T* thingp, const char* name);
template <typename T> T DoCallback(JS::CallbackTracer* trc, T* thingp, const char* name);
template <typename T> void DoMarking(GCMarker* gcmarker, T* thing);
template <typename T> void DoMarking(GCMarker* gcmarker, const T& thing);
template <typename T> void NoteWeakEdge(GCMarker* gcmarker, T** thingp);
template <typename T> void NoteWeakEdge(GCMarker* gcmarker, T* thingp);

template <typename T>
void
js::TraceEdge(JSTracer* trc, WriteBarrieredBase<T>* thingp, const char* name)
{
    DispatchToTracer(trc, ConvertToBase(thingp->unsafeUnbarrieredForTracing()), name);
}

template <typename T>
void
js::TraceEdge(JSTracer* trc, ReadBarriered<T>* thingp, const char* name)
{
    DispatchToTracer(trc, ConvertToBase(thingp->unsafeGet()), name);
}

template <typename T>
void
js::TraceNullableEdge(JSTracer* trc, WriteBarrieredBase<T>* thingp, const char* name)
{
    if (InternalBarrierMethods<T>::isMarkable(thingp->get()))
        DispatchToTracer(trc, ConvertToBase(thingp->unsafeUnbarrieredForTracing()), name);
}

template <typename T>
void
js::TraceNullableEdge(JSTracer* trc, ReadBarriered<T>* thingp, const char* name)
{
    if (InternalBarrierMethods<T>::isMarkable(thingp->unbarrieredGet()))
        DispatchToTracer(trc, ConvertToBase(thingp->unsafeGet()), name);
}

template <typename T>
JS_PUBLIC_API(void)
js::gc::TraceExternalEdge(JSTracer* trc, T* thingp, const char* name)
{
    MOZ_ASSERT(InternalBarrierMethods<T>::isMarkable(*thingp));
    DispatchToTracer(trc, ConvertToBase(thingp), name);
}

template <typename T>
void
js::TraceManuallyBarrieredEdge(JSTracer* trc, T* thingp, const char* name)
{
    DispatchToTracer(trc, ConvertToBase(thingp), name);
}

template <typename T>
JS_PUBLIC_API(void)
js::UnsafeTraceManuallyBarrieredEdge(JSTracer* trc, T* thingp, const char* name)
{
    DispatchToTracer(trc, ConvertToBase(thingp), name);
}

template <typename T>
void
js::TraceWeakEdge(JSTracer* trc, WeakRef<T>* thingp, const char* name)
{
    if (!trc->isMarkingTracer()) {
        // Non-marking tracers can select whether or not they see weak edges.
        if (trc->traceWeakEdges())
            DispatchToTracer(trc, ConvertToBase(thingp->unsafeUnbarrieredForTracing()), name);
        return;
    }

    NoteWeakEdge(GCMarker::fromTracer(trc),
                 ConvertToBase(thingp->unsafeUnbarrieredForTracing()));
}

template <typename T>
void
js::TraceRoot(JSTracer* trc, T* thingp, const char* name)
{
    AssertRootMarkingPhase(trc);
    DispatchToTracer(trc, ConvertToBase(thingp), name);
}

template <typename T>
void
js::TraceRoot(JSTracer* trc, ReadBarriered<T>* thingp, const char* name)
{
    TraceRoot(trc, thingp->unsafeGet(), name);
}

template <typename T>
void
js::TraceNullableRoot(JSTracer* trc, T* thingp, const char* name)
{
    AssertRootMarkingPhase(trc);
    if (InternalBarrierMethods<T>::isMarkable(*thingp))
        DispatchToTracer(trc, ConvertToBase(thingp), name);
}

template <typename T>
void
js::TraceNullableRoot(JSTracer* trc, ReadBarriered<T>* thingp, const char* name)
{
    TraceNullableRoot(trc, thingp->unsafeGet(), name);
}

template <typename T>
JS_PUBLIC_API(void)
JS::UnsafeTraceRoot(JSTracer* trc, T* thingp, const char* name)
{
    MOZ_ASSERT(thingp);
    js::TraceNullableRoot(trc, thingp, name);
}

template <typename T>
void
js::TraceRange(JSTracer* trc, size_t len, WriteBarrieredBase<T>* vec, const char* name)
{
    JS::AutoTracingIndex index(trc);
    for (auto i : IntegerRange(len)) {
        if (InternalBarrierMethods<T>::isMarkable(vec[i].get()))
            DispatchToTracer(trc, ConvertToBase(vec[i].unsafeUnbarrieredForTracing()), name);
        ++index;
    }
}

template <typename T>
void
js::TraceRootRange(JSTracer* trc, size_t len, T* vec, const char* name)
{
    AssertRootMarkingPhase(trc);
    JS::AutoTracingIndex index(trc);
    for (auto i : IntegerRange(len)) {
        if (InternalBarrierMethods<T>::isMarkable(vec[i]))
            DispatchToTracer(trc, ConvertToBase(&vec[i]), name);
        ++index;
    }
}

// Instantiate a copy of the Tracing templates for each derived type.
#define INSTANTIATE_ALL_VALID_TRACE_FUNCTIONS(type) \
    template void js::TraceEdge<type>(JSTracer*, WriteBarrieredBase<type>*, const char*); \
    template void js::TraceEdge<type>(JSTracer*, ReadBarriered<type>*, const char*); \
    template void js::TraceNullableEdge<type>(JSTracer*, WriteBarrieredBase<type>*, const char*); \
    template void js::TraceNullableEdge<type>(JSTracer*, ReadBarriered<type>*, const char*); \
    template void js::TraceManuallyBarrieredEdge<type>(JSTracer*, type*, const char*); \
    template void js::TraceWeakEdge<type>(JSTracer*, WeakRef<type>*, const char*); \
    template void js::TraceRoot<type>(JSTracer*, type*, const char*); \
    template void js::TraceRoot<type>(JSTracer*, ReadBarriered<type>*, const char*); \
    template void js::TraceNullableRoot<type>(JSTracer*, type*, const char*); \
    template void js::TraceNullableRoot<type>(JSTracer*, ReadBarriered<type>*, const char*); \
    template void js::TraceRange<type>(JSTracer*, size_t, WriteBarrieredBase<type>*, const char*); \
    template void js::TraceRootRange<type>(JSTracer*, size_t, type*, const char*);
FOR_EACH_GC_POINTER_TYPE(INSTANTIATE_ALL_VALID_TRACE_FUNCTIONS)
#undef INSTANTIATE_ALL_VALID_TRACE_FUNCTIONS

#define INSTANTIATE_PUBLIC_TRACE_FUNCTIONS(type) \
    template JS_PUBLIC_API(void) JS::UnsafeTraceRoot<type>(JSTracer*, type*, const char*); \
    template JS_PUBLIC_API(void) js::UnsafeTraceManuallyBarrieredEdge<type>(JSTracer*, type*, \
                                                                            const char*); \
    template JS_PUBLIC_API(void) js::gc::TraceExternalEdge<type>(JSTracer*, type*, const char*);
FOR_EACH_PUBLIC_GC_POINTER_TYPE(INSTANTIATE_PUBLIC_TRACE_FUNCTIONS)
FOR_EACH_PUBLIC_TAGGED_GC_POINTER_TYPE(INSTANTIATE_PUBLIC_TRACE_FUNCTIONS)
#undef INSTANTIATE_PUBLIC_TRACE_FUNCTIONS

template <typename T>
void
js::TraceManuallyBarrieredCrossCompartmentEdge(JSTracer* trc, JSObject* src, T* dst,
                                               const char* name)
{
    if (ShouldTraceCrossCompartment(trc, src, *dst))
        DispatchToTracer(trc, dst, name);
}
template void js::TraceManuallyBarrieredCrossCompartmentEdge<JSObject*>(JSTracer*, JSObject*,
                                                                        JSObject**, const char*);
template void js::TraceManuallyBarrieredCrossCompartmentEdge<JSScript*>(JSTracer*, JSObject*,
                                                                        JSScript**, const char*);

template <typename T>
void
js::TraceCrossCompartmentEdge(JSTracer* trc, JSObject* src, WriteBarrieredBase<T>* dst,
                              const char* name)
{
    if (ShouldTraceCrossCompartment(trc, src, dst->get()))
        DispatchToTracer(trc, dst->unsafeUnbarrieredForTracing(), name);
}
template void js::TraceCrossCompartmentEdge<Value>(JSTracer*, JSObject*,
                                                   WriteBarrieredBase<Value>*, const char*);

template <typename T>
void
js::TraceProcessGlobalRoot(JSTracer* trc, T* thing, const char* name)
{
    AssertRootMarkingPhase(trc);
    MOZ_ASSERT(ThingIsPermanentAtomOrWellKnownSymbol(thing));

    // We have to mark permanent atoms and well-known symbols through a special
    // method because the default DoMarking implementation automatically skips
    // them. Fortunately, atoms (permanent and non) cannot refer to other GC
    // things so they do not need to go through the mark stack and may simply
    // be marked directly.  Moreover, well-known symbols can refer only to
    // permanent atoms, so likewise require no subsquent marking.
    CheckTracedThing(trc, *ConvertToBase(&thing));
    if (trc->isMarkingTracer())
        thing->asTenured().markIfUnmarked(gc::MarkColor::Black);
    else
        DoCallback(trc->asCallbackTracer(), ConvertToBase(&thing), name);
}
template void js::TraceProcessGlobalRoot<JSAtom>(JSTracer*, JSAtom*, const char*);
template void js::TraceProcessGlobalRoot<JS::Symbol>(JSTracer*, JS::Symbol*, const char*);

// A typed functor adaptor for TraceRoot.
struct TraceRootFunctor {
    template <typename T>
    void operator()(JSTracer* trc, Cell** thingp, const char* name) {
        TraceRoot(trc, reinterpret_cast<T**>(thingp), name);
    }
};

void
js::TraceGenericPointerRoot(JSTracer* trc, Cell** thingp, const char* name)
{
    MOZ_ASSERT(thingp);
    if (!*thingp)
        return;
    TraceRootFunctor f;
    DispatchTraceKindTyped(f, (*thingp)->getTraceKind(), trc, thingp, name);
}

// A typed functor adaptor for TraceManuallyBarrieredEdge.
struct TraceManuallyBarrieredEdgeFunctor {
    template <typename T>
    void operator()(JSTracer* trc, Cell** thingp, const char* name) {
        TraceManuallyBarrieredEdge(trc, reinterpret_cast<T**>(thingp), name);
    }
};

void
js::TraceManuallyBarrieredGenericPointerEdge(JSTracer* trc, Cell** thingp, const char* name)
{
    MOZ_ASSERT(thingp);
    if (!*thingp)
        return;
    TraceManuallyBarrieredEdgeFunctor f;
    DispatchTraceKindTyped(f, (*thingp)->getTraceKind(), trc, thingp, name);
}

// This method is responsible for dynamic dispatch to the real tracer
// implementation. Consider replacing this choke point with virtual dispatch:
// a sufficiently smart C++ compiler may be able to devirtualize some paths.
template <typename T>
void
DispatchToTracer(JSTracer* trc, T* thingp, const char* name)
{
#define IS_SAME_TYPE_OR(name, type, _) mozilla::IsSame<type*, T>::value ||
    static_assert(
            JS_FOR_EACH_TRACEKIND(IS_SAME_TYPE_OR)
            mozilla::IsSame<T, JS::Value>::value ||
            mozilla::IsSame<T, jsid>::value ||
            mozilla::IsSame<T, TaggedProto>::value,
            "Only the base cell layout types are allowed into marking/tracing internals");
#undef IS_SAME_TYPE_OR
    if (trc->isMarkingTracer())
        return DoMarking(GCMarker::fromTracer(trc), *thingp);
    if (trc->isTenuringTracer())
        return static_cast<TenuringTracer*>(trc)->traverse(thingp);
    MOZ_ASSERT(trc->isCallbackTracer());
    DoCallback(trc->asCallbackTracer(), thingp, name);
}


/*** GC Marking Interface *************************************************************************/

namespace js {

typedef bool HasNoImplicitEdgesType;

template <typename T>
struct ImplicitEdgeHolderType {
    typedef HasNoImplicitEdgesType Type;
};

// For now, we only handle JSObject* and JSScript* keys, but the linear time
// algorithm can be easily extended by adding in more types here, then making
// GCMarker::traverse<T> call markPotentialEphemeronKey.
template <>
struct ImplicitEdgeHolderType<JSObject*> {
    typedef JSObject* Type;
};

template <>
struct ImplicitEdgeHolderType<JSScript*> {
    typedef JSScript* Type;
};

void
GCMarker::markEphemeronValues(gc::Cell* markedCell, WeakEntryVector& values)
{
    size_t initialLen = values.length();
    for (size_t i = 0; i < initialLen; i++)
        values[i].weakmap->markEntry(this, markedCell, values[i].key);

    // The vector should not be appended to during iteration because the key is
    // already marked, and even in cases where we have a multipart key, we
    // should only be inserting entries for the unmarked portions.
    MOZ_ASSERT(values.length() == initialLen);
}

template <typename T>
void
GCMarker::markImplicitEdgesHelper(T markedThing)
{
    if (!isWeakMarkingTracer())
        return;

    Zone* zone = gc::TenuredCell::fromPointer(markedThing)->zone();
    MOZ_ASSERT(zone->isGCMarking());
    MOZ_ASSERT(!zone->isGCSweeping());

    auto p = zone->gcWeakKeys().get(JS::GCCellPtr(markedThing));
    if (!p)
        return;
    WeakEntryVector& markables = p->value;

    markEphemeronValues(markedThing, markables);
    markables.clear(); // If key address is reused, it should do nothing
}

template <>
void
GCMarker::markImplicitEdgesHelper(HasNoImplicitEdgesType)
{
}

template <typename T>
void
GCMarker::markImplicitEdges(T* thing)
{
    markImplicitEdgesHelper<typename ImplicitEdgeHolderType<T*>::Type>(thing);
}

} // namespace js

template <typename T>
static inline bool
ShouldMark(GCMarker* gcmarker, T thing)
{
    // Don't trace things that are owned by another runtime.
    if (IsOwnedByOtherRuntime(gcmarker->runtime(), thing))
        return false;

    // Don't mark things outside a zone if we are in a per-zone GC.
    return thing->zone()->shouldMarkInZone();
}

template <>
bool
ShouldMark<JSObject*>(GCMarker* gcmarker, JSObject* obj)
{
    // Don't trace things that are owned by another runtime.
    if (IsOwnedByOtherRuntime(gcmarker->runtime(), obj))
        return false;

    // We may mark a Nursery thing outside the context of the
    // MinorCollectionTracer because of a pre-barrier. The pre-barrier is not
    // needed in this case because we perform a minor collection before each
    // incremental slice.
    if (IsInsideNursery(obj))
        return false;

    // Don't mark things outside a zone if we are in a per-zone GC. It is
    // faster to check our own arena, which we can do since we know that
    // the object is tenured.
    return obj->asTenured().zone()->shouldMarkInZone();
}

// JSStrings can also be in the nursery. See ShouldMark<JSObject*> for comments.
template <>
bool
ShouldMark<JSString*>(GCMarker* gcmarker, JSString* str)
{
    if (IsOwnedByOtherRuntime(gcmarker->runtime(), str))
        return false;
    if (IsInsideNursery(str))
        return false;
    return str->asTenured().zone()->shouldMarkInZone();
}

template <typename T>
void
DoMarking(GCMarker* gcmarker, T* thing)
{
    // Do per-type marking precondition checks.
    if (!ShouldMark(gcmarker, thing))
        return;

    CheckTracedThing(gcmarker, thing);
    gcmarker->traverse(thing);

    // Mark the compartment as live.
    SetMaybeAliveFlag(thing);
}

template <typename S>
struct DoMarkingFunctor : public VoidDefaultAdaptor<S> {
    template <typename T> void operator()(T* t, GCMarker* gcmarker) { DoMarking(gcmarker, t); }
};

template <typename T>
void
DoMarking(GCMarker* gcmarker, const T& thing)
{
    DispatchTyped(DoMarkingFunctor<T>(), thing, gcmarker);
}

template <typename T>
void
NoteWeakEdge(GCMarker* gcmarker, T** thingp)
{
    // Do per-type marking precondition checks.
    if (!ShouldMark(gcmarker, *thingp))
        return;

    CheckTracedThing(gcmarker, *thingp);

    // If the target is already marked, there's no need to store the edge.
    if (IsMarkedUnbarriered(gcmarker->runtime(), thingp))
        return;

    gcmarker->noteWeakEdge(thingp);
}

template <typename T>
void
NoteWeakEdge(GCMarker* gcmarker, T* thingp)
{
    MOZ_CRASH("the gc does not support tagged pointers as weak edges");
}

template <typename T>
void
js::GCMarker::noteWeakEdge(T* edge)
{
    static_assert(IsBaseOf<Cell, typename mozilla::RemovePointer<T>::Type>::value,
                  "edge must point to a GC pointer");
    MOZ_ASSERT((*edge)->isTenured());

    // Note: we really want the *source* Zone here. The edge may start in a
    // non-gc heap location, however, so we use the fact that cross-zone weak
    // references are not allowed and use the *target's* zone.
    JS::Zone::WeakEdges &weakRefs = (*edge)->asTenured().zone()->gcWeakRefs();
    AutoEnterOOMUnsafeRegion oomUnsafe;
    if (!weakRefs.append(reinterpret_cast<TenuredCell**>(edge)))
        oomUnsafe.crash("Failed to record a weak edge for sweeping.");
}

// The simplest traversal calls out to the fully generic traceChildren function
// to visit the child edges. In the absence of other traversal mechanisms, this
// function will rapidly grow the stack past its bounds and crash the process.
// Thus, this generic tracing should only be used in cases where subsequent
// tracing will not recurse.
template <typename T>
void
js::GCMarker::markAndTraceChildren(T* thing)
{
    if (ThingIsPermanentAtomOrWellKnownSymbol(thing))
        return;
    if (mark(thing))
        thing->traceChildren(this);
}
namespace js {
template <> void GCMarker::traverse(BaseShape* thing) { markAndTraceChildren(thing); }
template <> void GCMarker::traverse(JS::Symbol* thing) { markAndTraceChildren(thing); }
template <> void GCMarker::traverse(RegExpShared* thing) { markAndTraceChildren(thing); }
} // namespace js

// Strings, LazyScripts, Shapes, and Scopes are extremely common, but have
// simple patterns of recursion. We traverse trees of these edges immediately,
// with aggressive, manual inlining, implemented by eagerlyTraceChildren.
template <typename T>
void
js::GCMarker::markAndScan(T* thing)
{
    if (ThingIsPermanentAtomOrWellKnownSymbol(thing))
        return;
    if (mark(thing))
        eagerlyMarkChildren(thing);
}
namespace js {
template <> void GCMarker::traverse(JSString* thing) { markAndScan(thing); }
template <> void GCMarker::traverse(LazyScript* thing) { markAndScan(thing); }
template <> void GCMarker::traverse(Shape* thing) { markAndScan(thing); }
template <> void GCMarker::traverse(js::Scope* thing) { markAndScan(thing); }
} // namespace js

// Object and ObjectGroup are extremely common and can contain arbitrarily
// nested graphs, so are not trivially inlined. In this case we use a mark
// stack to control recursion. JitCode shares none of these properties, but is
// included for historical reasons. JSScript normally cannot recurse, but may
// be used as a weakmap key and thereby recurse into weakmapped values.
template <typename T>
void
js::GCMarker::markAndPush(T* thing)
{
    if (!mark(thing))
        return;
    pushTaggedPtr(thing);
    markImplicitEdges(thing);
}
namespace js {
template <> void GCMarker::traverse(JSObject* thing) { markAndPush(thing); }
template <> void GCMarker::traverse(ObjectGroup* thing) { markAndPush(thing); }
template <> void GCMarker::traverse(jit::JitCode* thing) { markAndPush(thing); }
template <> void GCMarker::traverse(JSScript* thing) { markAndPush(thing); }
} // namespace js

namespace js {
template <>
void
GCMarker::traverse(AccessorShape* thing) {
    MOZ_CRASH("AccessorShape must be marked as a Shape");
}
} // namespace js

template <typename S, typename T>
static void
CheckTraversedEdge(S source, T* target)
{
    // Atoms and Symbols do not have or mark their internal pointers, respectively.
    MOZ_ASSERT(!ThingIsPermanentAtomOrWellKnownSymbol(source));

    // The Zones must match, unless the target is an atom.
    MOZ_ASSERT_IF(!ThingIsPermanentAtomOrWellKnownSymbol(target),
                  target->zone()->isAtomsZone() || target->zone() == source->zone());

    // If we are marking an atom, that atom must be marked in the source zone's
    // atom bitmap.
    MOZ_ASSERT_IF(!ThingIsPermanentAtomOrWellKnownSymbol(target) &&
                  target->zone()->isAtomsZone() && !source->zone()->isAtomsZone(),
                  target->runtimeFromAnyThread()->gc.atomMarking
                      .atomIsMarked(source->zone(), reinterpret_cast<TenuredCell*>(target)));

    // Atoms and Symbols do not have access to a compartment pointer, or we'd need
    // to adjust the subsequent check to catch that case.
    MOZ_ASSERT_IF(ThingIsPermanentAtomOrWellKnownSymbol(target), !target->maybeCompartment());
    MOZ_ASSERT_IF(target->zoneFromAnyThread()->isAtomsZone(), !target->maybeCompartment());
    // If we have access to a compartment pointer for both things, they must match.
    MOZ_ASSERT_IF(source->maybeCompartment() && target->maybeCompartment(),
                  source->maybeCompartment() == target->maybeCompartment());
}

template <typename S, typename T>
void
js::GCMarker::traverseEdge(S source, T* target)
{
    CheckTraversedEdge(source, target);
    traverse(target);
}

template <typename V, typename S> struct TraverseEdgeFunctor : public VoidDefaultAdaptor<V> {
    template <typename T> void operator()(T t, GCMarker* gcmarker, S s) {
        return gcmarker->traverseEdge(s, t);
    }
};

template <typename S, typename T>
void
js::GCMarker::traverseEdge(S source, const T& thing)
{
    DispatchTyped(TraverseEdgeFunctor<T, S>(), thing, this, source);
}

namespace {

template <typename T> struct TypeParticipatesInCC {};
#define EXPAND_PARTICIPATES_IN_CC(_, type, addToCCKind) \
    template <> struct TypeParticipatesInCC<type> { static const bool value = addToCCKind; };
JS_FOR_EACH_TRACEKIND(EXPAND_PARTICIPATES_IN_CC)
#undef EXPAND_PARTICIPATES_IN_CC

struct ParticipatesInCCFunctor
{
    template <typename T>
    bool operator()() {
        return TypeParticipatesInCC<T>::value;
    }
};

} // namespace

static bool
TraceKindParticipatesInCC(JS::TraceKind kind)
{
    return DispatchTraceKindTyped(ParticipatesInCCFunctor(), kind);
}

template <typename T>
bool
js::GCMarker::mark(T* thing)
{
    AssertShouldMarkInZone(thing);
    TenuredCell* cell = TenuredCell::fromPointer(thing);
    MOZ_ASSERT(!IsInsideNursery(cell));

    if (!TypeParticipatesInCC<T>::value)
        return cell->markIfUnmarked(MarkColor::Black);

    return cell->markIfUnmarked(markColor());
}


/*** Inline, Eager GC Marking *********************************************************************/

// Each of the eager, inline marking paths is directly preceeded by the
// out-of-line, generic tracing code for comparison. Both paths must end up
// traversing equivalent subgraphs.

void
LazyScript::traceChildren(JSTracer* trc)
{
    if (script_)
        TraceWeakEdge(trc, &script_, "script");

    if (function_)
        TraceEdge(trc, &function_, "function");

    if (sourceObject_)
        TraceEdge(trc, &sourceObject_, "sourceObject");

    if (enclosingScope_)
        TraceEdge(trc, &enclosingScope_, "enclosingScope");

    // We rely on the fact that atoms are always tenured.
    JSAtom** closedOverBindings = this->closedOverBindings();
    for (auto i : IntegerRange(numClosedOverBindings())) {
        if (closedOverBindings[i])
            TraceManuallyBarrieredEdge(trc, &closedOverBindings[i], "closedOverBinding");
    }

    GCPtrFunction* innerFunctions = this->innerFunctions();
    for (auto i : IntegerRange(numInnerFunctions()))
        TraceEdge(trc, &innerFunctions[i], "lazyScriptInnerFunction");
}
inline void
js::GCMarker::eagerlyMarkChildren(LazyScript *thing)
{
    if (thing->script_)
        noteWeakEdge(thing->script_.unsafeUnbarrieredForTracing());

    if (thing->function_)
        traverseEdge(thing, static_cast<JSObject*>(thing->function_));

    if (thing->sourceObject_)
        traverseEdge(thing, static_cast<JSObject*>(thing->sourceObject_));

    if (thing->enclosingScope_)
        traverseEdge(thing, static_cast<Scope*>(thing->enclosingScope_));

    // We rely on the fact that atoms are always tenured.
    JSAtom** closedOverBindings = thing->closedOverBindings();
    for (auto i : IntegerRange(thing->numClosedOverBindings())) {
        if (closedOverBindings[i])
            traverseEdge(thing, static_cast<JSString*>(closedOverBindings[i]));
    }

    GCPtrFunction* innerFunctions = thing->innerFunctions();
    for (auto i : IntegerRange(thing->numInnerFunctions()))
        traverseEdge(thing, static_cast<JSObject*>(innerFunctions[i]));
}

void
Shape::traceChildren(JSTracer* trc)
{
    TraceEdge(trc, &base_, "base");
    TraceEdge(trc, &propidRef(), "propid");
    if (parent)
        TraceEdge(trc, &parent, "parent");

    if (hasGetterObject())
        TraceManuallyBarrieredEdge(trc, &asAccessorShape().getterObj, "getter");
    if (hasSetterObject())
        TraceManuallyBarrieredEdge(trc, &asAccessorShape().setterObj, "setter");
}
inline void
js::GCMarker::eagerlyMarkChildren(Shape* shape)
{
    MOZ_ASSERT_IF(markColor() == MarkColor::Gray, shape->isMarkedGray());
    MOZ_ASSERT_IF(markColor() == MarkColor::Black, shape->isMarkedBlack());

    do {
        // Special case: if a base shape has a shape table then all its pointers
        // must point to this shape or an anscestor.  Since these pointers will
        // be traced by this loop they do not need to be traced here as well.
        BaseShape* base = shape->base();
        CheckTraversedEdge(shape, base);
        if (mark(base)) {
            MOZ_ASSERT(base->canSkipMarkingShapeTable(shape));
            base->traceChildrenSkipShapeTable(this);
        }

        traverseEdge(shape, shape->propidRef().get());

        // When triggered between slices on belhalf of a barrier, these
        // objects may reside in the nursery, so require an extra check.
        // FIXME: Bug 1157967 - remove the isTenured checks.
        if (shape->hasGetterObject() && shape->getterObject()->isTenured())
            traverseEdge(shape, shape->getterObject());
        if (shape->hasSetterObject() && shape->setterObject()->isTenured())
            traverseEdge(shape, shape->setterObject());

        shape = shape->previous();
    } while (shape && mark(shape));
}

void
JSString::traceChildren(JSTracer* trc)
{
    if (hasBase())
        traceBase(trc);
    else if (isRope())
        asRope().traceChildren(trc);
}
inline void
GCMarker::eagerlyMarkChildren(JSString* str)
{
    if (str->isLinear())
        eagerlyMarkChildren(&str->asLinear());
    else
        eagerlyMarkChildren(&str->asRope());
}

void
JSString::traceBase(JSTracer* trc)
{
    MOZ_ASSERT(hasBase());
    TraceManuallyBarrieredEdge(trc, &d.s.u3.base, "base");
}
inline void
js::GCMarker::eagerlyMarkChildren(JSLinearString* linearStr)
{
    AssertShouldMarkInZone(linearStr);
    MOZ_ASSERT(linearStr->isMarkedAny());
    MOZ_ASSERT(linearStr->JSString::isLinear());

    // Use iterative marking to avoid blowing out the stack.
    while (linearStr->hasBase()) {
        linearStr = linearStr->base();
        MOZ_ASSERT(linearStr->JSString::isLinear());
        if (linearStr->isPermanentAtom())
            break;
        AssertShouldMarkInZone(linearStr);
        if (!mark(static_cast<JSString*>(linearStr)))
            break;
    }
}

void
JSRope::traceChildren(JSTracer* trc) {
    js::TraceManuallyBarrieredEdge(trc, &d.s.u2.left, "left child");
    js::TraceManuallyBarrieredEdge(trc, &d.s.u3.right, "right child");
}
inline void
js::GCMarker::eagerlyMarkChildren(JSRope* rope)
{
    // This function tries to scan the whole rope tree using the marking stack
    // as temporary storage. If that becomes full, the unscanned ropes are
    // added to the delayed marking list. When the function returns, the
    // marking stack is at the same depth as it was on entry. This way we avoid
    // using tags when pushing ropes to the stack as ropes never leak to other
    // users of the stack. This also assumes that a rope can only point to
    // other ropes or linear strings, it cannot refer to GC things of other
    // types.
    size_t savedPos = stack.position();
    JS_DIAGNOSTICS_ASSERT(rope->getTraceKind() == JS::TraceKind::String);
#ifdef JS_DEBUG
    static const size_t DEEP_ROPE_THRESHOLD = 100000;
    static const size_t ROPE_CYCLE_HISTORY = 100;
    DebugOnly<size_t> ropeDepth = 0;
    JSRope* history[ROPE_CYCLE_HISTORY];
#endif
    while (true) {
#ifdef JS_DEBUG
        if (++ropeDepth >= DEEP_ROPE_THRESHOLD) {
            // Bug 1011786 comment 294 - detect cyclic ropes. There are some
            // legitimate deep ropes, at least in tests. So if we hit a deep
            // rope, start recording the nodes we visit and check whether we
            // repeat. But do it on a finite window size W so that we're not
            // scanning the full history for every node. And only check every
            // Wth push, to add only constant overhead per node. This will only
            // catch cycles of size up to W (but it seems most likely that any
            // cycles will be size 1 or maybe 2.)
            if ((ropeDepth > DEEP_ROPE_THRESHOLD + ROPE_CYCLE_HISTORY) &&
                (ropeDepth % ROPE_CYCLE_HISTORY) == 0)
            {
                for (size_t i = 0; i < ROPE_CYCLE_HISTORY; i++)
                    MOZ_ASSERT(history[i] != rope, "cycle detected in rope");
            }
            history[ropeDepth % ROPE_CYCLE_HISTORY] = rope;
        }
#endif

        JS_DIAGNOSTICS_ASSERT(rope->getTraceKind() == JS::TraceKind::String);
        JS_DIAGNOSTICS_ASSERT(rope->JSString::isRope());
        AssertShouldMarkInZone(rope);
        MOZ_ASSERT(rope->isMarkedAny());
        JSRope* next = nullptr;

        JSString* right = rope->rightChild();
        if (!right->isPermanentAtom() &&
            mark(right))
        {
            if (right->isLinear())
                eagerlyMarkChildren(&right->asLinear());
            else
                next = &right->asRope();
        }

        JSString* left = rope->leftChild();
        if (!left->isPermanentAtom() &&
            mark(left))
        {
            if (left->isLinear()) {
                eagerlyMarkChildren(&left->asLinear());
            } else {
                // When both children are ropes, set aside the right one to
                // scan it later.
                if (next && !stack.pushTempRope(next))
                    delayMarkingChildren(next);
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

static inline void
TraceBindingNames(JSTracer* trc, BindingName* names, uint32_t length)
{
    for (uint32_t i = 0; i < length; i++) {
        JSAtom* name = names[i].name();
        MOZ_ASSERT(name);
        TraceManuallyBarrieredEdge(trc, &name, "scope name");
    }
};
static inline void
TraceNullableBindingNames(JSTracer* trc, BindingName* names, uint32_t length)
{
    for (uint32_t i = 0; i < length; i++) {
        if (JSAtom* name = names[i].name())
            TraceManuallyBarrieredEdge(trc, &name, "scope name");
    }
};
void
BindingName::trace(JSTracer* trc)
{
    if (JSAtom* atom = name())
        TraceManuallyBarrieredEdge(trc, &atom, "binding name");
}
void
BindingIter::trace(JSTracer* trc)
{
    TraceNullableBindingNames(trc, names_, length_);
}
void
LexicalScope::Data::trace(JSTracer* trc)
{
    TraceBindingNames(trc, names, length);
}
void
FunctionScope::Data::trace(JSTracer* trc)
{
    TraceNullableEdge(trc, &canonicalFunction, "scope canonical function");
    TraceNullableBindingNames(trc, names, length);
}
void
VarScope::Data::trace(JSTracer* trc)
{
    TraceBindingNames(trc, names, length);
}
void
GlobalScope::Data::trace(JSTracer* trc)
{
    TraceBindingNames(trc, names, length);
}
void
EvalScope::Data::trace(JSTracer* trc)
{
    TraceBindingNames(trc, names, length);
}
void
ModuleScope::Data::trace(JSTracer* trc)
{
    TraceNullableEdge(trc, &module, "scope module");
    TraceBindingNames(trc, names, length);
}
void
WasmInstanceScope::Data::trace(JSTracer* trc)
{
    TraceNullableEdge(trc, &instance, "wasm instance");
    TraceBindingNames(trc, names, length);
}
void
WasmFunctionScope::Data::trace(JSTracer* trc)
{
    TraceBindingNames(trc, names, length);
}
void
Scope::traceChildren(JSTracer* trc)
{
    TraceNullableEdge(trc, &enclosing_, "scope enclosing");
    TraceNullableEdge(trc, &environmentShape_, "scope env shape");
    switch (kind_) {
      case ScopeKind::Function:
        reinterpret_cast<FunctionScope::Data*>(data_)->trace(trc);
        break;
      case ScopeKind::FunctionBodyVar:
      case ScopeKind::ParameterExpressionVar:
        reinterpret_cast<VarScope::Data*>(data_)->trace(trc);
        break;
      case ScopeKind::Lexical:
      case ScopeKind::SimpleCatch:
      case ScopeKind::Catch:
      case ScopeKind::NamedLambda:
      case ScopeKind::StrictNamedLambda:
        reinterpret_cast<LexicalScope::Data*>(data_)->trace(trc);
        break;
      case ScopeKind::Global:
      case ScopeKind::NonSyntactic:
        reinterpret_cast<GlobalScope::Data*>(data_)->trace(trc);
        break;
      case ScopeKind::Eval:
      case ScopeKind::StrictEval:
        reinterpret_cast<EvalScope::Data*>(data_)->trace(trc);
        break;
      case ScopeKind::Module:
        reinterpret_cast<ModuleScope::Data*>(data_)->trace(trc);
        break;
      case ScopeKind::With:
        break;
      case ScopeKind::WasmInstance:
        reinterpret_cast<WasmInstanceScope::Data*>(data_)->trace(trc);
        break;
      case ScopeKind::WasmFunction:
        reinterpret_cast<WasmFunctionScope::Data*>(data_)->trace(trc);
        break;
    }
}
inline void
js::GCMarker::eagerlyMarkChildren(Scope* scope)
{
    if (scope->enclosing_)
        traverseEdge(scope, static_cast<Scope*>(scope->enclosing_));
    if (scope->environmentShape_)
        traverseEdge(scope, static_cast<Shape*>(scope->environmentShape_));
    BindingName* names = nullptr;
    uint32_t length = 0;
    switch (scope->kind_) {
      case ScopeKind::Function: {
        FunctionScope::Data* data = reinterpret_cast<FunctionScope::Data*>(scope->data_);
        traverseEdge(scope, static_cast<JSObject*>(data->canonicalFunction));
        names = data->names;
        length = data->length;
        break;
      }

      case ScopeKind::FunctionBodyVar:
      case ScopeKind::ParameterExpressionVar: {
        VarScope::Data* data = reinterpret_cast<VarScope::Data*>(scope->data_);
        names = data->names;
        length = data->length;
        break;
      }

      case ScopeKind::Lexical:
      case ScopeKind::SimpleCatch:
      case ScopeKind::Catch:
      case ScopeKind::NamedLambda:
      case ScopeKind::StrictNamedLambda: {
        LexicalScope::Data* data = reinterpret_cast<LexicalScope::Data*>(scope->data_);
        names = data->names;
        length = data->length;
        break;
      }

      case ScopeKind::Global:
      case ScopeKind::NonSyntactic: {
        GlobalScope::Data* data = reinterpret_cast<GlobalScope::Data*>(scope->data_);
        names = data->names;
        length = data->length;
        break;
      }

      case ScopeKind::Eval:
      case ScopeKind::StrictEval: {
        EvalScope::Data* data = reinterpret_cast<EvalScope::Data*>(scope->data_);
        names = data->names;
        length = data->length;
        break;
      }

      case ScopeKind::Module: {
        ModuleScope::Data* data = reinterpret_cast<ModuleScope::Data*>(scope->data_);
        traverseEdge(scope, static_cast<JSObject*>(data->module));
        names = data->names;
        length = data->length;
        break;
      }

      case ScopeKind::With:
        break;

      case ScopeKind::WasmInstance: {
        WasmInstanceScope::Data* data = reinterpret_cast<WasmInstanceScope::Data*>(scope->data_);
        traverseEdge(scope, static_cast<JSObject*>(data->instance));
        names = data->names;
        length = data->length;
        break;
      }

      case ScopeKind::WasmFunction: {
        WasmFunctionScope::Data* data = reinterpret_cast<WasmFunctionScope::Data*>(scope->data_);
        names = data->names;
        length = data->length;
        break;
      }
    }
    if (scope->kind_ == ScopeKind::Function) {
        for (uint32_t i = 0; i < length; i++) {
            if (JSAtom* name = names[i].name())
                traverseEdge(scope, static_cast<JSString*>(name));
        }
    } else {
        for (uint32_t i = 0; i < length; i++)
            traverseEdge(scope, static_cast<JSString*>(names[i].name()));
    }
}

void
js::ObjectGroup::traceChildren(JSTracer* trc)
{
    unsigned count = getPropertyCount();
    for (unsigned i = 0; i < count; i++) {
        if (ObjectGroup::Property* prop = getProperty(i))
            TraceEdge(trc, &prop->id, "group_property");
    }

    if (proto().isObject())
        TraceEdge(trc, &proto(), "group_proto");

    if (trc->isMarkingTracer())
        compartment()->mark();

    if (JSObject* global = compartment()->unsafeUnbarrieredMaybeGlobal())
        TraceManuallyBarrieredEdge(trc, &global, "group_global");


    if (newScript())
        newScript()->trace(trc);

    if (maybePreliminaryObjects())
        maybePreliminaryObjects()->trace(trc);

    if (maybeUnboxedLayout())
        unboxedLayout().trace(trc);

    if (ObjectGroup* unboxedGroup = maybeOriginalUnboxedGroup()) {
        TraceManuallyBarrieredEdge(trc, &unboxedGroup, "group_original_unboxed_group");
        setOriginalUnboxedGroup(unboxedGroup);
    }

    if (JSObject* descr = maybeTypeDescr()) {
        TraceManuallyBarrieredEdge(trc, &descr, "group_type_descr");
        setTypeDescr(&descr->as<TypeDescr>());
    }

    if (JSObject* fun = maybeInterpretedFunction()) {
        TraceManuallyBarrieredEdge(trc, &fun, "group_function");
        setInterpretedFunction(&fun->as<JSFunction>());
    }
}
void
js::GCMarker::lazilyMarkChildren(ObjectGroup* group)
{
    unsigned count = group->getPropertyCount();
    for (unsigned i = 0; i < count; i++) {
        if (ObjectGroup::Property* prop = group->getProperty(i))
            traverseEdge(group, prop->id.get());
    }

    if (group->proto().isObject())
        traverseEdge(group, group->proto().toObject());

    group->compartment()->mark();

    if (GlobalObject* global = group->compartment()->unsafeUnbarrieredMaybeGlobal())
        traverseEdge(group, static_cast<JSObject*>(global));

    if (group->newScript())
        group->newScript()->trace(this);

    if (group->maybePreliminaryObjects())
        group->maybePreliminaryObjects()->trace(this);

    if (group->maybeUnboxedLayout())
        group->unboxedLayout().trace(this);

    if (ObjectGroup* unboxedGroup = group->maybeOriginalUnboxedGroup())
        traverseEdge(group, unboxedGroup);

    if (TypeDescr* descr = group->maybeTypeDescr())
        traverseEdge(group, static_cast<JSObject*>(descr));

    if (JSFunction* fun = group->maybeInterpretedFunction())
        traverseEdge(group, static_cast<JSObject*>(fun));
}

struct TraverseObjectFunctor
{
    template <typename T>
    void operator()(T* thing, GCMarker* gcmarker, JSObject* src) {
        gcmarker->traverseEdge(src, *thing);
    }
};

// Call the trace hook set on the object, if present. If further tracing of
// NativeObject fields is required, this will return the native object.
enum class CheckGeneration { DoChecks, NoChecks};
template <typename Functor, typename... Args>
static inline NativeObject*
CallTraceHook(Functor f, JSTracer* trc, JSObject* obj, CheckGeneration check, Args&&... args)
{
    const Class* clasp = obj->getClass();
    MOZ_ASSERT(clasp);
    MOZ_ASSERT(obj->isNative() == clasp->isNative());

    if (!clasp->hasTrace())
        return &obj->as<NativeObject>();

    if (clasp->isTrace(InlineTypedObject::obj_trace)) {
        Shape** pshape = obj->as<InlineTypedObject>().addressOfShapeFromGC();
        f(pshape, mozilla::Forward<Args>(args)...);

        InlineTypedObject& tobj = obj->as<InlineTypedObject>();
        if (tobj.typeDescr().hasTraceList()) {
            VisitTraceList(f, tobj.typeDescr().traceList(), tobj.inlineTypedMemForGC(),
                           mozilla::Forward<Args>(args)...);
        }

        return nullptr;
    }

    if (clasp == &UnboxedPlainObject::class_) {
        JSObject** pexpando = obj->as<UnboxedPlainObject>().addressOfExpando();
        if (*pexpando)
            f(pexpando, mozilla::Forward<Args>(args)...);

        UnboxedPlainObject& unboxed = obj->as<UnboxedPlainObject>();
        const UnboxedLayout& layout = check == CheckGeneration::DoChecks
                                      ? unboxed.layout()
                                      : unboxed.layoutDontCheckGeneration();
        if (layout.traceList()) {
            VisitTraceList(f, layout.traceList(), unboxed.data(),
                           mozilla::Forward<Args>(args)...);
        }

        return nullptr;
    }

    clasp->doTrace(trc, obj);

    if (!clasp->isNative())
        return nullptr;
    return &obj->as<NativeObject>();
}

template <typename F, typename... Args>
static void
VisitTraceList(F f, const int32_t* traceList, uint8_t* memory, Args&&... args)
{
    while (*traceList != -1) {
        f(reinterpret_cast<JSString**>(memory + *traceList), mozilla::Forward<Args>(args)...);
        traceList++;
    }
    traceList++;
    while (*traceList != -1) {
        JSObject** objp = reinterpret_cast<JSObject**>(memory + *traceList);
        if (*objp)
            f(objp, mozilla::Forward<Args>(args)...);
        traceList++;
    }
    traceList++;
    while (*traceList != -1) {
        f(reinterpret_cast<Value*>(memory + *traceList), mozilla::Forward<Args>(args)...);
        traceList++;
    }
}


/*** Mark-stack Marking ***************************************************************************/

bool
GCMarker::drainMarkStack(SliceBudget& budget)
{
#ifdef DEBUG
    MOZ_ASSERT(!strictCompartmentChecking);
    strictCompartmentChecking = true;
    auto acc = mozilla::MakeScopeExit([&] {strictCompartmentChecking = false;});
#endif

    if (budget.isOverBudget())
        return false;

    for (;;) {
        while (!stack.isEmpty()) {
            processMarkStackTop(budget);
            if (budget.isOverBudget()) {
                saveValueRanges();
                return false;
            }
        }

        if (!hasDelayedChildren())
            break;

        /*
         * Mark children of things that caused too deep recursion during the
         * above tracing. Don't do this until we're done with everything
         * else.
         */
        if (!markDelayedChildren(budget)) {
            saveValueRanges();
            return false;
        }
    }

    return true;
}

inline static bool
ObjectDenseElementsMayBeMarkable(NativeObject* nobj)
{
    /*
     * For arrays that are large enough it's worth checking the type information
     * to see if the object's elements contain any GC pointers.  If not, we
     * don't need to trace them.
     */
    const unsigned MinElementsLength = 32;
    if (nobj->getDenseInitializedLength() < MinElementsLength || nobj->isSingleton())
        return true;

    ObjectGroup* group = nobj->group();
    if (group->needsSweep() || group->unknownProperties())
        return true;

    HeapTypeSet* typeSet = group->maybeGetProperty(JSID_VOID);
    if (!typeSet)
        return true;

    static const uint32_t flagMask =
        TYPE_FLAG_STRING | TYPE_FLAG_SYMBOL | TYPE_FLAG_LAZYARGS | TYPE_FLAG_ANYOBJECT;
    bool mayBeMarkable = typeSet->hasAnyFlag(flagMask) || typeSet->getObjectCount() != 0;

#ifdef DEBUG
    if (!mayBeMarkable) {
        const Value* elements = nobj->getDenseElementsAllowCopyOnWrite();
        for (unsigned i = 0; i < nobj->getDenseInitializedLength(); i++)
            MOZ_ASSERT(!elements[i].isGCThing());
    }
#endif

    return mayBeMarkable;
}

static inline void
CheckForCompartmentMismatch(JSObject* obj, JSObject* obj2)
{
#ifdef DEBUG
    if (MOZ_UNLIKELY(obj->compartment() != obj2->compartment())) {
        fprintf(stderr, "Compartment mismatch in pointer from %s object slot to %s object\n",
                obj->getClass()->name, obj2->getClass()->name);
        MOZ_CRASH("Compartment mismatch");
    }
#endif
}

inline void
GCMarker::processMarkStackTop(SliceBudget& budget)
{
    /*
     * The function uses explicit goto and implements the scanning of the
     * object directly. It allows to eliminate the tail recursion and
     * significantly improve the marking performance, see bug 641025.
     */
    HeapSlot* vp;
    HeapSlot* end;
    JSObject* obj;

    switch (stack.peekTag()) {
      case MarkStack::ValueArrayTag: {
        auto array = stack.popValueArray();
        obj = array.ptr.asValueArrayObject();
        vp = array.start;
        end = array.end;
        goto scan_value_array;
      }

      case MarkStack::ObjectTag: {
        obj = stack.popPtr().as<JSObject>();
        AssertShouldMarkInZone(obj);
        goto scan_obj;
      }

      case MarkStack::GroupTag: {
        auto group = stack.popPtr().as<ObjectGroup>();
        return lazilyMarkChildren(group);
      }

      case MarkStack::JitCodeTag: {
        auto code = stack.popPtr().as<jit::JitCode>();
        return code->traceChildren(this);
      }

      case MarkStack::ScriptTag: {
        auto script = stack.popPtr().as<JSScript>();
        return script->traceChildren(this);
      }

      case MarkStack::SavedValueArrayTag: {
        auto savedArray = stack.popSavedValueArray();
        JSObject* obj = savedArray.ptr.asSavedValueArrayObject();
        if (restoreValueArray(savedArray, &vp, &end))
            pushValueArray(obj, vp, end);
        else
            repush(obj);
        return;
      }

      default: MOZ_CRASH("Invalid tag in mark stack");
    }
    return;

  scan_value_array:
    MOZ_ASSERT(vp <= end);
    while (vp != end) {
        budget.step();
        if (budget.isOverBudget()) {
            pushValueArray(obj, vp, end);
            return;
        }

        const Value& v = *vp++;
        if (v.isString()) {
            traverseEdge(obj, v.toString());
        } else if (v.isObject()) {
            JSObject* obj2 = &v.toObject();
#ifdef DEBUG
            if (!obj2) {
                fprintf(stderr,
                        "processMarkStackTop found ObjectValue(nullptr) "
                        "at %zu Values from end of array in object:\n",
                        size_t(end - (vp - 1)));
                DumpObject(obj);
            }
#endif
            CheckForCompartmentMismatch(obj, obj2);
            if (mark(obj2)) {
                // Save the rest of this value array for later and start scanning obj2's children.
                pushValueArray(obj, vp, end);
                obj = obj2;
                goto scan_obj;
            }
        } else if (v.isSymbol()) {
            traverseEdge(obj, v.toSymbol());
        } else if (v.isPrivateGCThing()) {
            // v.toGCCellPtr cannot be inlined, so construct one manually.
            Cell* cell = v.toGCThing();
            traverseEdge(obj, JS::GCCellPtr(cell, cell->getTraceKind()));
        }
    }
    return;

  scan_obj:
    {
        AssertShouldMarkInZone(obj);

        budget.step();
        if (budget.isOverBudget()) {
            repush(obj);
            return;
        }

        markImplicitEdges(obj);
        ObjectGroup* group = obj->groupFromGC();
        traverseEdge(obj, group);

        NativeObject *nobj = CallTraceHook(TraverseObjectFunctor(), this, obj,
                                           CheckGeneration::DoChecks, this, obj);
        if (!nobj)
            return;

        Shape* shape = nobj->lastProperty();
        traverseEdge(obj, shape);

        unsigned nslots = nobj->slotSpan();

        do {
            if (nobj->hasEmptyElements())
                break;

            if (nobj->denseElementsAreCopyOnWrite()) {
                JSObject* owner = nobj->getElementsHeader()->ownerObject();
                if (owner != nobj) {
                    traverseEdge(obj, owner);
                    break;
                }
            }

            if (!ObjectDenseElementsMayBeMarkable(nobj))
                break;

            vp = nobj->getDenseElementsAllowCopyOnWrite();
            end = vp + nobj->getDenseInitializedLength();

            if (!nslots)
                goto scan_value_array;
            pushValueArray(nobj, vp, end);
        } while (false);

        vp = nobj->fixedSlots();
        if (nobj->slots_) {
            unsigned nfixed = nobj->numFixedSlots();
            if (nslots > nfixed) {
                pushValueArray(nobj, vp, vp + nfixed);
                vp = nobj->slots_;
                end = vp + (nslots - nfixed);
                goto scan_value_array;
            }
        }
        MOZ_ASSERT(nslots <= nobj->numFixedSlots());
        end = vp + nslots;
        goto scan_value_array;
    }
}

/*
 * During incremental GC, we return from drainMarkStack without having processed
 * the entire stack. At that point, JS code can run and reallocate slot arrays
 * that are stored on the stack. To prevent this from happening, we replace all
 * ValueArrayTag stack items with SavedValueArrayTag. In the latter, slots
 * pointers are replaced with slot indexes, and slot array end pointers are
 * replaced with the kind of index (properties vs. elements).
 */
void
GCMarker::saveValueRanges()
{
    MarkStackIter iter(stack);
    while (!iter.done()) {
        auto tag = iter.peekTag();
        if (tag == MarkStack::ValueArrayTag) {
            auto array = iter.peekValueArray();

            NativeObject* obj = &array.ptr.asValueArrayObject()->as<NativeObject>();
            MOZ_ASSERT(obj->isNative());

            uintptr_t index;
            HeapSlot::Kind kind;
            HeapSlot* vp = obj->getDenseElementsAllowCopyOnWrite();
            if (array.end == vp + obj->getDenseInitializedLength()) {
                MOZ_ASSERT(array.start >= vp);
                // Add the number of shifted elements here (and subtract in
                // restoreValueArray) to ensure shift() calls on the array
                // are handled correctly.
                index = obj->unshiftedIndex(array.start - vp);
                kind = HeapSlot::Element;
            } else {
                HeapSlot* vp = obj->fixedSlots();
                unsigned nfixed = obj->numFixedSlots();
                if (array.start == array.end) {
                    index = obj->slotSpan();
                } else if (array.start >= vp && array.start < vp + nfixed) {
                    MOZ_ASSERT(array.end == vp + Min(nfixed, obj->slotSpan()));
                    index = array.start - vp;
                } else {
                    MOZ_ASSERT(array.start >= obj->slots_ &&
                               array.end == obj->slots_ + obj->slotSpan() - nfixed);
                    index = (array.start - obj->slots_) + nfixed;
                }
                kind = HeapSlot::Slot;
            }
            iter.saveValueArray(obj, index, kind);
            iter.nextArray();
        } else if (tag == MarkStack::SavedValueArrayTag) {
            iter.nextArray();
        } else {
            iter.nextPtr();
        }
    }
}

bool
GCMarker::restoreValueArray(const MarkStack::SavedValueArray& array,
                            HeapSlot** vpp, HeapSlot** endp)
{
    JSObject* objArg = array.ptr.asSavedValueArrayObject();
    if (!objArg->isNative())
        return false;
    NativeObject* obj = &objArg->as<NativeObject>();

    uintptr_t start = array.index;
    if (array.kind == HeapSlot::Element) {
        uint32_t initlen = obj->getDenseInitializedLength();

        // Account for shifted elements.
        uint32_t numShifted = obj->getElementsHeader()->numShiftedElements();
        start = (numShifted < start) ? start - numShifted : 0;

        HeapSlot* vp = obj->getDenseElementsAllowCopyOnWrite();
        if (start < initlen) {
            *vpp = vp + start;
            *endp = vp + initlen;
        } else {
            /* The object shrunk, in which case no scanning is needed. */
            *vpp = *endp = vp;
        }
    } else {
        MOZ_ASSERT(array.kind == HeapSlot::Slot);
        HeapSlot* vp = obj->fixedSlots();
        unsigned nfixed = obj->numFixedSlots();
        unsigned nslots = obj->slotSpan();
        if (start < nslots) {
            if (start < nfixed) {
                *vpp = vp + start;
                *endp = vp + Min(nfixed, nslots);
            } else {
                *vpp = obj->slots_ + start - nfixed;
                *endp = obj->slots_ + nslots - nfixed;
            }
        } else {
            /* The object shrunk, in which case no scanning is needed. */
            *vpp = *endp = vp;
        }
    }

    MOZ_ASSERT(*vpp <= *endp);
    return true;
}


/*** Mark Stack ***********************************************************************************/

static_assert(sizeof(MarkStack::TaggedPtr) == sizeof(uintptr_t),
              "A TaggedPtr should be the same size as a pointer");
static_assert(sizeof(MarkStack::ValueArray) == sizeof(MarkStack::SavedValueArray),
              "ValueArray and SavedValueArray should be the same size");
static_assert((sizeof(MarkStack::ValueArray) % sizeof(uintptr_t)) == 0,
              "ValueArray and SavedValueArray should be multiples of the pointer size");

static const size_t ValueArrayWords = sizeof(MarkStack::ValueArray) / sizeof(uintptr_t);

template <typename T>
struct MapTypeToMarkStackTag {};
template <>
struct MapTypeToMarkStackTag<JSObject*> { static const auto value = MarkStack::ObjectTag; };
template <>
struct MapTypeToMarkStackTag<ObjectGroup*> { static const auto value = MarkStack::GroupTag; };
template <>
struct MapTypeToMarkStackTag<jit::JitCode*> { static const auto value = MarkStack::JitCodeTag; };
template <>
struct MapTypeToMarkStackTag<JSScript*> { static const auto value = MarkStack::ScriptTag; };

static inline bool
TagIsArrayTag(MarkStack::Tag tag)
{
    return tag == MarkStack::ValueArrayTag || tag == MarkStack::SavedValueArrayTag;
}

static inline void
CheckValueArray(const MarkStack::ValueArray& array)
{
    MOZ_ASSERT(array.ptr.tag() == MarkStack::ValueArrayTag);
    MOZ_ASSERT(uintptr_t(array.start) <= uintptr_t(array.end));
    MOZ_ASSERT((uintptr_t(array.end) - uintptr_t(array.start)) % sizeof(Value) == 0);
}

static inline void
CheckSavedValueArray(const MarkStack::SavedValueArray& array)
{
    MOZ_ASSERT(array.ptr.tag() == MarkStack::SavedValueArrayTag);
    MOZ_ASSERT(array.kind == HeapSlot::Slot || array.kind == HeapSlot::Element);
}

inline
MarkStack::TaggedPtr::TaggedPtr(Tag tag, Cell* ptr)
  : bits(tag | uintptr_t(ptr))
{
    MOZ_ASSERT(tag <= LastTag);
    MOZ_ASSERT((uintptr_t(ptr) & CellAlignMask) == 0);
}

inline MarkStack::Tag
MarkStack::TaggedPtr::tag() const
{
    auto tag = Tag(bits & TagMask);
    MOZ_ASSERT(tag <= LastTag);
    return tag;
}

inline Cell*
MarkStack::TaggedPtr::ptr() const
{
    return reinterpret_cast<Cell*>(bits & ~TagMask);
}

template <typename T>
inline T*
MarkStack::TaggedPtr::as() const
{
    MOZ_ASSERT(tag() == MapTypeToMarkStackTag<T*>::value);
    MOZ_ASSERT(ptr()->isTenured());
    MOZ_ASSERT(ptr()->is<T>());
    return static_cast<T*>(ptr());
}

inline JSObject*
MarkStack::TaggedPtr::asValueArrayObject() const
{
    MOZ_ASSERT(tag() == ValueArrayTag);
    MOZ_ASSERT(ptr()->isTenured());
    MOZ_ASSERT(ptr()->is<JSObject>());
    return static_cast<JSObject*>(ptr());
}

inline JSObject*
MarkStack::TaggedPtr::asSavedValueArrayObject() const
{
    MOZ_ASSERT(tag() == SavedValueArrayTag);
    MOZ_ASSERT(ptr()->isTenured());
    MOZ_ASSERT(ptr()->is<JSObject>());
    return static_cast<JSObject*>(ptr());
}

inline JSRope*
MarkStack::TaggedPtr::asTempRope() const
{
    MOZ_ASSERT(tag() == TempRopeTag);
    MOZ_ASSERT(ptr()->isTenured());
    MOZ_ASSERT(ptr()->is<JSString>());
    return static_cast<JSRope*>(ptr());
}

inline
MarkStack::ValueArray::ValueArray(JSObject* obj, HeapSlot* startArg, HeapSlot* endArg)
  : end(endArg), start(startArg), ptr(ValueArrayTag, obj)
{}

inline
MarkStack::SavedValueArray::SavedValueArray(JSObject* obj, size_t indexArg, HeapSlot::Kind kindArg)
  : kind(kindArg), index(indexArg), ptr(SavedValueArrayTag, obj)
{}

MarkStack::MarkStack(size_t maxCapacity)
  : stack_(nullptr)
  , tos_(nullptr)
  , end_(nullptr)
  , baseCapacity_(0)
  , maxCapacity_(maxCapacity)
#ifdef DEBUG
  , iteratorCount_(0)
#endif
{}

MarkStack::~MarkStack()
{
    MOZ_ASSERT(iteratorCount_ == 0);
    js_free(stack_);
}

bool
MarkStack::init(JSGCMode gcMode)
{
    setBaseCapacity(gcMode);

    MOZ_ASSERT(!stack_);
    auto newStack = js_pod_malloc<TaggedPtr>(baseCapacity_);
    if (!newStack)
        return false;

    setStack(newStack, 0, baseCapacity_);
    return true;
}

inline void
MarkStack::setStack(TaggedPtr* stack, size_t tosIndex, size_t capacity)
{
    MOZ_ASSERT(iteratorCount_ == 0);
    stack_ = stack;
    tos_ = stack + tosIndex;
    end_ = stack + capacity;
}

void
MarkStack::setBaseCapacity(JSGCMode mode)
{
    switch (mode) {
      case JSGC_MODE_GLOBAL:
      case JSGC_MODE_ZONE:
        baseCapacity_ = NON_INCREMENTAL_MARK_STACK_BASE_CAPACITY;
        break;
      case JSGC_MODE_INCREMENTAL:
        baseCapacity_ = INCREMENTAL_MARK_STACK_BASE_CAPACITY;
        break;
      default:
        MOZ_CRASH("bad gc mode");
    }

    if (baseCapacity_ > maxCapacity_)
        baseCapacity_ = maxCapacity_;
}

void
MarkStack::setMaxCapacity(size_t maxCapacity)
{
    MOZ_ASSERT(maxCapacity != 0);
    MOZ_ASSERT(isEmpty());
    maxCapacity_ = maxCapacity;
    if (baseCapacity_ > maxCapacity_)
        baseCapacity_ = maxCapacity_;

    reset();
}

inline bool
MarkStack::pushTaggedPtr(Tag tag, Cell* ptr)
{
    if (!ensureSpace(1))
        return false;

    MOZ_ASSERT(tos_ < end_);
    *tos_++ = TaggedPtr(tag, ptr);
    return true;
}

template <typename T>
inline bool
MarkStack::push(T* ptr)
{
    return pushTaggedPtr(MapTypeToMarkStackTag<T*>::value, ptr);
}

inline bool
MarkStack::pushTempRope(JSRope* rope)
{
    return pushTaggedPtr(TempRopeTag, rope);
}

inline bool
MarkStack::push(JSObject* obj, HeapSlot* start, HeapSlot* end)
{
    return push(ValueArray(obj, start, end));
}

inline bool
MarkStack::push(const ValueArray& array)
{
    CheckValueArray(array);

    if (!ensureSpace(ValueArrayWords))
        return false;

    *reinterpret_cast<ValueArray*>(tos_.ref()) = array;
    tos_ += ValueArrayWords;
    MOZ_ASSERT(tos_ <= end_);
    MOZ_ASSERT(peekTag() == ValueArrayTag);
    return true;
}

inline bool
MarkStack::push(const SavedValueArray& array)
{
    CheckSavedValueArray(array);

    if (!ensureSpace(ValueArrayWords))
        return false;

    *reinterpret_cast<SavedValueArray*>(tos_.ref()) = array;
    tos_ += ValueArrayWords;
    MOZ_ASSERT(tos_ <= end_);
    MOZ_ASSERT(peekTag() == SavedValueArrayTag);
    return true;
}

inline const MarkStack::TaggedPtr&
MarkStack::peekPtr() const
{
    MOZ_ASSERT(!isEmpty());
    return tos_[-1];
}

inline MarkStack::Tag
MarkStack::peekTag() const
{
    return peekPtr().tag();
}

inline MarkStack::TaggedPtr
MarkStack::popPtr()
{
    MOZ_ASSERT(!isEmpty());
    MOZ_ASSERT(!TagIsArrayTag(peekTag()));
    tos_--;
    return *tos_;
}

inline MarkStack::ValueArray
MarkStack::popValueArray()
{
    MOZ_ASSERT(peekTag() == ValueArrayTag);
    MOZ_ASSERT(position() >= ValueArrayWords);

    tos_ -= ValueArrayWords;
    const auto& array = *reinterpret_cast<ValueArray*>(tos_.ref());
    CheckValueArray(array);
    return array;
}

inline MarkStack::SavedValueArray
MarkStack::popSavedValueArray()
{
    MOZ_ASSERT(peekTag() == SavedValueArrayTag);
    MOZ_ASSERT(position() >= ValueArrayWords);

    tos_ -= ValueArrayWords;
    const auto& array = *reinterpret_cast<SavedValueArray*>(tos_.ref());
    CheckSavedValueArray(array);
    return array;
}

void
MarkStack::reset()
{
    if (capacity() == baseCapacity_) {
        // No size change; keep the current stack.
        setStack(stack_, 0, baseCapacity_);
        return;
    }

    MOZ_ASSERT(baseCapacity_ != 0);
    auto newStack = js_pod_realloc<TaggedPtr>(stack_, capacity(), baseCapacity_);
    if (!newStack) {
        // If the realloc fails, just keep using the existing stack; it's
        // not ideal but better than failing.
        newStack = stack_;
        baseCapacity_ = capacity();
    }
    setStack(newStack, 0, baseCapacity_);
}

inline bool
MarkStack::ensureSpace(size_t count)
{
    if ((tos_ + count) <= end_)
        return true;

    return enlarge(count);
}

bool
MarkStack::enlarge(size_t count)
{
    size_t newCapacity = Min(maxCapacity_.ref(), capacity() * 2);
    if (newCapacity < capacity() + count)
        return false;

    size_t tosIndex = position();

    MOZ_ASSERT(newCapacity != 0);
    auto newStack = js_pod_realloc<TaggedPtr>(stack_, capacity(), newCapacity);
    if (!newStack)
        return false;

    setStack(newStack, tosIndex, newCapacity);
    return true;
}

void
MarkStack::setGCMode(JSGCMode gcMode)
{
    // The mark stack won't be resized until the next call to reset(), but
    // that will happen at the end of the next GC.
    setBaseCapacity(gcMode);
}

size_t
MarkStack::sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) const
{
    return mallocSizeOf(stack_);
}

MarkStackIter::MarkStackIter(const MarkStack& stack)
  : stack_(stack),
    pos_(stack.tos_)
{
#ifdef DEBUG
    stack.iteratorCount_++;
#endif
}

MarkStackIter::~MarkStackIter()
{
#ifdef DEBUG
    MOZ_ASSERT(stack_.iteratorCount_);
    stack_.iteratorCount_--;
#endif
}

inline size_t
MarkStackIter::position() const
{
    return pos_ - stack_.stack_;
}

inline bool
MarkStackIter::done() const
{
    return position() == 0;
}

inline MarkStack::TaggedPtr
MarkStackIter::peekPtr() const
{
    MOZ_ASSERT(!done());
    return pos_[-1];
}

inline MarkStack::Tag
MarkStackIter::peekTag() const
{
    return peekPtr().tag();
}

inline MarkStack::ValueArray
MarkStackIter::peekValueArray() const
{
    MOZ_ASSERT(peekTag() == MarkStack::ValueArrayTag);
    MOZ_ASSERT(position() >= ValueArrayWords);

    const auto& array = *reinterpret_cast<MarkStack::ValueArray*>(pos_ - ValueArrayWords);
    CheckValueArray(array);
    return array;
}

inline void
MarkStackIter::nextPtr()
{
    MOZ_ASSERT(!done());
    MOZ_ASSERT(!TagIsArrayTag(peekTag()));
    pos_--;
}

inline void
MarkStackIter::next()
{
    if (TagIsArrayTag(peekTag()))
        nextArray();
    else
        nextPtr();
}

inline void
MarkStackIter::nextArray()
{
    MOZ_ASSERT(TagIsArrayTag(peekTag()));
    MOZ_ASSERT(position() >= ValueArrayWords);
    pos_ -= ValueArrayWords;
}

void
MarkStackIter::saveValueArray(NativeObject* obj, uintptr_t index, HeapSlot::Kind kind)
{
    MOZ_ASSERT(peekTag() == MarkStack::ValueArrayTag);
    MOZ_ASSERT(peekPtr().asValueArrayObject() == obj);
    MOZ_ASSERT(position() >= ValueArrayWords);

    auto& array = *reinterpret_cast<MarkStack::SavedValueArray*>(pos_ - ValueArrayWords);
    array = MarkStack::SavedValueArray(obj, index, kind);
    CheckSavedValueArray(array);
    MOZ_ASSERT(peekTag() == MarkStack::SavedValueArrayTag);
}


/*** GCMarker *************************************************************************************/

/*
 * ExpandWeakMaps: the GC is recomputing the liveness of WeakMap entries by
 * expanding each live WeakMap into its constituent key->value edges, a table
 * of which will be consulted in a later phase whenever marking a potential
 * key.
 */
GCMarker::GCMarker(JSRuntime* rt)
  : JSTracer(rt, JSTracer::TracerKindTag::Marking, ExpandWeakMaps),
    stack(),
    color(MarkColor::Black),
    unmarkedArenaStackTop(nullptr)
#ifdef DEBUG
  , markLaterArenas(0)
  , started(false)
  , strictCompartmentChecking(false)
#endif
{
}

bool
GCMarker::init(JSGCMode gcMode)
{
    return stack.init(gcMode);
}

void
GCMarker::start()
{
#ifdef DEBUG
    MOZ_ASSERT(!started);
    started = true;
#endif
    color = MarkColor::Black;
    linearWeakMarkingDisabled_ = false;

    MOZ_ASSERT(!unmarkedArenaStackTop);
    MOZ_ASSERT(markLaterArenas == 0);
}

void
GCMarker::stop()
{
#ifdef DEBUG
    MOZ_ASSERT(isDrained());

    MOZ_ASSERT(started);
    started = false;

    MOZ_ASSERT(!unmarkedArenaStackTop);
    MOZ_ASSERT(markLaterArenas == 0);
#endif

    /* Free non-ballast stack memory. */
    stack.reset();
    AutoEnterOOMUnsafeRegion oomUnsafe;
    for (GCZonesIter zone(runtime()); !zone.done(); zone.next()) {
        if (!zone->gcWeakKeys().clear())
            oomUnsafe.crash("clearing weak keys in GCMarker::stop()");
    }
}

void
GCMarker::reset()
{
    color = MarkColor::Black;

    stack.reset();
    MOZ_ASSERT(isMarkStackEmpty());

    while (unmarkedArenaStackTop) {
        Arena* arena = unmarkedArenaStackTop;
        MOZ_ASSERT(arena->hasDelayedMarking);
        MOZ_ASSERT(markLaterArenas);
        unmarkedArenaStackTop = arena->getNextDelayedMarking();
        arena->unsetDelayedMarking();
        arena->markOverflow = 0;

#ifdef DEBUG
        markLaterArenas--;
#endif
    }
    MOZ_ASSERT(isDrained());
    MOZ_ASSERT(!markLaterArenas);
}


template <typename T>
void
GCMarker::pushTaggedPtr(T* ptr)
{
    checkZone(ptr);
    if (!stack.push(ptr))
        delayMarkingChildren(ptr);
}

void
GCMarker::pushValueArray(JSObject* obj, HeapSlot* start, HeapSlot* end)
{
    checkZone(obj);
    if (!stack.push(obj, start, end))
        delayMarkingChildren(obj);
}

void
GCMarker::repush(JSObject* obj)
{
    MOZ_ASSERT_IF(markColor() == MarkColor::Gray, gc::TenuredCell::fromPointer(obj)->isMarkedGray());
    MOZ_ASSERT_IF(markColor() == MarkColor::Black, gc::TenuredCell::fromPointer(obj)->isMarkedBlack());
    pushTaggedPtr(obj);
}

void
GCMarker::enterWeakMarkingMode()
{
    MOZ_ASSERT(tag_ == TracerKindTag::Marking);
    if (linearWeakMarkingDisabled_)
        return;

    // During weak marking mode, we maintain a table mapping weak keys to
    // entries in known-live weakmaps. Initialize it with the keys of marked
    // weakmaps -- or more precisely, the keys of marked weakmaps that are
    // mapped to not yet live values. (Once bug 1167452 implements incremental
    // weakmap marking, this initialization step will become unnecessary, as
    // the table will already hold all such keys.)
    if (weakMapAction() == ExpandWeakMaps) {
        tag_ = TracerKindTag::WeakMarking;

        for (SweepGroupZonesIter zone(runtime()); !zone.done(); zone.next()) {
            for (WeakMapBase* m : zone->gcWeakMapList()) {
                if (m->marked)
                    (void) m->markIteratively(this);
            }
        }
    }
}

void
GCMarker::leaveWeakMarkingMode()
{
    MOZ_ASSERT_IF(weakMapAction() == ExpandWeakMaps && !linearWeakMarkingDisabled_,
                  tag_ == TracerKindTag::WeakMarking);
    tag_ = TracerKindTag::Marking;

    // Table is expensive to maintain when not in weak marking mode, so we'll
    // rebuild it upon entry rather than allow it to contain stale data.
    AutoEnterOOMUnsafeRegion oomUnsafe;
    for (GCZonesIter zone(runtime()); !zone.done(); zone.next()) {
        if (!zone->gcWeakKeys().clear())
            oomUnsafe.crash("clearing weak keys in GCMarker::leaveWeakMarkingMode()");
    }
}

void
GCMarker::markDelayedChildren(Arena* arena)
{
    MOZ_ASSERT(arena->markOverflow);
    arena->markOverflow = 0;

    JS::TraceKind kind = MapAllocToTraceKind(arena->getAllocKind());

    // Whether we need to mark children of gray or black cells in the arena
    // depends on which kind of marking we were doing when the arena as pushed
    // onto the list.  We never change mark color without draining the mark
    // stack though so this is the same as the current color.
    bool markGrayCells = markColor() == MarkColor::Gray && TraceKindParticipatesInCC(kind);

    for (ArenaCellIterUnderGC i(arena); !i.done(); i.next()) {
        TenuredCell* t = i.getCell();
        if ((markGrayCells && t->isMarkedGray()) || (!markGrayCells && t->isMarkedBlack()))
            js::TraceChildren(this, t, kind);
    }
}

bool
GCMarker::markDelayedChildren(SliceBudget& budget)
{
    GCRuntime& gc = runtime()->gc;
    gcstats::AutoPhase ap(gc.stats(), gc.state() == State::Mark, gcstats::PhaseKind::MARK_DELAYED);

    MOZ_ASSERT(unmarkedArenaStackTop);
    do {
        /*
         * If marking gets delayed at the same arena again, we must repeat
         * marking of its things. For that we pop arena from the stack and
         * clear its hasDelayedMarking flag before we begin the marking.
         */
        Arena* arena = unmarkedArenaStackTop;
        MOZ_ASSERT(arena->hasDelayedMarking);
        MOZ_ASSERT(markLaterArenas);
        unmarkedArenaStackTop = arena->getNextDelayedMarking();
        arena->unsetDelayedMarking();
#ifdef DEBUG
        markLaterArenas--;
#endif
        markDelayedChildren(arena);

        budget.step(150);
        if (budget.isOverBudget())
            return false;
    } while (unmarkedArenaStackTop);
    MOZ_ASSERT(!markLaterArenas);

    return true;
}

template<typename T>
static void
PushArenaTyped(GCMarker* gcmarker, Arena* arena)
{
    for (ArenaCellIterUnderGC i(arena); !i.done(); i.next())
        gcmarker->traverse(i.get<T>());
}

struct PushArenaFunctor {
    template <typename T> void operator()(GCMarker* gcmarker, Arena* arena) {
        PushArenaTyped<T>(gcmarker, arena);
    }
};

void
gc::PushArena(GCMarker* gcmarker, Arena* arena)
{
    DispatchTraceKindTyped(PushArenaFunctor(),
                           MapAllocToTraceKind(arena->getAllocKind()), gcmarker, arena);
}

#ifdef DEBUG
void
GCMarker::checkZone(void* p)
{
    MOZ_ASSERT(started);
    DebugOnly<Cell*> cell = static_cast<Cell*>(p);
    MOZ_ASSERT_IF(cell->isTenured(), cell->asTenured().zone()->isCollecting());
}
#endif

size_t
GCMarker::sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf,
                              const AutoLockForExclusiveAccess& lock) const
{
    size_t size = stack.sizeOfExcludingThis(mallocSizeOf);
    for (ZonesIter zone(runtime(), WithAtoms); !zone.done(); zone.next())
        size += zone->gcGrayRoots().sizeOfExcludingThis(mallocSizeOf);
    return size;
}

#ifdef DEBUG
Zone*
GCMarker::stackContainsCrossZonePointerTo(const Cell* target) const
{
    MOZ_ASSERT(!JS::CurrentThreadIsHeapCollecting());

    Zone* targetZone = target->asTenured().zone();

    for (MarkStackIter iter(stack); !iter.done(); iter.next()) {
        if (iter.peekTag() != MarkStack::ObjectTag)
            continue;

        auto source = iter.peekPtr().as<JSObject>();
        Zone* sourceZone = source->zone();
        if (sourceZone == targetZone)
            continue;

        // The private slot of proxy objects might contain a cross-compartment
        // pointer.
        if (source->is<ProxyObject>()) {
            Value value = source->as<ProxyObject>().private_();
            MOZ_ASSERT_IF(!IsCrossCompartmentWrapper(source),
                          IsObjectValueInCompartment(value, source->compartment()));
            if (value.isObject() && &value.toObject() == target)
                return sourceZone;
        }

        if (Debugger::isDebuggerCrossCompartmentEdge(source, target))
            return sourceZone;
    }

    return nullptr;
}
#endif // DEBUG


/*** Tenuring Tracer *****************************************************************************/

namespace js {
template <typename T>
void
TenuringTracer::traverse(T** tp)
{
}

template <>
void
TenuringTracer::traverse(JSObject** objp)
{
    // We only ever visit the internals of objects after moving them to tenured.
    MOZ_ASSERT(!nursery().isInside(objp));

    Cell** cellp = reinterpret_cast<Cell**>(objp);
    if (!IsInsideNursery(*cellp) || nursery().getForwardedPointer(cellp))
        return;

    // Take a fast path for tenuring a plain object which is by far the most
    // common case.
    JSObject* obj = *objp;
    if (obj->is<PlainObject>()) {
        *objp = movePlainObjectToTenured(&obj->as<PlainObject>());
        return;
    }

    *objp = moveToTenuredSlow(obj);
}

template <>
void
TenuringTracer::traverse(JSString** strp)
{
    // We only ever visit the internals of strings after moving them to tenured.
    MOZ_ASSERT(!nursery().isInside(strp));

    Cell** cellp = reinterpret_cast<Cell**>(strp);
    if (IsInsideNursery(*cellp) && !nursery().getForwardedPointer(cellp))
        *strp = moveToTenured(*strp);
}

template <typename S>
struct TenuringTraversalFunctor : public IdentityDefaultAdaptor<S> {
    template <typename T> S operator()(T* t, TenuringTracer* trc) {
        trc->traverse(&t);
        return js::gc::RewrapTaggedPointer<S, T>::wrap(t);
    }
};

template <typename T>
void
TenuringTracer::traverse(T* thingp)
{
    *thingp = DispatchTyped(TenuringTraversalFunctor<T>(), *thingp, this);
}
} // namespace js

template <typename T>
void
js::gc::StoreBuffer::MonoTypeBuffer<T>::trace(StoreBuffer* owner, TenuringTracer& mover)
{
    mozilla::ReentrancyGuard g(*owner);
    MOZ_ASSERT(owner->isEnabled());
    MOZ_ASSERT(stores_.initialized());
    if (last_)
        last_.trace(mover);
    for (typename StoreSet::Range r = stores_.all(); !r.empty(); r.popFront())
        r.front().trace(mover);
}

namespace js {
namespace gc {
template void
StoreBuffer::MonoTypeBuffer<StoreBuffer::ValueEdge>::trace(StoreBuffer*, TenuringTracer&);
template void
StoreBuffer::MonoTypeBuffer<StoreBuffer::SlotsEdge>::trace(StoreBuffer*, TenuringTracer&);
template void
StoreBuffer::MonoTypeBuffer<StoreBuffer::CellPtrEdge>::trace(StoreBuffer*, TenuringTracer&);
} // namespace gc
} // namespace js

void
js::gc::StoreBuffer::SlotsEdge::trace(TenuringTracer& mover) const
{
    NativeObject* obj = object();
    MOZ_ASSERT(IsCellPointerValid(obj));

    // Beware JSObject::swap exchanging a native object for a non-native one.
    if (!obj->isNative())
        return;

    if (IsInsideNursery(obj))
        return;

    if (kind() == ElementKind) {
        uint32_t initLen = obj->getDenseInitializedLength();
        uint32_t numShifted = obj->getElementsHeader()->numShiftedElements();
        uint32_t clampedStart = start_;
        clampedStart = numShifted < clampedStart ? clampedStart - numShifted : 0;
        clampedStart = Min(clampedStart, initLen);
        uint32_t clampedEnd = start_ + count_;
        clampedEnd = numShifted < clampedEnd ? clampedEnd - numShifted : 0;
        clampedEnd = Min(clampedEnd, initLen);
        MOZ_ASSERT(clampedStart <= clampedEnd);
        mover.traceSlots(static_cast<HeapSlot*>(obj->getDenseElements() + clampedStart)
                            ->unsafeUnbarrieredForTracing(), clampedEnd - clampedStart);
    } else {
        uint32_t start = Min(start_, obj->slotSpan());
        uint32_t end = Min(start_ + count_, obj->slotSpan());
        MOZ_ASSERT(start <= end);
        mover.traceObjectSlots(obj, start, end - start);
    }
}

static inline void
TraceWholeCell(TenuringTracer& mover, JSObject* object)
{
    mover.traceObject(object);

    // Additionally trace the expando object attached to any unboxed plain
    // objects. Baseline and Ion can write properties to the expando while
    // only adding a post barrier to the owning unboxed object. Note that
    // it isn't possible for a nursery unboxed object to have a tenured
    // expando, so that adding a post barrier on the original object will
    // capture any tenured->nursery edges in the expando as well.

    if (object->is<UnboxedPlainObject>()) {
        if (UnboxedExpandoObject* expando = object->as<UnboxedPlainObject>().maybeExpando())
            expando->traceChildren(&mover);
    }
}

static inline void
TraceWholeCell(TenuringTracer& mover, JSString* str)
{
    str->traceChildren(&mover);
}

static inline void
TraceWholeCell(TenuringTracer& mover, JSScript* script)
{
    script->traceChildren(&mover);
}

static inline void
TraceWholeCell(TenuringTracer& mover, jit::JitCode* jitcode)
{
    jitcode->traceChildren(&mover);
}

template <typename T>
static void
TraceBufferedCells(TenuringTracer& mover, Arena* arena, ArenaCellSet* cells)
{
    for (size_t i = 0; i < MaxArenaCellIndex; i++) {
        if (cells->hasCell(i)) {
            auto cell = reinterpret_cast<T*>(uintptr_t(arena) + ArenaCellIndexBytes * i);
            TraceWholeCell(mover, cell);
        }
    }
}

void
js::gc::StoreBuffer::traceWholeCells(TenuringTracer& mover)
{
    for (ArenaCellSet* cells = bufferWholeCell; cells; cells = cells->next) {
        Arena* arena = cells->arena;
        MOZ_ASSERT(IsCellPointerValid(arena));

        MOZ_ASSERT(arena->bufferedCells() == cells);
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
            TraceBufferedCells<JSScript>(mover, arena, cells);
            break;
          case JS::TraceKind::JitCode:
            TraceBufferedCells<jit::JitCode>(mover, arena, cells);
            break;
          default:
            MOZ_CRASH("Unexpected trace kind");
        }
    }

    bufferWholeCell = nullptr;
}

void
js::gc::StoreBuffer::CellPtrEdge::trace(TenuringTracer& mover) const
{
    if (!*edge)
        return;

    MOZ_ASSERT(IsCellPointerValid(*edge));

#ifdef DEBUG
    auto traceKind = (*edge)->getTraceKind();
    MOZ_ASSERT(traceKind == JS::TraceKind::Object || traceKind == JS::TraceKind::String);
#endif

    // Bug 1376646: Make separate store buffers for strings and objects, and
    // only check IsInsideNursery once.

    if (!IsInsideNursery(*edge))
        return;

    if (JSString::nurseryCellIsString(*edge))
        mover.traverse(reinterpret_cast<JSString**>(edge));
    else
        mover.traverse(reinterpret_cast<JSObject**>(edge));
}

void
js::gc::StoreBuffer::ValueEdge::trace(TenuringTracer& mover) const
{
    if (deref())
        mover.traverse(edge);
}


struct TenuringFunctor
{
    template <typename T>
    void operator()(T* thing, TenuringTracer& mover) {
        mover.traverse(thing);
    }
};

// Visit all object children of the object and trace them.
void
js::TenuringTracer::traceObject(JSObject* obj)
{
    NativeObject *nobj = CallTraceHook(TenuringFunctor(), this, obj,
                                       CheckGeneration::NoChecks, *this);
    if (!nobj)
        return;

    // Note: the contents of copy on write elements pointers are filled in
    // during parsing and cannot contain nursery pointers.
    if (!nobj->hasEmptyElements() &&
        !nobj->denseElementsAreCopyOnWrite() &&
        ObjectDenseElementsMayBeMarkable(nobj))
    {
        Value* elems = static_cast<HeapSlot*>(nobj->getDenseElements())->unsafeUnbarrieredForTracing();
        traceSlots(elems, elems + nobj->getDenseInitializedLength());
    }

    traceObjectSlots(nobj, 0, nobj->slotSpan());
}

void
js::TenuringTracer::traceObjectSlots(NativeObject* nobj, uint32_t start, uint32_t length)
{
    HeapSlot* fixedStart;
    HeapSlot* fixedEnd;
    HeapSlot* dynStart;
    HeapSlot* dynEnd;
    nobj->getSlotRange(start, length, &fixedStart, &fixedEnd, &dynStart, &dynEnd);
    if (fixedStart)
        traceSlots(fixedStart->unsafeUnbarrieredForTracing(), fixedEnd->unsafeUnbarrieredForTracing());
    if (dynStart)
        traceSlots(dynStart->unsafeUnbarrieredForTracing(), dynEnd->unsafeUnbarrieredForTracing());
}

void
js::TenuringTracer::traceSlots(Value* vp, Value* end)
{
    for (; vp != end; ++vp)
        traverse(vp);
}

inline void
js::TenuringTracer::traceSlots(JS::Value* vp, uint32_t nslots)
{
    traceSlots(vp, vp + nslots);
}

void
js::TenuringTracer::traceString(JSString* str)
{
    str->traceChildren(this);
}

#ifdef DEBUG
static inline ptrdiff_t
OffsetToChunkEnd(void* p)
{
    return ChunkLocationOffset - (uintptr_t(p) & gc::ChunkMask);
}
#endif

/* Insert the given relocation entry into the list of things to visit. */
inline void
js::TenuringTracer::insertIntoObjectFixupList(RelocationOverlay* entry) {
    *objTail = entry;
    objTail = &entry->nextRef();
    *objTail = nullptr;
}

template <typename T>
inline T*
js::TenuringTracer::allocTenured(Zone* zone, AllocKind kind) {
    TenuredCell* t = zone->arenas.allocateFromFreeList(kind, Arena::thingSize(kind));
    if (!t) {
        AutoEnterOOMUnsafeRegion oomUnsafe;
        t = runtime()->gc.refillFreeListInGC(zone, kind);
        if (!t)
            oomUnsafe.crash(ChunkSize, "Failed to allocate object while tenuring.");
    }
    return static_cast<T*>(static_cast<Cell*>(t));
}

JSObject*
js::TenuringTracer::moveToTenuredSlow(JSObject* src)
{
    MOZ_ASSERT(IsInsideNursery(src));
    MOZ_ASSERT(!src->zone()->usedByHelperThread());
    MOZ_ASSERT(!src->is<PlainObject>());

    AllocKind dstKind = src->allocKindForTenure(nursery());
    auto dst = allocTenured<JSObject>(src->zone(), dstKind);

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

    // Copy the Cell contents.
    MOZ_ASSERT(OffsetToChunkEnd(src) >= ptrdiff_t(srcSize));
    js_memcpy(dst, src, srcSize);

    // Move the slots and elements, if we need to.
    if (src->isNative()) {
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

    RelocationOverlay* overlay = RelocationOverlay::fromCell(src);
    overlay->forwardTo(dst);
    insertIntoObjectFixupList(overlay);

    TracePromoteToTenured(src, dst);
    return dst;
}

inline JSObject*
js::TenuringTracer::movePlainObjectToTenured(PlainObject* src)
{
    // Fast path version of moveToTenuredSlow() for specialized for PlainObject.

    MOZ_ASSERT(IsInsideNursery(src));
    MOZ_ASSERT(!src->zone()->usedByHelperThread());

    AllocKind dstKind = src->allocKindForTenure();
    auto dst = allocTenured<PlainObject>(src->zone(), dstKind);

    size_t srcSize = Arena::thingSize(dstKind);
    tenuredSize += srcSize;

    // Copy the Cell contents.
    MOZ_ASSERT(OffsetToChunkEnd(src) >= ptrdiff_t(srcSize));
    js_memcpy(dst, src, srcSize);

    // Move the slots and elements.
    tenuredSize += moveSlotsToTenured(dst, src);
    tenuredSize += moveElementsToTenured(dst, src, dstKind);

    MOZ_ASSERT(!dst->getClass()->extObjectMovedOp());

    RelocationOverlay* overlay = RelocationOverlay::fromCell(src);
    overlay->forwardTo(dst);
    insertIntoObjectFixupList(overlay);

    TracePromoteToTenured(src, dst);
    return dst;
}

size_t
js::TenuringTracer::moveSlotsToTenured(NativeObject* dst, NativeObject* src)
{
    /* Fixed slots have already been copied over. */
    if (!src->hasDynamicSlots())
        return 0;

    if (!nursery().isInside(src->slots_)) {
        nursery().removeMallocedBuffer(src->slots_);
        return 0;
    }

    Zone* zone = src->zone();
    size_t count = src->numDynamicSlots();

    {
        AutoEnterOOMUnsafeRegion oomUnsafe;
        dst->slots_ = zone->pod_malloc<HeapSlot>(count);
        if (!dst->slots_)
            oomUnsafe.crash(sizeof(HeapSlot) * count, "Failed to allocate slots while tenuring.");
    }

    PodCopy(dst->slots_, src->slots_, count);
    nursery().setSlotsForwardingPointer(src->slots_, dst->slots_, count);
    return count * sizeof(HeapSlot);
}

size_t
js::TenuringTracer::moveElementsToTenured(NativeObject* dst, NativeObject* src, AllocKind dstKind)
{
    if (src->hasEmptyElements() || src->denseElementsAreCopyOnWrite())
        return 0;

    void* srcAllocatedHeader = src->getUnshiftedElementsHeader();

    /* TODO Bug 874151: Prefer to put element data inline if we have space. */
    if (!nursery().isInside(srcAllocatedHeader)) {
        MOZ_ASSERT(src->elements_ == dst->elements_);
        nursery().removeMallocedBuffer(srcAllocatedHeader);
        return 0;
    }

    ObjectElements* srcHeader = src->getElementsHeader();

    // Shifted elements are copied too.
    uint32_t numShifted = srcHeader->numShiftedElements();
    size_t nslots = srcHeader->numAllocatedElements();

    /* Unlike other objects, Arrays can have fixed elements. */
    if (src->is<ArrayObject>() && nslots <= GetGCKindSlots(dstKind)) {
        dst->as<ArrayObject>().setFixedElements();
        js_memcpy(dst->getElementsHeader(), srcAllocatedHeader, nslots * sizeof(HeapSlot));
        dst->elements_ += numShifted;
        nursery().setElementsForwardingPointer(srcHeader, dst->getElementsHeader(),
                                               srcHeader->capacity);
        return nslots * sizeof(HeapSlot);
    }

    MOZ_ASSERT(nslots >= 2);

    Zone* zone = src->zone();
    ObjectElements* dstHeader;
    {
        AutoEnterOOMUnsafeRegion oomUnsafe;
        dstHeader = reinterpret_cast<ObjectElements*>(zone->pod_malloc<HeapSlot>(nslots));
        if (!dstHeader) {
            oomUnsafe.crash(sizeof(HeapSlot) * nslots,
                            "Failed to allocate elements while tenuring.");
        }
    }

    js_memcpy(dstHeader, srcAllocatedHeader, nslots * sizeof(HeapSlot));
    dst->elements_ = dstHeader->elements() + numShifted;
    nursery().setElementsForwardingPointer(srcHeader, dst->getElementsHeader(),
                                           srcHeader->capacity);
    return nslots * sizeof(HeapSlot);
}

inline void
js::TenuringTracer::insertIntoStringFixupList(RelocationOverlay* entry) {
    *stringTail = entry;
    stringTail = &entry->nextRef();
    *stringTail = nullptr;
}

JSString*
js::TenuringTracer::moveToTenured(JSString* src)
{
    MOZ_ASSERT(IsInsideNursery(src));
    MOZ_ASSERT(!src->zone()->usedByHelperThread());

    AllocKind dstKind = src->getAllocKind();
    Zone* zone = src->zone();
    zone->tenuredStrings++;

    TenuredCell* t = zone->arenas.allocateFromFreeList(dstKind, Arena::thingSize(dstKind));
    if (!t) {
        AutoEnterOOMUnsafeRegion oomUnsafe;
        t = runtime()->gc.refillFreeListInGC(zone, dstKind);
        if (!t)
            oomUnsafe.crash(ChunkSize, "Failed to allocate string while tenuring.");
    }
    JSString* dst = reinterpret_cast<JSString*>(t);
    tenuredSize += moveStringToTenured(dst, src, dstKind);

    RelocationOverlay* overlay = RelocationOverlay::fromCell(src);
    overlay->forwardTo(dst);
    insertIntoStringFixupList(overlay);

    TracePromoteToTenured(src, dst);
    return dst;
}

void
js::Nursery::collectToFixedPoint(TenuringTracer& mover, TenureCountCache& tenureCounts)
{
    for (RelocationOverlay* p = mover.objHead; p; p = p->next()) {
        JSObject* obj = static_cast<JSObject*>(p->forwardingAddress());
        mover.traceObject(obj);

        TenureCount& entry = tenureCounts.findEntry(obj->groupRaw());
        if (entry.group == obj->groupRaw()) {
            entry.count++;
        } else if (!entry.group) {
            entry.group = obj->groupRaw();
            entry.count = 1;
        }
    }

    for (RelocationOverlay* p = mover.stringHead; p; p = p->next())
        mover.traceString(static_cast<JSString*>(p->forwardingAddress()));
}

size_t
js::TenuringTracer::moveStringToTenured(JSString* dst, JSString* src, AllocKind dstKind)
{
    size_t size = Arena::thingSize(dstKind);

    // At the moment, strings always have the same AllocKind between src and
    // dst. This may change in the future.
    MOZ_ASSERT(dst->asTenured().getAllocKind() == src->getAllocKind());

    // Copy the Cell contents.
    MOZ_ASSERT(OffsetToChunkEnd(src) >= ptrdiff_t(size));
    js_memcpy(dst, src, size);

    if (!src->isInline() && src->isLinear()) {
        if (src->isUndepended() || !src->hasBase()) {
            void* chars = src->asLinear().nonInlineCharsRaw();
            nursery().removeMallocedBuffer(chars);
        }
    }

    return size;
}


/*** IsMarked / IsAboutToBeFinalized **************************************************************/

template <typename T>
static inline void
CheckIsMarkedThing(T* thingp)
{
#define IS_SAME_TYPE_OR(name, type, _) mozilla::IsSame<type*, T>::value ||
    static_assert(
            JS_FOR_EACH_TRACEKIND(IS_SAME_TYPE_OR)
            false, "Only the base cell layout types are allowed into marking/tracing internals");
#undef IS_SAME_TYPE_OR

#ifdef DEBUG
    MOZ_ASSERT(thingp);
    MOZ_ASSERT(*thingp);
    JSRuntime* rt = (*thingp)->runtimeFromAnyThread();
    MOZ_ASSERT_IF(!ThingIsPermanentAtomOrWellKnownSymbol(*thingp),
                  CurrentThreadCanAccessRuntime(rt) ||
                  CurrentThreadCanAccessZone((*thingp)->zoneFromAnyThread()) ||
                  (JS::CurrentThreadIsHeapCollecting() && rt->gc.state() == State::Sweep));
#endif
}

template <typename T>
static bool
IsMarkedInternalCommon(T* thingp)
{
    CheckIsMarkedThing(thingp);
    MOZ_ASSERT(!IsInsideNursery(*thingp));

    TenuredCell& thing = (*thingp)->asTenured();
    Zone* zone = thing.zoneFromAnyThread();
    if (!zone->isCollectingFromAnyThread() || zone->isGCFinished())
        return true;

    if (zone->isGCCompacting() && IsForwarded(*thingp)) {
        *thingp = Forwarded(*thingp);
        return true;
    }

    return thing.isMarkedAny();
}

template <typename T>
static bool
IsMarkedInternal(JSRuntime* rt, T** thingp)
{
    if (IsOwnedByOtherRuntime(rt, *thingp))
        return true;

    return IsMarkedInternalCommon(thingp);
}

template <>
/* static */ bool
IsMarkedInternal(JSRuntime* rt, JSObject** thingp)
{
    if (IsOwnedByOtherRuntime(rt, *thingp))
        return true;

    if (IsInsideNursery(*thingp)) {
        MOZ_ASSERT(CurrentThreadCanAccessRuntime(rt));
        Cell** cellp = reinterpret_cast<Cell**>(thingp);
        return Nursery::getForwardedPointer(cellp);
    }
    return IsMarkedInternalCommon(thingp);
}

template <typename S>
struct IsMarkedFunctor : public IdentityDefaultAdaptor<S> {
    template <typename T> S operator()(T* t, JSRuntime* rt, bool* rv) {
        *rv = IsMarkedInternal(rt, &t);
        return js::gc::RewrapTaggedPointer<S, T>::wrap(t);
    }
};

template <typename T>
static bool
IsMarkedInternal(JSRuntime* rt, T* thingp)
{
    bool rv = true;
    *thingp = DispatchTyped(IsMarkedFunctor<T>(), *thingp, rt, &rv);
    return rv;
}

bool
js::gc::IsAboutToBeFinalizedDuringSweep(TenuredCell& tenured)
{
    MOZ_ASSERT(!IsInsideNursery(&tenured));
    MOZ_ASSERT(tenured.zoneFromAnyThread()->isGCSweeping());
    return !tenured.isMarkedAny();
}

template <typename T>
static bool
IsAboutToBeFinalizedInternal(T** thingp)
{
    CheckIsMarkedThing(thingp);
    T* thing = *thingp;
    JSRuntime* rt = thing->runtimeFromAnyThread();

    /* Permanent atoms are never finalized by non-owning runtimes. */
    if (ThingIsPermanentAtomOrWellKnownSymbol(thing) && TlsContext.get()->runtime() != rt)
        return false;

    if (IsInsideNursery(thing)) {
        return JS::CurrentThreadIsHeapMinorCollecting() &&
               !Nursery::getForwardedPointer(reinterpret_cast<Cell**>(thingp));
    }

    Zone* zone = thing->asTenured().zoneFromAnyThread();
    if (zone->isGCSweeping()) {
        return IsAboutToBeFinalizedDuringSweep(thing->asTenured());
    } else if (zone->isGCCompacting() && IsForwarded(thing)) {
        *thingp = Forwarded(thing);
        return false;
    }

    return false;
}

template <typename S>
struct IsAboutToBeFinalizedInternalFunctor : public IdentityDefaultAdaptor<S> {
    template <typename T> S operator()(T* t, bool* rv) {
        *rv = IsAboutToBeFinalizedInternal(&t);
        return js::gc::RewrapTaggedPointer<S, T>::wrap(t);
    }
};

template <typename T>
static bool
IsAboutToBeFinalizedInternal(T* thingp)
{
    bool rv = false;
    *thingp = DispatchTyped(IsAboutToBeFinalizedInternalFunctor<T>(), *thingp, &rv);
    return rv;
}

namespace js {
namespace gc {

template <typename T>
bool
IsMarkedUnbarriered(JSRuntime* rt, T* thingp)
{
    return IsMarkedInternal(rt, ConvertToBase(thingp));
}

template <typename T>
bool
IsMarked(JSRuntime* rt, WriteBarrieredBase<T>* thingp)
{
    return IsMarkedInternal(rt, ConvertToBase(thingp->unsafeUnbarrieredForTracing()));
}

template <typename T>
bool
IsAboutToBeFinalizedUnbarriered(T* thingp)
{
    return IsAboutToBeFinalizedInternal(ConvertToBase(thingp));
}

template <typename T>
bool
IsAboutToBeFinalized(WriteBarrieredBase<T>* thingp)
{
    return IsAboutToBeFinalizedInternal(ConvertToBase(thingp->unsafeUnbarrieredForTracing()));
}

template <typename T>
bool
IsAboutToBeFinalized(ReadBarrieredBase<T>* thingp)
{
    return IsAboutToBeFinalizedInternal(ConvertToBase(thingp->unsafeUnbarrieredForTracing()));
}

template <typename T>
JS_PUBLIC_API(bool)
EdgeNeedsSweep(JS::Heap<T>* thingp)
{
    return IsAboutToBeFinalizedInternal(ConvertToBase(thingp->unsafeGet()));
}

template <typename T>
JS_PUBLIC_API(bool)
EdgeNeedsSweepUnbarrieredSlow(T* thingp)
{
    return IsAboutToBeFinalizedInternal(ConvertToBase(thingp));
}

// Instantiate a copy of the Tracing templates for each derived type.
#define INSTANTIATE_ALL_VALID_TRACE_FUNCTIONS(type) \
    template bool IsMarkedUnbarriered<type>(JSRuntime*, type*);                \
    template bool IsMarked<type>(JSRuntime*, WriteBarrieredBase<type>*); \
    template bool IsAboutToBeFinalizedUnbarriered<type>(type*); \
    template bool IsAboutToBeFinalized<type>(WriteBarrieredBase<type>*); \
    template bool IsAboutToBeFinalized<type>(ReadBarrieredBase<type>*);
#define INSTANTIATE_ALL_VALID_HEAP_TRACE_FUNCTIONS(type) \
    template JS_PUBLIC_API(bool) EdgeNeedsSweep<type>(JS::Heap<type>*); \
    template JS_PUBLIC_API(bool) EdgeNeedsSweepUnbarrieredSlow<type>(type*);
FOR_EACH_GC_POINTER_TYPE(INSTANTIATE_ALL_VALID_TRACE_FUNCTIONS)
FOR_EACH_PUBLIC_GC_POINTER_TYPE(INSTANTIATE_ALL_VALID_HEAP_TRACE_FUNCTIONS)
FOR_EACH_PUBLIC_TAGGED_GC_POINTER_TYPE(INSTANTIATE_ALL_VALID_HEAP_TRACE_FUNCTIONS)
#undef INSTANTIATE_ALL_VALID_TRACE_FUNCTIONS

} /* namespace gc */
} /* namespace js */


/*** Cycle Collector Barrier Implementation *******************************************************/

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
struct AssertNonGrayTracer : public JS::CallbackTracer {
    explicit AssertNonGrayTracer(JSRuntime* rt) : JS::CallbackTracer(rt) {}
    void onChild(const JS::GCCellPtr& thing) override {
        MOZ_ASSERT(!thing.asCell()->isMarkedGray());
    }
};
#endif

class UnmarkGrayTracer : public JS::CallbackTracer
{
  public:
    // We set weakMapAction to DoNotTraceWeakMaps because the cycle collector
    // will fix up any color mismatches involving weakmaps when it runs.
    explicit UnmarkGrayTracer(JSRuntime *rt)
      : JS::CallbackTracer(rt, DoNotTraceWeakMaps)
      , unmarkedAny(false)
      , oom(false)
      , stack(rt->gc.unmarkGrayStack)
    {}

    void unmark(JS::GCCellPtr cell);

    // Whether we unmarked anything.
    bool unmarkedAny;

    // Whether we ran out of memory.
    bool oom;

  private:
    // Stack of cells to traverse.
    Vector<JS::GCCellPtr, 0, SystemAllocPolicy>& stack;

    void onChild(const JS::GCCellPtr& thing) override;

#ifdef DEBUG
    TracerKind getTracerKind() const override { return TracerKind::UnmarkGray; }
#endif
};

void
UnmarkGrayTracer::onChild(const JS::GCCellPtr& thing)
{
    Cell* cell = thing.asCell();

    // Cells in the nursery cannot be gray, and therefore must necessarily point
    // to only black edges.
    if (!cell->isTenured()) {
#ifdef DEBUG
        AssertNonGrayTracer nongray(runtime());
        TraceChildren(&nongray, cell, thing.kind());
#endif
        return;
    }

    TenuredCell& tenured = cell->asTenured();
    if (!tenured.isMarkedGray())
        return;

    tenured.markBlack();
    unmarkedAny = true;

    if (!stack.append(thing))
        oom = true;
}

void
UnmarkGrayTracer::unmark(JS::GCCellPtr cell)
{
    MOZ_ASSERT(stack.empty());

    onChild(cell);

    while (!stack.empty() && !oom)
        TraceChildren(this, stack.popCopy());

    if (oom) {
         // If we run out of memory, we take a drastic measure: require that we
         // GC again before the next CC.
        stack.clear();
        runtime()->gc.setGrayBitsInvalid();
        return;
    }
}

#ifdef DEBUG
bool
js::IsUnmarkGrayTracer(JSTracer* trc)
{
    return trc->isCallbackTracer() &&
           trc->asCallbackTracer()->getTracerKind() == JS::CallbackTracer::TracerKind::UnmarkGray;
}
#endif

static bool
UnmarkGrayGCThing(JSRuntime* rt, JS::GCCellPtr thing)
{
    MOZ_ASSERT(thing);

    UnmarkGrayTracer unmarker(rt);
    gcstats::AutoPhase innerPhase(rt->gc.stats(), gcstats::PhaseKind::UNMARK_GRAY);
    unmarker.unmark(thing);
    return unmarker.unmarkedAny;
}

JS_FRIEND_API(bool)
JS::UnmarkGrayGCThingRecursively(JS::GCCellPtr thing)
{
    MOZ_ASSERT(!JS::CurrentThreadIsHeapCollecting());
    MOZ_ASSERT(!JS::CurrentThreadIsHeapCycleCollecting());

    JSRuntime* rt = thing.asCell()->runtimeFromActiveCooperatingThread();
    gcstats::AutoPhase outerPhase(rt->gc.stats(), gcstats::PhaseKind::BARRIER);
    return UnmarkGrayGCThing(rt, thing);
}

bool
js::UnmarkGrayShapeRecursively(Shape* shape)
{
    return JS::UnmarkGrayGCThingRecursively(JS::GCCellPtr(shape));
}

namespace js {
namespace debug {

MarkInfo
GetMarkInfo(Cell* rawCell)
{
    if (!rawCell->isTenured())
        return MarkInfo::NURSERY;

    TenuredCell* cell = &rawCell->asTenured();
    if (cell->isMarkedGray())
        return MarkInfo::GRAY;
    if (cell->isMarkedBlack())
        return MarkInfo::BLACK;
    return MarkInfo::UNMARKED;
}

uintptr_t*
GetMarkWordAddress(Cell* cell)
{
    if (!cell->isTenured())
        return nullptr;

    uintptr_t* wordp;
    uintptr_t mask;
    js::gc::detail::GetGCThingMarkWordAndMask(uintptr_t(cell), ColorBit::BlackBit, &wordp, &mask);
    return wordp;
}

uintptr_t
GetMarkMask(Cell* cell, uint32_t colorBit)
{
    MOZ_ASSERT(colorBit == 0 || colorBit == 1);

    if (!cell->isTenured())
        return 0;

    ColorBit bit = colorBit == 0 ? ColorBit::BlackBit : ColorBit::GrayOrBlackBit;
    uintptr_t* wordp;
    uintptr_t mask;
    js::gc::detail::GetGCThingMarkWordAndMask(uintptr_t(cell), bit, &wordp, &mask);
    return mask;
}

}
}
