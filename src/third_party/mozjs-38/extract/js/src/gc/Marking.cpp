/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "gc/Marking.h"

#include "mozilla/DebugOnly.h"

#include "jsprf.h"

#include "gc/GCInternals.h"
#include "jit/IonCode.h"
#include "js/SliceBudget.h"
#include "vm/ArgumentsObject.h"
#include "vm/ArrayObject.h"
#include "vm/ScopeObject.h"
#include "vm/Shape.h"
#include "vm/Symbol.h"
#include "vm/TypedArrayObject.h"
#include "vm/UnboxedObject.h"

#include "jscompartmentinlines.h"
#include "jsobjinlines.h"

#include "gc/Nursery-inl.h"
#include "vm/String-inl.h"
#include "vm/Symbol-inl.h"

using namespace js;
using namespace js::gc;

using mozilla::DebugOnly;

void * const js::NullPtr::constNullValue = nullptr;

JS_PUBLIC_DATA(void * const) JS::NullPtr::constNullValue = nullptr;

/*
 * There are two mostly separate mark paths. The first is a fast path used
 * internally in the GC. The second is a slow path used for root marking and
 * for API consumers like the cycle collector or Class::trace implementations.
 *
 * The fast path uses explicit stacks. The basic marking process during a GC is
 * that all roots are pushed on to a mark stack, and then each item on the
 * stack is scanned (possibly pushing more stuff) until the stack is empty.
 *
 * PushMarkStack pushes a GC thing onto the mark stack. In some cases (shapes
 * or strings) it eagerly marks the object rather than pushing it. Popping and
 * scanning is done by the processMarkStackTop method. For efficiency reasons
 * like tail recursion elimination that method also implements the scanning of
 * objects. For other GC things it uses helper methods.
 *
 * Most of the marking code outside Marking.cpp uses functions like MarkObject,
 * MarkString, etc. These functions check if an object is in the compartment
 * currently being GCed. If it is, they call PushMarkStack. Roots are pushed
 * this way as well as pointers traversed inside trace hooks (for things like
 * PropertyIteratorObjects). It is always valid to call a MarkX function
 * instead of PushMarkStack, although it may be slower.
 *
 * The MarkX functions also handle non-GC object traversal. In this case, they
 * call a callback for each object visited. This is a recursive process; the
 * mark stacks are not involved. These callbacks may ask for the outgoing
 * pointers to be visited. Eventually, this leads to the MarkChildren functions
 * being called. These functions duplicate much of the functionality of
 * scanning functions, but they don't push onto an explicit stack.
 */

static inline void
PushMarkStack(GCMarker* gcmarker, JSObject* thing);

static inline void
PushMarkStack(GCMarker* gcmarker, JSFunction* thing);

static inline void
PushMarkStack(GCMarker* gcmarker, JSScript* thing);

static inline void
PushMarkStack(GCMarker* gcmarker, Shape* thing);

static inline void
PushMarkStack(GCMarker* gcmarker, JSString* str);

static inline void
PushMarkStack(GCMarker* gcmarker, JS::Symbol* sym);

static inline void
PushMarkStack(GCMarker* gcmarker, ObjectGroup* thing);

namespace js {
namespace gc {

static void MarkChildren(JSTracer* trc, JSString* str);
static void MarkChildren(JSTracer* trc, JS::Symbol* sym);
static void MarkChildren(JSTracer* trc, JSScript* script);
static void MarkChildren(JSTracer* trc, LazyScript* lazy);
static void MarkChildren(JSTracer* trc, Shape* shape);
static void MarkChildren(JSTracer* trc, BaseShape* base);
static void MarkChildren(JSTracer* trc, ObjectGroup* group);
static void MarkChildren(JSTracer* trc, jit::JitCode* code);

} /* namespace gc */
} /* namespace js */

/*** Object Marking ***/

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
        JS_SWEPT_TENURED_PATTERN,
        JS_ALLOCATED_TENURED_PATTERN,
        JS_SWEPT_CODE_PATTERN,
        JS_SWEPT_FRAME_PATTERN
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
#endif

static GCMarker*
AsGCMarker(JSTracer* trc)
{
    MOZ_ASSERT(IS_GC_MARKING_TRACER(trc));
    return static_cast<GCMarker*>(trc);
}

template <typename T> bool ThingIsPermanentAtom(T* thing) { return false; }
template <> bool ThingIsPermanentAtom<JSString>(JSString* str) { return str->isPermanentAtom(); }
template <> bool ThingIsPermanentAtom<JSFlatString>(JSFlatString* str) { return str->isPermanentAtom(); }
template <> bool ThingIsPermanentAtom<JSLinearString>(JSLinearString* str) { return str->isPermanentAtom(); }
template <> bool ThingIsPermanentAtom<JSAtom>(JSAtom* atom) { return atom->isPermanent(); }
template <> bool ThingIsPermanentAtom<PropertyName>(PropertyName* name) { return name->isPermanent(); }
template <> bool ThingIsPermanentAtom<JS::Symbol>(JS::Symbol* sym) { return sym->isWellKnownSymbol(); }

template<typename T>
static inline void
CheckMarkedThing(JSTracer* trc, T** thingp)
{
#ifdef DEBUG
    MOZ_ASSERT(trc);
    MOZ_ASSERT(thingp);

    T* thing = *thingp;
    MOZ_ASSERT(*thingp);

    thing = MaybeForwarded(thing);

    /* This function uses data that's not available in the nursery. */
    if (IsInsideNursery(thing))
        return;

    MOZ_ASSERT_IF(!MovingTracer::IsMovingTracer(trc) && !Nursery::IsMinorCollectionTracer(trc),
                  !IsForwarded(*thingp));

    /*
     * Permanent atoms are not associated with this runtime, but will be ignored
     * during marking.
     */
    if (ThingIsPermanentAtom(thing))
        return;

    Zone* zone = thing->zoneFromAnyThread();
    JSRuntime* rt = trc->runtime();

    MOZ_ASSERT_IF(!MovingTracer::IsMovingTracer(trc), CurrentThreadCanAccessZone(zone));
    MOZ_ASSERT_IF(!MovingTracer::IsMovingTracer(trc), CurrentThreadCanAccessRuntime(rt));

    MOZ_ASSERT(zone->runtimeFromAnyThread() == trc->runtime());
    MOZ_ASSERT(trc->hasTracingDetails());

    bool isGcMarkingTracer = IS_GC_MARKING_TRACER(trc);

    MOZ_ASSERT_IF(zone->requireGCTracer(), isGcMarkingTracer);

    MOZ_ASSERT(thing->isAligned());

    MOZ_ASSERT(MapTypeToTraceKind<T>::kind == GetGCThingTraceKind(thing));

    if (isGcMarkingTracer) {
        GCMarker* gcMarker = static_cast<GCMarker*>(trc);
        MOZ_ASSERT_IF(gcMarker->shouldCheckCompartments(),
                      zone->isCollecting() || rt->isAtomsZone(zone));

        MOZ_ASSERT_IF(gcMarker->getMarkColor() == GRAY,
                      !zone->isGCMarkingBlack() || rt->isAtomsZone(zone));

        MOZ_ASSERT(!(zone->isGCSweeping() || zone->isGCFinished() || zone->isGCCompacting()));
    }

    /*
     * Try to assert that the thing is allocated.  This is complicated by the
     * fact that allocated things may still contain the poison pattern if that
     * part has not been overwritten, and that the free span list head in the
     * ArenaHeader may not be synced with the real one in ArenaLists.  Also,
     * background sweeping may be running and concurrently modifiying the free
     * list.
     */
    MOZ_ASSERT_IF(IsThingPoisoned(thing) && rt->isHeapBusy() && !rt->gc.isBackgroundSweeping(),
                  !InFreeList(thing->asTenured().arenaHeader(), thing));
#endif
}

/*
 * We only set the maybeAlive flag for objects and scripts. It's assumed that,
 * if a compartment is alive, then it will have at least some live object or
 * script it in. Even if we get this wrong, the worst that will happen is that
 * scheduledForDestruction will be set on the compartment, which will cause some
 * extra GC activity to try to free the compartment.
 */
template<typename T>
static inline void
SetMaybeAliveFlag(T* thing)
{
}

template<>
void
SetMaybeAliveFlag(JSObject* thing)
{
    thing->compartment()->maybeAlive = true;
}

template<>
void
SetMaybeAliveFlag(NativeObject* thing)
{
    thing->compartment()->maybeAlive = true;
}

template<>
void
SetMaybeAliveFlag(JSScript* thing)
{
    thing->compartment()->maybeAlive = true;
}

template<typename T>
static void
MarkInternal(JSTracer* trc, T** thingp)
{
    CheckMarkedThing(trc, thingp);
    T* thing = *thingp;

    if (!trc->callback) {
        /*
         * We may mark a Nursery thing outside the context of the
         * MinorCollectionTracer because of a pre-barrier. The pre-barrier is
         * not needed in this case because we perform a minor collection before
         * each incremental slice.
         */
        if (IsInsideNursery(thing))
            return;

        /*
         * Don't mark permanent atoms, as they may be associated with another
         * runtime. Note that PushMarkStack() also checks this, but the tests
         * and maybeAlive write below should only be done on the main thread.
         */
        if (ThingIsPermanentAtom(thing))
            return;

        /*
         * Don't mark things outside a compartment if we are in a
         * per-compartment GC.
         */
        if (!thing->zone()->isGCMarking())
            return;

        PushMarkStack(AsGCMarker(trc), thing);
        SetMaybeAliveFlag(thing);
    } else {
        trc->callback(trc, (void**)thingp, MapTypeToTraceKind<T>::kind);
        trc->unsetTracingLocation();
    }

    trc->clearTracingDetails();
}

#define JS_ROOT_MARKING_ASSERT(trc)                                     \
    MOZ_ASSERT_IF(IS_GC_MARKING_TRACER(trc),                            \
                  trc->runtime()->gc.state() == NO_INCREMENTAL ||       \
                  trc->runtime()->gc.state() == MARK_ROOTS);

namespace js {
namespace gc {

template <typename T>
void
MarkUnbarriered(JSTracer* trc, T** thingp, const char* name)
{
    trc->setTracingName(name);
    MarkInternal(trc, thingp);
}

template <typename T>
static void
Mark(JSTracer* trc, BarrieredBase<T*>* thing, const char* name)
{
    trc->setTracingName(name);
    MarkInternal(trc, thing->unsafeGet());
}

void
MarkPermanentAtom(JSTracer* trc, JSAtom* atom, const char* name)
{
    trc->setTracingName(name);

    MOZ_ASSERT(atom->isPermanent());

    CheckMarkedThing(trc, &atom);

    if (!trc->callback) {
        // Atoms do not refer to other GC things so don't need to go on the mark stack.
        // Additionally, PushMarkStack will ignore permanent atoms.
        atom->markIfUnmarked();
    } else {
        void* thing = atom;
        trc->callback(trc, &thing, JSTRACE_STRING);
        MOZ_ASSERT(thing == atom);
        trc->unsetTracingLocation();
    }

    trc->clearTracingDetails();
}

void
MarkWellKnownSymbol(JSTracer* trc, JS::Symbol* sym)
{
    if (!sym)
        return;

    trc->setTracingName("wellKnownSymbols");

    MOZ_ASSERT(sym->isWellKnownSymbol());
    CheckMarkedThing(trc, &sym);
    if (!trc->callback) {
        // Permanent atoms are marked before well-known symbols.
        MOZ_ASSERT(sym->description()->isMarked());
        sym->markIfUnmarked();
    } else {
        void* thing = sym;
        trc->callback(trc, &thing, JSTRACE_SYMBOL);
        MOZ_ASSERT(thing == sym);
        trc->unsetTracingLocation();
    }

    trc->clearTracingDetails();
}

} /* namespace gc */
} /* namespace js */

template <typename T>
static void
MarkRoot(JSTracer* trc, T** thingp, const char* name)
{
    JS_ROOT_MARKING_ASSERT(trc);
    trc->setTracingName(name);
    MarkInternal(trc, thingp);
}

template <typename T>
static void
MarkRange(JSTracer* trc, size_t len, HeapPtr<T*>* vec, const char* name)
{
    for (size_t i = 0; i < len; ++i) {
        if (vec[i].get()) {
            trc->setTracingIndex(name, i);
            MarkInternal(trc, vec[i].unsafeGet());
        }
    }
}

template <typename T>
static void
MarkRootRange(JSTracer* trc, size_t len, T** vec, const char* name)
{
    JS_ROOT_MARKING_ASSERT(trc);
    for (size_t i = 0; i < len; ++i) {
        if (vec[i]) {
            trc->setTracingIndex(name, i);
            MarkInternal(trc, &vec[i]);
        }
    }
}

namespace js {
namespace gc {

template <typename T>
static bool
IsMarked(T** thingp)
{
    MOZ_ASSERT_IF(!ThingIsPermanentAtom(*thingp),
                  CurrentThreadCanAccessRuntime((*thingp)->runtimeFromMainThread()));
    return IsMarkedFromAnyThread(thingp);
}

template <typename T>
static bool
IsMarkedFromAnyThread(T** thingp)
{
    MOZ_ASSERT(thingp);
    MOZ_ASSERT(*thingp);
    JSRuntime* rt = (*thingp)->runtimeFromAnyThread();

    if (IsInsideNursery(*thingp)) {
        Nursery& nursery = rt->gc.nursery;
        return nursery.getForwardedPointer(thingp);
    }

    Zone* zone = (*thingp)->asTenured().zoneFromAnyThread();
    if (!zone->isCollectingFromAnyThread() || zone->isGCFinished())
        return true;
    if (zone->isGCCompacting() && IsForwarded(*thingp))
        *thingp = Forwarded(*thingp);
    return (*thingp)->asTenured().isMarked();
}

template <typename T>
static bool
IsAboutToBeFinalized(T** thingp)
{
    MOZ_ASSERT_IF(!ThingIsPermanentAtom(*thingp),
                  CurrentThreadCanAccessRuntime((*thingp)->runtimeFromMainThread()));
    return IsAboutToBeFinalizedFromAnyThread(thingp);
}

template <typename T>
static bool
IsAboutToBeFinalizedFromAnyThread(T** thingp)
{
    MOZ_ASSERT(thingp);
    MOZ_ASSERT(*thingp);

    T* thing = *thingp;
    JSRuntime* rt = thing->runtimeFromAnyThread();

    /* Permanent atoms are never finalized by non-owning runtimes. */
    if (ThingIsPermanentAtom(thing) && !TlsPerThreadData.get()->associatedWith(rt))
        return false;

    Nursery& nursery = rt->gc.nursery;
    MOZ_ASSERT_IF(!rt->isHeapMinorCollecting(), !IsInsideNursery(thing));
    if (rt->isHeapMinorCollecting()) {
        if (IsInsideNursery(thing))
            return !nursery.getForwardedPointer(thingp);
        return false;
    }

    Zone* zone = thing->asTenured().zoneFromAnyThread();
    if (zone->isGCSweeping()) {
        if (thing->asTenured().arenaHeader()->allocatedDuringIncremental)
            return false;
        return !thing->asTenured().isMarked();
    }
    else if (zone->isGCCompacting() && IsForwarded(thing)) {
        *thingp = Forwarded(thing);
        return false;
    }

    return false;
}

template <typename T>
T*
UpdateIfRelocated(JSRuntime* rt, T** thingp)
{
    MOZ_ASSERT(thingp);
    if (!*thingp)
        return nullptr;

    if (rt->isHeapMinorCollecting() && IsInsideNursery(*thingp)) {
        rt->gc.nursery.getForwardedPointer(thingp);
        return *thingp;
    }

    Zone* zone = (*thingp)->zone();
    if (zone->isGCCompacting() && IsForwarded(*thingp))
        *thingp = Forwarded(*thingp);

    return *thingp;
}

#define DeclMarkerImpl(base, type)                                                                \
void                                                                                              \
Mark##base(JSTracer* trc, BarrieredBase<type*>* thing, const char* name)                          \
{                                                                                                 \
    Mark<type>(trc, thing, name);                                                                 \
}                                                                                                 \
                                                                                                  \
void                                                                                              \
Mark##base##Root(JSTracer* trc, type** thingp, const char* name)                                  \
{                                                                                                 \
    MarkRoot<type>(trc, thingp, name);                                                            \
}                                                                                                 \
                                                                                                  \
void                                                                                              \
Mark##base##Unbarriered(JSTracer* trc, type** thingp, const char* name)                           \
{                                                                                                 \
    MarkUnbarriered<type>(trc, thingp, name);                                                     \
}                                                                                                 \
                                                                                                  \
/* Explicitly instantiate MarkUnbarriered<type*>. It is referenced from */                        \
/* other translation units and the instantiation might otherwise get */                           \
/* inlined away. */                                                                               \
template void MarkUnbarriered<type>(JSTracer*, type**, const char*);                           \
                                                                                                  \
void                                                                                              \
Mark##base##Range(JSTracer* trc, size_t len, HeapPtr<type*>* vec, const char* name)               \
{                                                                                                 \
    MarkRange<type>(trc, len, vec, name);                                                         \
}                                                                                                 \
                                                                                                  \
void                                                                                              \
Mark##base##RootRange(JSTracer* trc, size_t len, type** vec, const char* name)                    \
{                                                                                                 \
    MarkRootRange<type>(trc, len, vec, name);                                                     \
}                                                                                                 \
                                                                                                  \
bool                                                                                              \
Is##base##Marked(type** thingp)                                                                   \
{                                                                                                 \
    return IsMarked<type>(thingp);                                                                \
}                                                                                                 \
                                                                                                  \
bool                                                                                              \
Is##base##MarkedFromAnyThread(BarrieredBase<type*>* thingp)                                       \
{                                                                                                 \
    return IsMarkedFromAnyThread<type>(thingp->unsafeGet());                                      \
}                                                                                                 \
                                                                                                  \
bool                                                                                              \
Is##base##Marked(BarrieredBase<type*>* thingp)                                                    \
{                                                                                                 \
    return IsMarked<type>(thingp->unsafeGet());                                                   \
}                                                                                                 \
                                                                                                  \
bool                                                                                              \
Is##base##AboutToBeFinalized(type** thingp)                                                       \
{                                                                                                 \
    return IsAboutToBeFinalized<type>(thingp);                                                    \
}                                                                                                 \
                                                                                                  \
bool                                                                                              \
Is##base##AboutToBeFinalizedFromAnyThread(type** thingp)                                          \
{                                                                                                 \
    return IsAboutToBeFinalizedFromAnyThread<type>(thingp);                                       \
}                                                                                                 \
                                                                                                  \
bool                                                                                              \
Is##base##AboutToBeFinalized(BarrieredBase<type*>* thingp)                                        \
{                                                                                                 \
    return IsAboutToBeFinalized<type>(thingp->unsafeGet());                                       \
}                                                                                                 \
                                                                                                  \
type *                                                                                            \
Update##base##IfRelocated(JSRuntime* rt, BarrieredBase<type*>* thingp)                            \
{                                                                                                 \
    return UpdateIfRelocated<type>(rt, thingp->unsafeGet());                                      \
}                                                                                                 \
                                                                                                  \
type *                                                                                            \
Update##base##IfRelocated(JSRuntime* rt, type** thingp)                                           \
{                                                                                                 \
    return UpdateIfRelocated<type>(rt, thingp);                                                   \
}


DeclMarkerImpl(BaseShape, BaseShape)
DeclMarkerImpl(BaseShape, UnownedBaseShape)
DeclMarkerImpl(JitCode, jit::JitCode)
DeclMarkerImpl(Object, NativeObject)
DeclMarkerImpl(Object, ArrayObject)
DeclMarkerImpl(Object, ArgumentsObject)
DeclMarkerImpl(Object, ArrayBufferObject)
DeclMarkerImpl(Object, ArrayBufferObjectMaybeShared)
DeclMarkerImpl(Object, ArrayBufferViewObject)
DeclMarkerImpl(Object, DebugScopeObject)
DeclMarkerImpl(Object, GlobalObject)
DeclMarkerImpl(Object, JSObject)
DeclMarkerImpl(Object, JSFunction)
DeclMarkerImpl(Object, NestedScopeObject)
DeclMarkerImpl(Object, PlainObject)
DeclMarkerImpl(Object, SavedFrame)
DeclMarkerImpl(Object, ScopeObject)
DeclMarkerImpl(Object, SharedArrayBufferObject)
DeclMarkerImpl(Object, SharedTypedArrayObject)
DeclMarkerImpl(Script, JSScript)
DeclMarkerImpl(LazyScript, LazyScript)
DeclMarkerImpl(Shape, Shape)
DeclMarkerImpl(String, JSAtom)
DeclMarkerImpl(String, JSString)
DeclMarkerImpl(String, JSFlatString)
DeclMarkerImpl(String, JSLinearString)
DeclMarkerImpl(String, PropertyName)
DeclMarkerImpl(Symbol, JS::Symbol)
DeclMarkerImpl(ObjectGroup, js::ObjectGroup)

} /* namespace gc */
} /* namespace js */

/*** Externally Typed Marking ***/

void
gc::MarkKind(JSTracer* trc, void** thingp, JSGCTraceKind kind)
{
    MOZ_ASSERT(thingp);
    MOZ_ASSERT(*thingp);
    DebugOnly<Cell*> cell = static_cast<Cell*>(*thingp);
    MOZ_ASSERT_IF(cell->isTenured(),
                  kind == MapAllocToTraceKind(cell->asTenured().getAllocKind()));
    switch (kind) {
      case JSTRACE_OBJECT:
        MarkInternal(trc, reinterpret_cast<JSObject**>(thingp));
        break;
      case JSTRACE_SCRIPT:
        MarkInternal(trc, reinterpret_cast<JSScript**>(thingp));
        break;
      case JSTRACE_STRING:
        MarkInternal(trc, reinterpret_cast<JSString**>(thingp));
        break;
      case JSTRACE_SYMBOL:
        MarkInternal(trc, reinterpret_cast<JS::Symbol**>(thingp));
        break;
      case JSTRACE_BASE_SHAPE:
        MarkInternal(trc, reinterpret_cast<BaseShape**>(thingp));
        break;
      case JSTRACE_JITCODE:
        MarkInternal(trc, reinterpret_cast<jit::JitCode**>(thingp));
        break;
      case JSTRACE_LAZY_SCRIPT:
        MarkInternal(trc, reinterpret_cast<LazyScript**>(thingp));
        break;
      case JSTRACE_SHAPE:
        MarkInternal(trc, reinterpret_cast<Shape**>(thingp));
        break;
      case JSTRACE_OBJECT_GROUP:
        MarkInternal(trc, reinterpret_cast<ObjectGroup**>(thingp));
        break;
      default:
        MOZ_CRASH("Invalid trace kind in MarkKind.");
    }
}

static void
MarkGCThingInternal(JSTracer* trc, void** thingp, const char* name)
{
    trc->setTracingName(name);
    MOZ_ASSERT(thingp);
    if (!*thingp)
        return;
    MarkKind(trc, thingp, GetGCThingTraceKind(*thingp));
}

void
gc::MarkGCThingRoot(JSTracer* trc, void** thingp, const char* name)
{
    JS_ROOT_MARKING_ASSERT(trc);
    MarkGCThingInternal(trc, thingp, name);
}

void
gc::MarkGCThingUnbarriered(JSTracer* trc, void** thingp, const char* name)
{
    MarkGCThingInternal(trc, thingp, name);
}

/*** ID Marking ***/

static inline void
MarkIdInternal(JSTracer* trc, jsid* id)
{
    if (JSID_IS_STRING(*id)) {
        JSString* str = JSID_TO_STRING(*id);
        trc->setTracingLocation((void*)id);
        MarkInternal(trc, &str);
        *id = NON_INTEGER_ATOM_TO_JSID(reinterpret_cast<JSAtom*>(str));
    } else if (JSID_IS_SYMBOL(*id)) {
        JS::Symbol* sym = JSID_TO_SYMBOL(*id);
        trc->setTracingLocation((void*)id);
        MarkInternal(trc, &sym);
        *id = SYMBOL_TO_JSID(sym);
    } else {
        /* Unset realLocation manually if we do not call MarkInternal. */
        trc->unsetTracingLocation();
    }
}

void
gc::MarkId(JSTracer* trc, BarrieredBase<jsid>* id, const char* name)
{
    trc->setTracingName(name);
    MarkIdInternal(trc, id->unsafeGet());
}

void
gc::MarkIdRoot(JSTracer* trc, jsid* id, const char* name)
{
    JS_ROOT_MARKING_ASSERT(trc);
    trc->setTracingName(name);
    MarkIdInternal(trc, id);
}

void
gc::MarkIdUnbarriered(JSTracer* trc, jsid* id, const char* name)
{
    trc->setTracingName(name);
    MarkIdInternal(trc, id);
}

void
gc::MarkIdRange(JSTracer* trc, size_t len, HeapId* vec, const char* name)
{
    for (size_t i = 0; i < len; ++i) {
        trc->setTracingIndex(name, i);
        MarkIdInternal(trc, vec[i].unsafeGet());
    }
}

void
gc::MarkIdRootRange(JSTracer* trc, size_t len, jsid* vec, const char* name)
{
    JS_ROOT_MARKING_ASSERT(trc);
    for (size_t i = 0; i < len; ++i) {
        trc->setTracingIndex(name, i);
        MarkIdInternal(trc, &vec[i]);
    }
}

/*** Value Marking ***/

static inline void
MarkValueInternal(JSTracer* trc, Value* v)
{
    if (v->isMarkable()) {
        MOZ_ASSERT(v->toGCThing());
        void* thing = v->toGCThing();
        trc->setTracingLocation((void*)v);
        MarkKind(trc, &thing, v->gcKind());
        if (v->isString()) {
            v->setString((JSString*)thing);
        } else if (v->isObject()) {
            v->setObjectOrNull((JSObject*)thing);
        } else {
            MOZ_ASSERT(v->isSymbol());
            v->setSymbol((JS::Symbol*)thing);
        }
    } else {
        /* Unset realLocation manually if we do not call MarkInternal. */
        trc->unsetTracingLocation();
    }
}

void
gc::MarkValue(JSTracer* trc, BarrieredBase<Value>* v, const char* name)
{
    trc->setTracingName(name);
    MarkValueInternal(trc, v->unsafeGet());
}

void
gc::MarkValueRoot(JSTracer* trc, Value* v, const char* name)
{
    JS_ROOT_MARKING_ASSERT(trc);
    trc->setTracingName(name);
    MarkValueInternal(trc, v);
}

void
TypeSet::MarkTypeRoot(JSTracer* trc, TypeSet::Type* v, const char* name)
{
    JS_ROOT_MARKING_ASSERT(trc);
    trc->setTracingName(name);
    if (v->isSingleton()) {
        JSObject* obj = v->singleton();
        MarkInternal(trc, &obj);
        *v = TypeSet::ObjectType(obj);
    } else if (v->isGroup()) {
        ObjectGroup* group = v->group();
        MarkInternal(trc, &group);
        *v = TypeSet::ObjectType(group);
    }
}

void
gc::MarkValueRange(JSTracer* trc, size_t len, BarrieredBase<Value>* vec, const char* name)
{
    for (size_t i = 0; i < len; ++i) {
        trc->setTracingIndex(name, i);
        MarkValueInternal(trc, vec[i].unsafeGet());
    }
}

void
gc::MarkValueRootRange(JSTracer* trc, size_t len, Value* vec, const char* name)
{
    JS_ROOT_MARKING_ASSERT(trc);
    for (size_t i = 0; i < len; ++i) {
        trc->setTracingIndex(name, i);
        MarkValueInternal(trc, &vec[i]);
    }
}

bool
gc::IsValueMarked(Value* v)
{
    MOZ_ASSERT(v->isMarkable());
    bool rv;
    if (v->isString()) {
        JSString* str = (JSString*)v->toGCThing();
        rv = IsMarked<JSString>(&str);
        v->setString(str);
    } else if (v->isObject()) {
        JSObject* obj = (JSObject*)v->toGCThing();
        rv = IsMarked<JSObject>(&obj);
        v->setObject(*obj);
    } else {
        MOZ_ASSERT(v->isSymbol());
        JS::Symbol* sym = v->toSymbol();
        rv = IsMarked<JS::Symbol>(&sym);
        v->setSymbol(sym);
    }
    return rv;
}

bool
gc::IsValueAboutToBeFinalized(Value* v)
{
    MOZ_ASSERT(v->isMarkable());
    bool rv;
    if (v->isString()) {
        JSString* str = (JSString*)v->toGCThing();
        rv = IsAboutToBeFinalized<JSString>(&str);
        v->setString(str);
    } else if (v->isObject()) {
        JSObject* obj = (JSObject*)v->toGCThing();
        rv = IsAboutToBeFinalized<JSObject>(&obj);
        v->setObject(*obj);
    } else {
        MOZ_ASSERT(v->isSymbol());
        JS::Symbol* sym = v->toSymbol();
        rv = IsAboutToBeFinalized<JS::Symbol>(&sym);
        v->setSymbol(sym);
    }
    return rv;
}

bool
gc::IsValueAboutToBeFinalizedFromAnyThread(Value* v)
{
    MOZ_ASSERT(v->isMarkable());
    bool rv;
    if (v->isString()) {
        JSString* str = (JSString*)v->toGCThing();
        rv = IsAboutToBeFinalizedFromAnyThread<JSString>(&str);
        v->setString(str);
    } else if (v->isObject()) {
        JSObject* obj = (JSObject*)v->toGCThing();
        rv = IsAboutToBeFinalizedFromAnyThread<JSObject>(&obj);
        v->setObject(*obj);
    } else {
        MOZ_ASSERT(v->isSymbol());
        JS::Symbol* sym = v->toSymbol();
        rv = IsAboutToBeFinalizedFromAnyThread<JS::Symbol>(&sym);
        v->setSymbol(sym);
    }
    return rv;
}

/*** Slot Marking ***/

bool
gc::IsSlotMarked(HeapSlot* s)
{
    return IsMarked(s);
}

void
gc::MarkSlot(JSTracer* trc, HeapSlot* s, const char* name)
{
    trc->setTracingName(name);
    MarkValueInternal(trc, s->unsafeGet());
}

void
gc::MarkArraySlots(JSTracer* trc, size_t len, HeapSlot* vec, const char* name)
{
    for (size_t i = 0; i < len; ++i) {
        trc->setTracingIndex(name, i);
        MarkValueInternal(trc, vec[i].unsafeGet());
    }
}

void
gc::MarkObjectSlots(JSTracer* trc, NativeObject* obj, uint32_t start, uint32_t nslots)
{
    MOZ_ASSERT(obj->isNative());
    for (uint32_t i = start; i < (start + nslots); ++i) {
        trc->setTracingDetails(js_GetObjectSlotName, obj, i);
        MarkValueInternal(trc, obj->getSlotRef(i).unsafeGet());
    }
}

static bool
ShouldMarkCrossCompartment(JSTracer* trc, JSObject* src, Cell* cell)
{
    if (!IS_GC_MARKING_TRACER(trc))
        return true;

    uint32_t color = AsGCMarker(trc)->getMarkColor();
    MOZ_ASSERT(color == BLACK || color == GRAY);

    if (IsInsideNursery(cell)) {
        MOZ_ASSERT(color == BLACK);
        return false;
    }
    TenuredCell& tenured = cell->asTenured();

    JS::Zone* zone = tenured.zone();
    if (color == BLACK) {
        /*
         * Having black->gray edges violates our promise to the cycle
         * collector. This can happen if we're collecting a compartment and it
         * has an edge to an uncollected compartment: it's possible that the
         * source and destination of the cross-compartment edge should be gray,
         * but the source was marked black by the conservative scanner.
         */
        if (tenured.isMarked(GRAY)) {
            MOZ_ASSERT(!zone->isCollecting());
            trc->runtime()->gc.setFoundBlackGrayEdges();
        }
        return zone->isGCMarking();
    } else {
        if (zone->isGCMarkingBlack()) {
            /*
             * The destination compartment is being not being marked gray now,
             * but it will be later, so record the cell so it can be marked gray
             * at the appropriate time.
             */
            if (!tenured.isMarked())
                DelayCrossCompartmentGrayMarking(src);
            return false;
        }
        return zone->isGCMarkingGray();
    }
}

void
gc::MarkCrossCompartmentObjectUnbarriered(JSTracer* trc, JSObject* src, JSObject** dst, const char* name)
{
    if (ShouldMarkCrossCompartment(trc, src, *dst))
        MarkObjectUnbarriered(trc, dst, name);
}

void
gc::MarkCrossCompartmentScriptUnbarriered(JSTracer* trc, JSObject* src, JSScript** dst,
                                          const char* name)
{
    if (ShouldMarkCrossCompartment(trc, src, *dst))
        MarkScriptUnbarriered(trc, dst, name);
}

void
gc::MarkCrossCompartmentSlot(JSTracer* trc, JSObject* src, HeapValue* dst, const char* name)
{
    if (dst->isMarkable() && ShouldMarkCrossCompartment(trc, src, (Cell*)dst->toGCThing()))
        MarkValue(trc, dst, name);
}

/*** Special Marking ***/

void
gc::MarkValueUnbarriered(JSTracer* trc, Value* v, const char* name)
{
    trc->setTracingName(name);
    MarkValueInternal(trc, v);
}

bool
gc::IsCellMarked(Cell** thingp)
{
    return IsMarked<Cell>(thingp);
}

bool
gc::IsCellAboutToBeFinalized(Cell** thingp)
{
    return IsAboutToBeFinalized<Cell>(thingp);
}

bool
gc::IsCellAboutToBeFinalizedFromAnyThread(Cell** thingp)
{
    return IsAboutToBeFinalizedFromAnyThread<Cell>(thingp);
}

/*** Push Mark Stack ***/

#define JS_COMPARTMENT_ASSERT(rt, thing)                                \
    MOZ_ASSERT((thing)->zone()->isGCMarking())

#define JS_COMPARTMENT_ASSERT_STR(rt, thing)                            \
    MOZ_ASSERT((thing)->zone()->isGCMarking() ||                        \
               (rt)->isAtomsZone((thing)->zone()));

// Symbols can also be in the atoms zone.
#define JS_COMPARTMENT_ASSERT_SYM(rt, sym)                              \
    JS_COMPARTMENT_ASSERT_STR(rt, sym)

static void
PushMarkStack(GCMarker* gcmarker, JSObject* thing)
{
    JS_COMPARTMENT_ASSERT(gcmarker->runtime(), thing);
    MOZ_ASSERT(!IsInsideNursery(thing));

    if (thing->asTenured().markIfUnmarked(gcmarker->getMarkColor()))
        gcmarker->pushObject(thing);
}

/*
 * PushMarkStack for BaseShape unpacks its children directly onto the mark
 * stack. For a pre-barrier between incremental slices, this may result in
 * objects in the nursery getting pushed onto the mark stack. It is safe to
 * ignore these objects because they will be marked by the matching
 * post-barrier during the minor GC at the start of each incremental slice.
 */
static void
MaybePushMarkStackBetweenSlices(GCMarker* gcmarker, JSObject* thing)
{
    DebugOnly<JSRuntime*> rt = gcmarker->runtime();
    JS_COMPARTMENT_ASSERT(rt, thing);
    MOZ_ASSERT_IF(rt->isHeapBusy(), !IsInsideNursery(thing));

    if (!IsInsideNursery(thing) && thing->asTenured().markIfUnmarked(gcmarker->getMarkColor()))
        gcmarker->pushObject(thing);
}

static void
PushMarkStack(GCMarker* gcmarker, JSFunction* thing)
{
    JS_COMPARTMENT_ASSERT(gcmarker->runtime(), thing);
    MOZ_ASSERT(!IsInsideNursery(thing));

    if (thing->asTenured().markIfUnmarked(gcmarker->getMarkColor()))
        gcmarker->pushObject(thing);
}

static void
PushMarkStack(GCMarker* gcmarker, ObjectGroup* thing)
{
    JS_COMPARTMENT_ASSERT(gcmarker->runtime(), thing);
    MOZ_ASSERT(!IsInsideNursery(thing));

    if (thing->markIfUnmarked(gcmarker->getMarkColor()))
        gcmarker->pushType(thing);
}

static void
PushMarkStack(GCMarker* gcmarker, JSScript* thing)
{
    JS_COMPARTMENT_ASSERT(gcmarker->runtime(), thing);
    MOZ_ASSERT(!IsInsideNursery(thing));

    /*
     * We mark scripts directly rather than pushing on the stack as they can
     * refer to other scripts only indirectly (like via nested functions) and
     * we cannot get to deep recursion.
     */
    if (thing->markIfUnmarked(gcmarker->getMarkColor()))
        MarkChildren(gcmarker, thing);
}

static void
PushMarkStack(GCMarker* gcmarker, LazyScript* thing)
{
    JS_COMPARTMENT_ASSERT(gcmarker->runtime(), thing);
    MOZ_ASSERT(!IsInsideNursery(thing));

    /*
     * We mark lazy scripts directly rather than pushing on the stack as they
     * only refer to normal scripts and to strings, and cannot recurse.
     */
    if (thing->markIfUnmarked(gcmarker->getMarkColor()))
        MarkChildren(gcmarker, thing);
}

static void
ScanShape(GCMarker* gcmarker, Shape* shape);

static void
PushMarkStack(GCMarker* gcmarker, Shape* thing)
{
    JS_COMPARTMENT_ASSERT(gcmarker->runtime(), thing);
    MOZ_ASSERT(!IsInsideNursery(thing));

    /* We mark shapes directly rather than pushing on the stack. */
    if (thing->markIfUnmarked(gcmarker->getMarkColor()))
        ScanShape(gcmarker, thing);
}

static void
PushMarkStack(GCMarker* gcmarker, jit::JitCode* thing)
{
    JS_COMPARTMENT_ASSERT(gcmarker->runtime(), thing);
    MOZ_ASSERT(!IsInsideNursery(thing));

    if (thing->markIfUnmarked(gcmarker->getMarkColor()))
        gcmarker->pushJitCode(thing);
}

static inline void
ScanBaseShape(GCMarker* gcmarker, BaseShape* base);

static void
PushMarkStack(GCMarker* gcmarker, BaseShape* thing)
{
    JS_COMPARTMENT_ASSERT(gcmarker->runtime(), thing);
    MOZ_ASSERT(!IsInsideNursery(thing));

    /* We mark base shapes directly rather than pushing on the stack. */
    if (thing->markIfUnmarked(gcmarker->getMarkColor()))
        ScanBaseShape(gcmarker, thing);
}

static void
ScanShape(GCMarker* gcmarker, Shape* shape)
{
  restart:
    PushMarkStack(gcmarker, shape->base());

    const BarrieredBase<jsid>& id = shape->propidRef();
    if (JSID_IS_STRING(id))
        PushMarkStack(gcmarker, JSID_TO_STRING(id));
    else if (JSID_IS_SYMBOL(id))
        PushMarkStack(gcmarker, JSID_TO_SYMBOL(id));

    if (shape->hasGetterObject())
        MaybePushMarkStackBetweenSlices(gcmarker, shape->getterObject());

    if (shape->hasSetterObject())
        MaybePushMarkStackBetweenSlices(gcmarker, shape->setterObject());

    shape = shape->previous();
    if (shape && shape->markIfUnmarked(gcmarker->getMarkColor()))
        goto restart;
}

static inline void
ScanBaseShape(GCMarker* gcmarker, BaseShape* base)
{
    base->assertConsistency();

    base->compartment()->mark();

    if (JSObject* parent = base->getObjectParent()) {
        MaybePushMarkStackBetweenSlices(gcmarker, parent);
    } else if (GlobalObject* global = base->compartment()->unsafeUnbarrieredMaybeGlobal()) {
        PushMarkStack(gcmarker, global);
    }

    if (JSObject* metadata = base->getObjectMetadata())
        MaybePushMarkStackBetweenSlices(gcmarker, metadata);

    /*
     * All children of the owned base shape are consistent with its
     * unowned one, thus we do not need to trace through children of the
     * unowned base shape.
     */
    if (base->isOwned()) {
        UnownedBaseShape* unowned = base->baseUnowned();
        MOZ_ASSERT(base->compartment() == unowned->compartment());
        unowned->markIfUnmarked(gcmarker->getMarkColor());
    }
}

static inline void
ScanLinearString(GCMarker* gcmarker, JSLinearString* str)
{
    JS_COMPARTMENT_ASSERT_STR(gcmarker->runtime(), str);
    MOZ_ASSERT(str->isMarked());

    /*
     * Add extra asserts to confirm the static type to detect incorrect string
     * mutations.
     */
    MOZ_ASSERT(str->JSString::isLinear());
    while (str->hasBase()) {
        str = str->base();
        MOZ_ASSERT(str->JSString::isLinear());
        if (str->isPermanentAtom())
            break;
        JS_COMPARTMENT_ASSERT_STR(gcmarker->runtime(), str);
        if (!str->markIfUnmarked())
            break;
    }
}

/*
 * The function tries to scan the whole rope tree using the marking stack as
 * temporary storage. If that becomes full, the unscanned ropes are added to
 * the delayed marking list. When the function returns, the marking stack is
 * at the same depth as it was on entry. This way we avoid using tags when
 * pushing ropes to the stack as ropes never leaks to other users of the
 * stack. This also assumes that a rope can only point to other ropes or
 * linear strings, it cannot refer to GC things of other types.
 */
static void
ScanRope(GCMarker* gcmarker, JSRope* rope)
{
    ptrdiff_t savedPos = gcmarker->stack.position();
    JS_DIAGNOSTICS_ASSERT(GetGCThingTraceKind(rope) == JSTRACE_STRING);
    for (;;) {
        JS_DIAGNOSTICS_ASSERT(GetGCThingTraceKind(rope) == JSTRACE_STRING);
        JS_DIAGNOSTICS_ASSERT(rope->JSString::isRope());
        JS_COMPARTMENT_ASSERT_STR(gcmarker->runtime(), rope);
        MOZ_ASSERT(rope->isMarked());
        JSRope* next = nullptr;

        JSString* right = rope->rightChild();
        if (!right->isPermanentAtom() && right->markIfUnmarked()) {
            if (right->isLinear())
                ScanLinearString(gcmarker, &right->asLinear());
            else
                next = &right->asRope();
        }

        JSString* left = rope->leftChild();
        if (!left->isPermanentAtom() && left->markIfUnmarked()) {
            if (left->isLinear()) {
                ScanLinearString(gcmarker, &left->asLinear());
            } else {
                /*
                 * When both children are ropes, set aside the right one to
                 * scan it later.
                 */
                if (next && !gcmarker->stack.push(reinterpret_cast<uintptr_t>(next)))
                    gcmarker->delayMarkingChildren(next);
                next = &left->asRope();
            }
        }
        if (next) {
            rope = next;
        } else if (savedPos != gcmarker->stack.position()) {
            MOZ_ASSERT(savedPos < gcmarker->stack.position());
            rope = reinterpret_cast<JSRope*>(gcmarker->stack.pop());
        } else {
            break;
        }
    }
    MOZ_ASSERT(savedPos == gcmarker->stack.position());
 }

static inline void
ScanString(GCMarker* gcmarker, JSString* str)
{
    if (str->isLinear())
        ScanLinearString(gcmarker, &str->asLinear());
    else
        ScanRope(gcmarker, &str->asRope());
}

static inline void
PushMarkStack(GCMarker* gcmarker, JSString* str)
{
    // Permanent atoms might not be associated with this runtime.
    if (str->isPermanentAtom())
        return;

    JS_COMPARTMENT_ASSERT_STR(gcmarker->runtime(), str);

    /*
     * As string can only refer to other strings we fully scan its GC graph
     * using the explicit stack when navigating the rope tree to avoid
     * dealing with strings on the stack in drainMarkStack.
     */
    if (str->markIfUnmarked())
        ScanString(gcmarker, str);
}

static inline void
ScanSymbol(GCMarker* gcmarker, JS::Symbol* sym)
{
    if (JSString* desc = sym->description())
        PushMarkStack(gcmarker, desc);
}

static inline void
PushMarkStack(GCMarker* gcmarker, JS::Symbol* sym)
{
    // Well-known symbols might not be associated with this runtime.
    if (sym->isWellKnownSymbol())
        return;

    JS_COMPARTMENT_ASSERT_SYM(gcmarker->runtime(), sym);
    MOZ_ASSERT(!IsInsideNursery(sym));

    if (sym->markIfUnmarked())
        ScanSymbol(gcmarker, sym);
}

void
gc::MarkChildren(JSTracer* trc, JSObject* obj)
{
    obj->markChildren(trc);
}

static void
gc::MarkChildren(JSTracer* trc, JSString* str)
{
    if (str->hasBase())
        str->markBase(trc);
    else if (str->isRope())
        str->asRope().markChildren(trc);
}

static void
gc::MarkChildren(JSTracer* trc, JS::Symbol* sym)
{
    sym->markChildren(trc);
}

static void
gc::MarkChildren(JSTracer* trc, JSScript* script)
{
    script->markChildren(trc);
}

static void
gc::MarkChildren(JSTracer* trc, LazyScript* lazy)
{
    lazy->markChildren(trc);
}

static void
gc::MarkChildren(JSTracer* trc, Shape* shape)
{
    shape->markChildren(trc);
}

static void
gc::MarkChildren(JSTracer* trc, BaseShape* base)
{
    base->markChildren(trc);
}

/*
 * This function is used by the cycle collector to trace through the
 * children of a BaseShape (and its baseUnowned(), if any). The cycle
 * collector does not directly care about BaseShapes, so only the
 * parent is marked. Furthermore, the parent is marked only if it isn't the
 * same as prevParent, which will be updated to the current shape's parent.
 */
static inline void
MarkCycleCollectorChildren(JSTracer* trc, BaseShape* base, JSObject** prevParent)
{
    MOZ_ASSERT(base);

    /*
     * The cycle collector does not need to trace unowned base shapes,
     * as they have the same parent as the original base shape.
     */
    base->assertConsistency();

    JSObject* parent = base->getObjectParent();
    if (parent && parent != *prevParent) {
        MarkObjectUnbarriered(trc, &parent, "parent");
        MOZ_ASSERT(parent == base->getObjectParent());
        *prevParent = parent;
    }
}

/*
 * This function is used by the cycle collector to trace through a
 * shape. The cycle collector does not care about shapes or base
 * shapes, so those are not marked. Instead, any shapes or base shapes
 * that are encountered have their children marked. Stack space is
 * bounded. If two shapes in a row have the same parent pointer, the
 * parent pointer will only be marked once.
 */
void
gc::MarkCycleCollectorChildren(JSTracer* trc, Shape* shape)
{
    JSObject* prevParent = nullptr;
    do {
        MarkCycleCollectorChildren(trc, shape->base(), &prevParent);
        MarkId(trc, &shape->propidRef(), "propid");

        if (shape->hasGetterObject()) {
            JSObject* tmp = shape->getterObject();
            MarkObjectUnbarriered(trc, &tmp, "getter");
            MOZ_ASSERT(tmp == shape->getterObject());
        }

        if (shape->hasSetterObject()) {
            JSObject* tmp = shape->setterObject();
            MarkObjectUnbarriered(trc, &tmp, "setter");
            MOZ_ASSERT(tmp == shape->setterObject());
        }

        shape = shape->previous();
    } while (shape);
}

static void
ScanObjectGroup(GCMarker* gcmarker, ObjectGroup* group)
{
    unsigned count = group->getPropertyCount();
    for (unsigned i = 0; i < count; i++) {
        if (ObjectGroup::Property* prop = group->getProperty(i))
            MarkId(gcmarker, &prop->id, "ObjectGroup property id");
    }

    if (group->proto().isObject())
        PushMarkStack(gcmarker, group->proto().toObject());

    if (group->singleton() && !group->lazy())
        PushMarkStack(gcmarker, group->singleton());

    if (group->newScript())
        group->newScript()->trace(gcmarker);

    if (group->maybeUnboxedLayout())
        group->unboxedLayout().trace(gcmarker);

    if (ObjectGroup* unboxedGroup = group->maybeOriginalUnboxedGroup())
        PushMarkStack(gcmarker, unboxedGroup);

    if (TypeDescr* descr = group->maybeTypeDescr())
        PushMarkStack(gcmarker, descr);

    if (JSFunction* fun = group->maybeInterpretedFunction())
        PushMarkStack(gcmarker, fun);
}

static void
gc::MarkChildren(JSTracer* trc, ObjectGroup* group)
{
    unsigned count = group->getPropertyCount();
    for (unsigned i = 0; i < count; i++) {
        if (ObjectGroup::Property* prop = group->getProperty(i))
            MarkId(trc, &prop->id, "group_property");
    }

    if (group->proto().isObject())
        MarkObject(trc, &group->protoRaw(), "group_proto");

    if (group->singleton() && !group->lazy())
        MarkObject(trc, &group->singletonRaw(), "group_singleton");

    if (group->newScript())
        group->newScript()->trace(trc);

    if (group->maybeUnboxedLayout())
        group->unboxedLayout().trace(trc);

    if (ObjectGroup* unboxedGroup = group->maybeOriginalUnboxedGroup()) {
        MarkObjectGroupUnbarriered(trc, &unboxedGroup, "group_original_unboxed_group");
        group->setOriginalUnboxedGroup(unboxedGroup);
    }

    if (JSObject* descr = group->maybeTypeDescr()) {
        MarkObjectUnbarriered(trc, &descr, "group_type_descr");
        group->setTypeDescr(&descr->as<TypeDescr>());
    }

    if (JSObject* fun = group->maybeInterpretedFunction()) {
        MarkObjectUnbarriered(trc, &fun, "group_function");
        group->setInterpretedFunction(&fun->as<JSFunction>());
    }
}

static void
gc::MarkChildren(JSTracer* trc, jit::JitCode* code)
{
    code->trace(trc);
}

template<typename T>
static void
PushArenaTyped(GCMarker* gcmarker, ArenaHeader* aheader)
{
    for (ArenaCellIterUnderGC i(aheader); !i.done(); i.next())
        PushMarkStack(gcmarker, i.get<T>());
}

void
gc::PushArena(GCMarker* gcmarker, ArenaHeader* aheader)
{
    switch (MapAllocToTraceKind(aheader->getAllocKind())) {
      case JSTRACE_OBJECT:
        PushArenaTyped<JSObject>(gcmarker, aheader);
        break;

      case JSTRACE_SCRIPT:
        PushArenaTyped<JSScript>(gcmarker, aheader);
        break;

      case JSTRACE_STRING:
        PushArenaTyped<JSString>(gcmarker, aheader);
        break;

      case JSTRACE_SYMBOL:
        PushArenaTyped<JS::Symbol>(gcmarker, aheader);
        break;

      case JSTRACE_BASE_SHAPE:
        PushArenaTyped<js::BaseShape>(gcmarker, aheader);
        break;

      case JSTRACE_JITCODE:
        PushArenaTyped<js::jit::JitCode>(gcmarker, aheader);
        break;

      case JSTRACE_LAZY_SCRIPT:
        PushArenaTyped<LazyScript>(gcmarker, aheader);
        break;

      case JSTRACE_SHAPE:
        PushArenaTyped<js::Shape>(gcmarker, aheader);
        break;

      case JSTRACE_OBJECT_GROUP:
        PushArenaTyped<js::ObjectGroup>(gcmarker, aheader);
        break;

      default:
        MOZ_CRASH("Invalid trace kind in PushArena.");
    }
}

struct SlotArrayLayout
{
    union {
        HeapSlot* end;
        uintptr_t kind;
    };
    union {
        HeapSlot* start;
        uintptr_t index;
    };
    NativeObject* obj;

    static void staticAsserts() {
        /* This should have the same layout as three mark stack items. */
        JS_STATIC_ASSERT(sizeof(SlotArrayLayout) == 3 * sizeof(uintptr_t));
    }
};

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
    for (uintptr_t* p = stack.tos_; p > stack.stack_; ) {
        uintptr_t tag = *--p & StackTagMask;
        if (tag == ValueArrayTag) {
            *p &= ~StackTagMask;
            p -= 2;
            SlotArrayLayout* arr = reinterpret_cast<SlotArrayLayout*>(p);
            NativeObject* obj = arr->obj;
            MOZ_ASSERT(obj->isNative());

            HeapSlot* vp = obj->getDenseElementsAllowCopyOnWrite();
            if (arr->end == vp + obj->getDenseInitializedLength()) {
                MOZ_ASSERT(arr->start >= vp);
                arr->index = arr->start - vp;
                arr->kind = HeapSlot::Element;
            } else {
                HeapSlot* vp = obj->fixedSlots();
                unsigned nfixed = obj->numFixedSlots();
                if (arr->start == arr->end) {
                    arr->index = obj->slotSpan();
                } else if (arr->start >= vp && arr->start < vp + nfixed) {
                    MOZ_ASSERT(arr->end == vp + Min(nfixed, obj->slotSpan()));
                    arr->index = arr->start - vp;
                } else {
                    MOZ_ASSERT(arr->start >= obj->slots_ &&
                               arr->end == obj->slots_ + obj->slotSpan() - nfixed);
                    arr->index = (arr->start - obj->slots_) + nfixed;
                }
                arr->kind = HeapSlot::Slot;
            }
            p[2] |= SavedValueArrayTag;
        } else if (tag == SavedValueArrayTag) {
            p -= 2;
        }
    }
}

bool
GCMarker::restoreValueArray(NativeObject* obj, void** vpp, void** endp)
{
    uintptr_t start = stack.pop();
    HeapSlot::Kind kind = (HeapSlot::Kind) stack.pop();

    if (kind == HeapSlot::Element) {
        if (!obj->is<ArrayObject>())
            return false;

        uint32_t initlen = obj->getDenseInitializedLength();
        HeapSlot* vp = obj->getDenseElementsAllowCopyOnWrite();
        if (start < initlen) {
            *vpp = vp + start;
            *endp = vp + initlen;
        } else {
            /* The object shrunk, in which case no scanning is needed. */
            *vpp = *endp = vp;
        }
    } else {
        MOZ_ASSERT(kind == HeapSlot::Slot);
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

void
GCMarker::processMarkStackOther(uintptr_t tag, uintptr_t addr)
{
    if (tag == GroupTag) {
        ScanObjectGroup(this, reinterpret_cast<ObjectGroup*>(addr));
    } else if (tag == SavedValueArrayTag) {
        MOZ_ASSERT(!(addr & CellMask));
        NativeObject* obj = reinterpret_cast<NativeObject*>(addr);
        HeapValue* vp, *end;
        if (restoreValueArray(obj, (void**)&vp, (void**)&end))
            pushValueArray(obj, vp, end);
        else
            pushObject(obj);
    } else if (tag == JitCodeTag) {
        MarkChildren(this, reinterpret_cast<jit::JitCode*>(addr));
    }
}

MOZ_ALWAYS_INLINE void
GCMarker::markAndScanString(JSObject* source, JSString* str)
{
    if (!str->isPermanentAtom()) {
        JS_COMPARTMENT_ASSERT_STR(runtime(), str);
        MOZ_ASSERT(runtime()->isAtomsZone(str->zone()) || str->zone() == source->zone());
        if (str->markIfUnmarked())
            ScanString(this, str);
    }
}

MOZ_ALWAYS_INLINE void
GCMarker::markAndScanSymbol(JSObject* source, JS::Symbol* sym)
{
    if (!sym->isWellKnownSymbol()) {
        JS_COMPARTMENT_ASSERT_SYM(runtime(), sym);
        MOZ_ASSERT(runtime()->isAtomsZone(sym->zone()) || sym->zone() == source->zone());
        if (sym->markIfUnmarked())
            ScanSymbol(this, sym);
    }
}

MOZ_ALWAYS_INLINE bool
GCMarker::markObject(JSObject* source, JSObject* obj)
{
    JS_COMPARTMENT_ASSERT(runtime(), obj);
    MOZ_ASSERT(obj->compartment() == source->compartment());
    return obj->asTenured().markIfUnmarked(getMarkColor());
}

inline void
GCMarker::processMarkStackTop(SliceBudget& budget)
{
    /*
     * The function uses explicit goto and implements the scanning of the
     * object directly. It allows to eliminate the tail recursion and
     * significantly improve the marking performance, see bug 641025.
     */
    HeapSlot* vp, *end;
    JSObject* obj;

    const int32_t* unboxedTraceList;
    uint8_t* unboxedMemory;

    uintptr_t addr = stack.pop();
    uintptr_t tag = addr & StackTagMask;
    addr &= ~StackTagMask;

    if (tag == ValueArrayTag) {
        JS_STATIC_ASSERT(ValueArrayTag == 0);
        MOZ_ASSERT(!(addr & CellMask));
        obj = reinterpret_cast<JSObject*>(addr);
        uintptr_t addr2 = stack.pop();
        uintptr_t addr3 = stack.pop();
        MOZ_ASSERT(addr2 <= addr3);
        MOZ_ASSERT((addr3 - addr2) % sizeof(Value) == 0);
        vp = reinterpret_cast<HeapSlot*>(addr2);
        end = reinterpret_cast<HeapSlot*>(addr3);
        goto scan_value_array;
    }

    if (tag == ObjectTag) {
        obj = reinterpret_cast<JSObject*>(addr);
        JS_COMPARTMENT_ASSERT(runtime(), obj);
        goto scan_obj;
    }

    processMarkStackOther(tag, addr);
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
            markAndScanString(obj, v.toString());
        } else if (v.isObject()) {
            JSObject* obj2 = &v.toObject();
            if (markObject(obj, obj2)) {
                pushValueArray(obj, vp, end);
                obj = obj2;
                goto scan_obj;
            }
        } else if (v.isSymbol()) {
            markAndScanSymbol(obj, v.toSymbol());
        }
    }
    return;

  scan_unboxed:
    {
        while (*unboxedTraceList != -1) {
            JSString* str = *reinterpret_cast<JSString**>(unboxedMemory + *unboxedTraceList);
            markAndScanString(obj, str);
            unboxedTraceList++;
        }
        unboxedTraceList++;
        while (*unboxedTraceList != -1) {
            JSObject* obj2 = *reinterpret_cast<JSObject**>(unboxedMemory + *unboxedTraceList);
            if (obj2 && markObject(obj, obj2))
                pushObject(obj2);
            unboxedTraceList++;
        }
        unboxedTraceList++;
        while (*unboxedTraceList != -1) {
            const Value& v = *reinterpret_cast<Value*>(unboxedMemory + *unboxedTraceList);
            if (v.isString()) {
                markAndScanString(obj, v.toString());
            } else if (v.isObject()) {
                JSObject* obj2 = &v.toObject();
                if (markObject(obj, obj2))
                    pushObject(obj2);
            } else if (v.isSymbol()) {
                markAndScanSymbol(obj, v.toSymbol());
            }
            unboxedTraceList++;
        }
        return;
    }

  scan_obj:
    {
        JS_COMPARTMENT_ASSERT(runtime(), obj);

        budget.step();
        if (budget.isOverBudget()) {
            pushObject(obj);
            return;
        }

        ObjectGroup* group = obj->groupFromGC();
        PushMarkStack(this, group);

        Shape* shape = obj->lastProperty();
        PushMarkStack(this, shape);

        /* Call the trace hook if necessary. */
        const Class* clasp = group->clasp();
        if (clasp->trace) {
            // Global objects all have the same trace hook. That hook is safe without barriers
            // if the global has no custom trace hook of its own, or has been moved to a different
            // compartment, and so can't have one.
            MOZ_ASSERT_IF(!(clasp->trace == JS_GlobalObjectTraceHook &&
                            (!obj->compartment()->options().getTrace() || !obj->isOwnGlobal())),
                          clasp->flags & JSCLASS_IMPLEMENTS_BARRIERS);
            if (clasp->trace == InlineTypedObject::obj_trace) {
                TypeDescr* descr = &obj->as<InlineOpaqueTypedObject>().typeDescr();
                if (!descr->hasTraceList())
                    return;
                unboxedTraceList = descr->traceList();
                unboxedMemory = obj->as<InlineOpaqueTypedObject>().inlineTypedMem();
                goto scan_unboxed;
            }
            if (clasp == &UnboxedPlainObject::class_) {
                const UnboxedLayout& layout = obj->as<UnboxedPlainObject>().layout();
                unboxedTraceList = layout.traceList();
                if (!unboxedTraceList)
                    return;
                unboxedMemory = obj->as<UnboxedPlainObject>().data();
                goto scan_unboxed;
            }
            clasp->trace(this, obj);
        }

        if (!shape->isNative())
            return;

        NativeObject* nobj = &obj->as<NativeObject>();
        unsigned nslots = nobj->slotSpan();

        do {
            if (nobj->hasEmptyElements())
                break;

            if (nobj->denseElementsAreCopyOnWrite()) {
                JSObject* owner = nobj->getElementsHeader()->ownerObject();
                if (owner != nobj) {
                    PushMarkStack(this, owner);
                    break;
                }
            }

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

bool
GCMarker::drainMarkStack(SliceBudget& budget)
{
#ifdef DEBUG
    struct AutoCheckCompartment {
        bool& flag;
        explicit AutoCheckCompartment(bool& comparmentCheckFlag) : flag(comparmentCheckFlag) {
            MOZ_ASSERT(!flag);
            flag = true;
        }
        ~AutoCheckCompartment() { flag = false; }
    } acc(strictCompartmentChecking);
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

void
js::TraceChildren(JSTracer* trc, void* thing, JSGCTraceKind kind)
{
    switch (kind) {
      case JSTRACE_OBJECT:
        MarkChildren(trc, static_cast<JSObject*>(thing));
        break;

      case JSTRACE_SCRIPT:
        MarkChildren(trc, static_cast<JSScript*>(thing));
        break;

      case JSTRACE_STRING:
        MarkChildren(trc, static_cast<JSString*>(thing));
        break;

      case JSTRACE_SYMBOL:
        MarkChildren(trc, static_cast<JS::Symbol*>(thing));
        break;

      case JSTRACE_BASE_SHAPE:
        MarkChildren(trc, static_cast<BaseShape*>(thing));
        break;

      case JSTRACE_JITCODE:
        MarkChildren(trc, (js::jit::JitCode*)thing);
        break;

      case JSTRACE_LAZY_SCRIPT:
        MarkChildren(trc, static_cast<LazyScript*>(thing));
        break;

      case JSTRACE_SHAPE:
        MarkChildren(trc, static_cast<Shape*>(thing));
        break;

      case JSTRACE_OBJECT_GROUP:
        MarkChildren(trc, (ObjectGroup*)thing);
        break;

      default:
        MOZ_CRASH("Invalid trace kind in TraceChildren.");
    }
}

#ifdef DEBUG
static void
AssertNonGrayGCThing(JSTracer* trc, void** thingp, JSGCTraceKind kind)
{
    DebugOnly<Cell*> thing(static_cast<Cell*>(*thingp));
    MOZ_ASSERT_IF(thing->isTenured(), !thing->asTenured().isMarked(js::gc::GRAY));
}
#endif

static void
UnmarkGrayChildren(JSTracer* trc, void** thingp, JSGCTraceKind kind);

struct UnmarkGrayTracer : public JSTracer
{
    /*
     * We set eagerlyTraceWeakMaps to false because the cycle collector will fix
     * up any color mismatches involving weakmaps when it runs.
     */
    explicit UnmarkGrayTracer(JSRuntime* rt)
      : JSTracer(rt, UnmarkGrayChildren, DoNotTraceWeakMaps),
        tracingShape(false),
        previousShape(nullptr),
        unmarkedAny(false)
    {}

    UnmarkGrayTracer(JSTracer* trc, bool tracingShape)
      : JSTracer(trc->runtime(), UnmarkGrayChildren, DoNotTraceWeakMaps),
        tracingShape(tracingShape),
        previousShape(nullptr),
        unmarkedAny(false)
    {}

    /* True iff we are tracing the immediate children of a shape. */
    bool tracingShape;

    /* If tracingShape, shape child or nullptr. Otherwise, nullptr. */
    Shape* previousShape;

    /* Whether we unmarked anything. */
    bool unmarkedAny;
};

/*
 * The GC and CC are run independently. Consequently, the following sequence of
 * events can occur:
 * 1. GC runs and marks an object gray.
 * 2. Some JS code runs that creates a pointer from a JS root to the gray
 *    object. If we re-ran a GC at this point, the object would now be black.
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
 * to its value, and from an object being watched by a watchpoint to the
 * watchpoint's closure. These "implicit edges" are represented in some other
 * container object, such as the weakmap or the watchpoint itself. In these
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
static void
UnmarkGrayChildren(JSTracer* trc, void** thingp, JSGCTraceKind kind)
{
    int stackDummy;
    if (!JS_CHECK_STACK_SIZE(trc->runtime()->mainThread.nativeStackLimit[StackForSystemCode],
                             &stackDummy))
    {
        /*
         * If we run out of stack, we take a more drastic measure: require that
         * we GC again before the next CC.
         */
        trc->runtime()->gc.setGrayBitsInvalid();
        return;
    }

    Cell* cell = static_cast<Cell*>(*thingp);

    // Cells in the nursery cannot be gray, and therefore must necessarily point
    // to only black edges.
    if (!cell->isTenured()) {
#ifdef DEBUG
        JSTracer nongray(trc->runtime(), AssertNonGrayGCThing);
        TraceChildren(&nongray, cell, kind);
#endif
        return;
    }

    TenuredCell& tenured = cell->asTenured();
    if (!tenured.isMarked(js::gc::GRAY))
        return;
    tenured.unmark(js::gc::GRAY);

    UnmarkGrayTracer* tracer = static_cast<UnmarkGrayTracer*>(trc);
    tracer->unmarkedAny = true;

    // Trace children of |tenured|. If |tenured| and its parent are both
    // shapes, |tenured| will get saved to mPreviousShape without being traced.
    // The parent will later trace |tenured|. This is done to avoid increasing
    // the stack depth during shape tracing. It is safe to do because a shape
    // can only have one child that is a shape.
    UnmarkGrayTracer childTracer(tracer, kind == JSTRACE_SHAPE);

    if (kind != JSTRACE_SHAPE) {
        TraceChildren(&childTracer, &tenured, kind);
        MOZ_ASSERT(!childTracer.previousShape);
        tracer->unmarkedAny |= childTracer.unmarkedAny;
        return;
    }

    MOZ_ASSERT(kind == JSTRACE_SHAPE);
    Shape* shape = static_cast<Shape*>(&tenured);
    if (tracer->tracingShape) {
        MOZ_ASSERT(!tracer->previousShape);
        tracer->previousShape = shape;
        return;
    }

    do {
        MOZ_ASSERT(!shape->isMarked(js::gc::GRAY));
        TraceChildren(&childTracer, shape, JSTRACE_SHAPE);
        shape = childTracer.previousShape;
        childTracer.previousShape = nullptr;
    } while (shape);
    tracer->unmarkedAny |= childTracer.unmarkedAny;
}

bool
js::UnmarkGrayCellRecursively(gc::Cell* cell, JSGCTraceKind kind)
{
    MOZ_ASSERT(cell);

    JSRuntime* rt = cell->runtimeFromMainThread();

    // When the ReadBarriered type is used in a HashTable, it is difficult or
    // impossible to suppress the implicit cast operator while iterating for GC.
    if (rt->isHeapBusy())
        return false;

    bool unmarkedArg = false;
    if (cell->isTenured()) {
        if (!cell->asTenured().isMarked(GRAY))
            return false;

        cell->asTenured().unmark(GRAY);
        unmarkedArg = true;
    }

    UnmarkGrayTracer trc(rt);
    TraceChildren(&trc, cell, kind);

    return unmarkedArg || trc.unmarkedAny;
}

bool
js::UnmarkGrayShapeRecursively(Shape* shape)
{
    return js::UnmarkGrayCellRecursively(shape, JSTRACE_SHAPE);
}

JS_FRIEND_API(bool)
JS::UnmarkGrayGCThingRecursively(JS::GCCellPtr thing)
{
    return js::UnmarkGrayCellRecursively(thing.asCell(), thing.kind());
}
