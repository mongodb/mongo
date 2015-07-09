/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jsgcinlines_h
#define jsgcinlines_h

#include "jsgc.h"

#include "gc/GCTrace.h"
#include "gc/Zone.h"

namespace js {

class Shape;

namespace gc {

static inline AllocKind
GetGCObjectKind(const Class* clasp)
{
    if (clasp == FunctionClassPtr)
        return JSFunction::FinalizeKind;
    uint32_t nslots = JSCLASS_RESERVED_SLOTS(clasp);
    if (clasp->flags & JSCLASS_HAS_PRIVATE)
        nslots++;
    return GetGCObjectKind(nslots);
}

inline bool
ShouldNurseryAllocateObject(const Nursery& nursery, InitialHeap heap)
{
    return nursery.isEnabled() && heap != TenuredHeap;
}

inline JSGCTraceKind
GetGCThingTraceKind(const void* thing)
{
    MOZ_ASSERT(thing);
    const Cell* cell = static_cast<const Cell*>(thing);
    if (IsInsideNursery(cell))
        return JSTRACE_OBJECT;
    return MapAllocToTraceKind(cell->asTenured().getAllocKind());
}

inline void
GCRuntime::poke()
{
    poked = true;

#ifdef JS_GC_ZEAL
    /* Schedule a GC to happen "soon" after a GC poke. */
    if (zealMode == ZealPokeValue)
        nextScheduled = 1;
#endif
}

class ArenaIter
{
    ArenaHeader* aheader;
    ArenaHeader* unsweptHeader;
    ArenaHeader* sweptHeader;

  public:
    ArenaIter() {
        aheader = nullptr;
        unsweptHeader = nullptr;
        sweptHeader = nullptr;
    }

    ArenaIter(JS::Zone* zone, AllocKind kind) {
        init(zone, kind);
    }

    void init(JS::Zone* zone, AllocKind kind) {
        aheader = zone->arenas.getFirstArena(kind);
        unsweptHeader = zone->arenas.getFirstArenaToSweep(kind);
        sweptHeader = zone->arenas.getFirstSweptArena(kind);
        if (!unsweptHeader) {
            unsweptHeader = sweptHeader;
            sweptHeader = nullptr;
        }
        if (!aheader) {
            aheader = unsweptHeader;
            unsweptHeader = sweptHeader;
            sweptHeader = nullptr;
        }
    }

    bool done() const {
        return !aheader;
    }

    ArenaHeader* get() const {
        return aheader;
    }

    void next() {
        MOZ_ASSERT(!done());
        aheader = aheader->next;
        if (!aheader) {
            aheader = unsweptHeader;
            unsweptHeader = sweptHeader;
            sweptHeader = nullptr;
        }
    }
};

class ArenaCellIterImpl
{
    // These three are set in initUnsynchronized().
    size_t firstThingOffset;
    size_t thingSize;
#ifdef DEBUG
    bool isInited;
#endif

    // These three are set in reset() (which is called by init()).
    FreeSpan span;
    uintptr_t thing;
    uintptr_t limit;

    // Upon entry, |thing| points to any thing (free or used) and finds the
    // first used thing, which may be |thing|.
    void moveForwardIfFree() {
        MOZ_ASSERT(!done());
        MOZ_ASSERT(thing);
        // Note: if |span| is empty, this test will fail, which is what we want
        // -- |span| being empty means that we're past the end of the last free
        // thing, all the remaining things in the arena are used, and we'll
        // never need to move forward.
        if (thing == span.first) {
            thing = span.last + thingSize;
            span = *span.nextSpan();
        }
    }

  public:
    ArenaCellIterImpl()
      : firstThingOffset(0)     // Squelch
      , thingSize(0)            //   warnings
      , limit(0)
    {
    }

    void initUnsynchronized(ArenaHeader* aheader) {
        AllocKind kind = aheader->getAllocKind();
#ifdef DEBUG
        isInited = true;
#endif
        firstThingOffset = Arena::firstThingOffset(kind);
        thingSize = Arena::thingSize(kind);
        reset(aheader);
    }

    void init(ArenaHeader* aheader) {
#ifdef DEBUG
        AllocKind kind = aheader->getAllocKind();
        MOZ_ASSERT(aheader->zone->arenas.isSynchronizedFreeList(kind));
#endif
        initUnsynchronized(aheader);
    }

    // Use this to move from an Arena of a particular kind to another Arena of
    // the same kind.
    void reset(ArenaHeader* aheader) {
        MOZ_ASSERT(isInited);
        span = aheader->getFirstFreeSpan();
        uintptr_t arenaAddr = aheader->arenaAddress();
        thing = arenaAddr + firstThingOffset;
        limit = arenaAddr + ArenaSize;
        moveForwardIfFree();
    }

    bool done() const {
        return thing == limit;
    }

    TenuredCell* getCell() const {
        MOZ_ASSERT(!done());
        return reinterpret_cast<TenuredCell*>(thing);
    }

    template<typename T> T* get() const {
        MOZ_ASSERT(!done());
        return static_cast<T*>(getCell());
    }

    void next() {
        MOZ_ASSERT(!done());
        thing += thingSize;
        if (thing < limit)
            moveForwardIfFree();
    }
};

template<>
JSObject*
ArenaCellIterImpl::get<JSObject>() const;

class ArenaCellIterUnderGC : public ArenaCellIterImpl
{
  public:
    explicit ArenaCellIterUnderGC(ArenaHeader* aheader) {
        MOZ_ASSERT(aheader->zone->runtimeFromAnyThread()->isHeapBusy());
        init(aheader);
    }
};

class ArenaCellIterUnderFinalize : public ArenaCellIterImpl
{
  public:
    explicit ArenaCellIterUnderFinalize(ArenaHeader* aheader) {
        initUnsynchronized(aheader);
    }
};

class ZoneCellIterImpl
{
    ArenaIter arenaIter;
    ArenaCellIterImpl cellIter;

  protected:
    ZoneCellIterImpl() {}

    void init(JS::Zone* zone, AllocKind kind) {
        MOZ_ASSERT(zone->arenas.isSynchronizedFreeList(kind));
        arenaIter.init(zone, kind);
        if (!arenaIter.done())
            cellIter.init(arenaIter.get());
    }

  public:
    bool done() const {
        return arenaIter.done();
    }

    template<typename T> T* get() const {
        MOZ_ASSERT(!done());
        return cellIter.get<T>();
    }

    Cell* getCell() const {
        MOZ_ASSERT(!done());
        return cellIter.getCell();
    }

    void next() {
        MOZ_ASSERT(!done());
        cellIter.next();
        if (cellIter.done()) {
            MOZ_ASSERT(!arenaIter.done());
            arenaIter.next();
            if (!arenaIter.done())
                cellIter.reset(arenaIter.get());
        }
    }
};

class ZoneCellIterUnderGC : public ZoneCellIterImpl
{
  public:
    ZoneCellIterUnderGC(JS::Zone* zone, AllocKind kind) {
        MOZ_ASSERT(zone->runtimeFromAnyThread()->gc.nursery.isEmpty());
        MOZ_ASSERT(zone->runtimeFromAnyThread()->isHeapBusy());
        init(zone, kind);
    }
};

class ZoneCellIter : public ZoneCellIterImpl
{
    JS::AutoAssertNoAlloc noAlloc;
    ArenaLists* lists;
    AllocKind kind;

  public:
    ZoneCellIter(JS::Zone* zone, AllocKind kind)
      : lists(&zone->arenas),
        kind(kind)
    {
        JSRuntime* rt = zone->runtimeFromMainThread();

        /*
         * We have a single-threaded runtime, so there's no need to protect
         * against other threads iterating or allocating. However, we do have
         * background finalization; we have to wait for this to finish if it's
         * currently active.
         */
        if (IsBackgroundFinalized(kind) &&
            zone->arenas.needBackgroundFinalizeWait(kind))
        {
            rt->gc.waitBackgroundSweepEnd();
        }

        /* Evict the nursery before iterating so we can see all things. */
        rt->gc.evictNursery();

        if (lists->isSynchronizedFreeList(kind)) {
            lists = nullptr;
        } else {
            MOZ_ASSERT(!rt->isHeapBusy());
            lists->copyFreeListToArena(kind);
        }

        /* Assert that no GCs can occur while a ZoneCellIter is live. */
        noAlloc.disallowAlloc(rt);

        init(zone, kind);
    }

    ~ZoneCellIter() {
        if (lists)
            lists->clearFreeListInArena(kind);
    }
};

class GCZonesIter
{
  private:
    ZonesIter zone;

  public:
    explicit GCZonesIter(JSRuntime* rt, ZoneSelector selector = WithAtoms)
      : zone(rt, selector)
    {
        if (!zone->isCollecting())
            next();
    }

    bool done() const { return zone.done(); }

    void next() {
        MOZ_ASSERT(!done());
        do {
            zone.next();
        } while (!zone.done() && !zone->isCollectingFromAnyThread());
    }

    JS::Zone* get() const {
        MOZ_ASSERT(!done());
        return zone;
    }

    operator JS::Zone*() const { return get(); }
    JS::Zone* operator->() const { return get(); }
};

typedef CompartmentsIterT<GCZonesIter> GCCompartmentsIter;

/* Iterates over all zones in the current zone group. */
class GCZoneGroupIter {
  private:
    JS::Zone* current;

  public:
    explicit GCZoneGroupIter(JSRuntime* rt) {
        MOZ_ASSERT(rt->isHeapBusy());
        current = rt->gc.getCurrentZoneGroup();
    }

    bool done() const { return !current; }

    void next() {
        MOZ_ASSERT(!done());
        current = current->nextNodeInGroup();
    }

    JS::Zone* get() const {
        MOZ_ASSERT(!done());
        return current;
    }

    operator JS::Zone*() const { return get(); }
    JS::Zone* operator->() const { return get(); }
};

typedef CompartmentsIterT<GCZoneGroupIter> GCCompartmentGroupIter;

/*
 * Attempt to allocate a new GC thing out of the nursery. If there is not enough
 * room in the nursery or there is an OOM, this method will return nullptr.
 */
template <AllowGC allowGC>
inline JSObject*
TryNewNurseryObject(JSContext* cx, size_t thingSize, size_t nDynamicSlots, const js::Class* clasp)
{
    MOZ_ASSERT(!IsAtomsCompartment(cx->compartment()));
    JSRuntime* rt = cx->runtime();
    Nursery& nursery = rt->gc.nursery;
    JSObject* obj = nursery.allocateObject(cx, thingSize, nDynamicSlots, clasp);
    if (obj)
        return obj;
    if (allowGC && !rt->mainThread.suppressGC) {
        cx->minorGC(JS::gcreason::OUT_OF_NURSERY);

        /* Exceeding gcMaxBytes while tenuring can disable the Nursery. */
        if (nursery.isEnabled()) {
            JSObject* obj = nursery.allocateObject(cx, thingSize, nDynamicSlots, clasp);
            MOZ_ASSERT(obj);
            return obj;
        }
    }
    return nullptr;
}

static inline bool
PossiblyFail()
{
    JS_OOM_POSSIBLY_FAIL_BOOL();
    return true;
}

static inline bool
GCIfNeeded(ExclusiveContext* cx)
{
    if (cx->isJSContext()) {
        JSContext* ncx = cx->asJSContext();
        JSRuntime* rt = ncx->runtime();

#ifdef JS_GC_ZEAL
        if (rt->gc.needZealousGC())
            rt->gc.runDebugGC();
#endif

        // Invoking the interrupt callback can fail and we can't usefully
        // handle that here. Just check in case we need to collect instead.
        if (rt->hasPendingInterrupt())
            rt->gc.gcIfRequested(ncx);

        // If we have grown past our GC heap threshold while in the middle of
        // an incremental GC, we're growing faster than we're GCing, so stop
        // the world and do a full, non-incremental GC right now, if possible.
        if (rt->gc.isIncrementalGCInProgress() &&
            cx->zone()->usage.gcBytes() > cx->zone()->threshold.gcTriggerBytes())
        {
            PrepareZoneForGC(cx->zone());
            AutoKeepAtoms keepAtoms(cx->perThreadData);
            rt->gc.gc(GC_NORMAL, JS::gcreason::INCREMENTAL_TOO_SLOW);
        }
    }

    return true;
}

template <AllowGC allowGC>
static inline bool
CheckAllocatorState(ExclusiveContext* cx, AllocKind kind)
{
    if (allowGC) {
        if (!GCIfNeeded(cx))
            return false;
    }

    if (!cx->isJSContext())
        return true;

    JSContext* ncx = cx->asJSContext();
    JSRuntime* rt = ncx->runtime();
#if defined(JS_GC_ZEAL) || defined(DEBUG)
    MOZ_ASSERT_IF(rt->isAtomsCompartment(ncx->compartment()),
                  kind == FINALIZE_STRING ||
                  kind == FINALIZE_FAT_INLINE_STRING ||
                  kind == FINALIZE_SYMBOL ||
                  kind == FINALIZE_JITCODE);
    MOZ_ASSERT(!rt->isHeapBusy());
    MOZ_ASSERT(rt->gc.isAllocAllowed());
#endif

    // Crash if we perform a GC action when it is not safe.
    if (allowGC && !rt->mainThread.suppressGC)
        JS::AutoAssertOnGC::VerifyIsSafeToGC(rt);

    // For testing out of memory conditions
    if (!PossiblyFail()) {
        js_ReportOutOfMemory(ncx);
        return false;
    }

    return true;
}

template <typename T>
static inline void
CheckIncrementalZoneState(ExclusiveContext* cx, T* t)
{
#ifdef DEBUG
    if (!cx->isJSContext())
        return;

    Zone* zone = cx->asJSContext()->zone();
    MOZ_ASSERT_IF(t && zone->wasGCStarted() && (zone->isGCMarking() || zone->isGCSweeping()),
                  t->asTenured().arenaHeader()->allocatedDuringIncremental);
#endif
}

/*
 * Allocate a new GC thing. After a successful allocation the caller must
 * fully initialize the thing before calling any function that can potentially
 * trigger GC. This will ensure that GC tracing never sees junk values stored
 * in the partially initialized thing.
 */

template <AllowGC allowGC>
inline JSObject*
AllocateObject(ExclusiveContext* cx, AllocKind kind, size_t nDynamicSlots, InitialHeap heap,
               const js::Class* clasp)
{
    size_t thingSize = Arena::thingSize(kind);

    MOZ_ASSERT(thingSize == Arena::thingSize(kind));
    MOZ_ASSERT(thingSize >= sizeof(JSObject));
    static_assert(sizeof(JSObject) >= CellSize,
                  "All allocations must be at least the allocator-imposed minimum size.");

    if (!CheckAllocatorState<allowGC>(cx, kind))
        return nullptr;

    if (cx->isJSContext() &&
        ShouldNurseryAllocateObject(cx->asJSContext()->nursery(), heap))
    {
        JSObject* obj = TryNewNurseryObject<allowGC>(cx->asJSContext(), thingSize, nDynamicSlots,
                                                     clasp);
        if (obj)
            return obj;
    }

    HeapSlot* slots = nullptr;
    if (nDynamicSlots) {
        slots = cx->zone()->pod_malloc<HeapSlot>(nDynamicSlots);
        if (MOZ_UNLIKELY(!slots))
            return nullptr;
        js::Debug_SetSlotRangeToCrashOnTouch(slots, nDynamicSlots);
    }

    JSObject* obj = reinterpret_cast<JSObject*>(cx->arenas()->allocateFromFreeList(kind, thingSize));
    if (!obj)
        obj = reinterpret_cast<JSObject*>(GCRuntime::refillFreeListFromAnyThread<allowGC>(cx, kind));

    if (obj)
        obj->setInitialSlotsMaybeNonNative(slots);
    else
        js_free(slots);

    CheckIncrementalZoneState(cx, obj);
    js::gc::TraceTenuredAlloc(obj, kind);
    return obj;
}

template <typename T, AllowGC allowGC>
inline T*
AllocateNonObject(ExclusiveContext* cx)
{
    static_assert(sizeof(T) >= CellSize,
                  "All allocations must be at least the allocator-imposed minimum size.");

    AllocKind kind = MapTypeToFinalizeKind<T>::kind;
    size_t thingSize = sizeof(T);

    MOZ_ASSERT(thingSize == Arena::thingSize(kind));
    if (!CheckAllocatorState<allowGC>(cx, kind))
        return nullptr;

    T* t = static_cast<T*>(cx->arenas()->allocateFromFreeList(kind, thingSize));
    if (!t)
        t = static_cast<T*>(GCRuntime::refillFreeListFromAnyThread<allowGC>(cx, kind));

    CheckIncrementalZoneState(cx, t);
    js::gc::TraceTenuredAlloc(t, kind);
    return t;
}

/*
 * When allocating for initialization from a cached object copy, we will
 * potentially destroy the cache entry we want to copy if we allow GC. On the
 * other hand, since these allocations are extremely common, we don't want to
 * delay GC from these allocation sites. Instead we allow the GC, but still
 * fail the allocation, forcing the non-cached path.
 */
template <AllowGC allowGC>
inline JSObject*
AllocateObjectForCacheHit(JSContext* cx, AllocKind kind, InitialHeap heap, const js::Class* clasp)
{
    if (ShouldNurseryAllocateObject(cx->nursery(), heap)) {
        size_t thingSize = Arena::thingSize(kind);

        MOZ_ASSERT(thingSize == Arena::thingSize(kind));
        if (!CheckAllocatorState<NoGC>(cx, kind))
            return nullptr;

        JSObject* obj = TryNewNurseryObject<NoGC>(cx, thingSize, 0, clasp);
        if (!obj && allowGC) {
            cx->minorGC(JS::gcreason::OUT_OF_NURSERY);
            return nullptr;
        }
        return obj;
    }

    JSObject* obj = AllocateObject<NoGC>(cx, kind, 0, heap, clasp);
    if (!obj && allowGC) {
        cx->runtime()->gc.maybeGC(cx->zone());
        return nullptr;
    }

    return obj;
}

inline bool
IsInsideGGCNursery(const js::gc::Cell* cell)
{
    if (!cell)
        return false;
    uintptr_t addr = uintptr_t(cell);
    addr &= ~js::gc::ChunkMask;
    addr |= js::gc::ChunkLocationOffset;
    uint32_t location = *reinterpret_cast<uint32_t*>(addr);
    MOZ_ASSERT(location != 0);
    return location & js::gc::ChunkLocationBitNursery;
}

} /* namespace gc */

template <js::AllowGC allowGC>
inline JSObject*
NewGCObject(js::ExclusiveContext* cx, js::gc::AllocKind kind, size_t nDynamicSlots,
            js::gc::InitialHeap heap, const js::Class* clasp)
{
    MOZ_ASSERT(kind >= js::gc::FINALIZE_OBJECT0 && kind <= js::gc::FINALIZE_OBJECT_LAST);
    return js::gc::AllocateObject<allowGC>(cx, kind, nDynamicSlots, heap, clasp);
}

template <js::AllowGC allowGC>
inline jit::JitCode*
NewJitCode(js::ExclusiveContext* cx)
{
    return gc::AllocateNonObject<jit::JitCode, allowGC>(cx);
}

inline
ObjectGroup*
NewObjectGroup(js::ExclusiveContext* cx)
{
    return gc::AllocateNonObject<ObjectGroup, js::CanGC>(cx);
}

template <js::AllowGC allowGC>
inline JSString*
NewGCString(js::ExclusiveContext* cx)
{
    return js::gc::AllocateNonObject<JSString, allowGC>(cx);
}

template <js::AllowGC allowGC>
inline JSFatInlineString*
NewGCFatInlineString(js::ExclusiveContext* cx)
{
    return js::gc::AllocateNonObject<JSFatInlineString, allowGC>(cx);
}

inline JSExternalString*
NewGCExternalString(js::ExclusiveContext* cx)
{
    return js::gc::AllocateNonObject<JSExternalString, js::CanGC>(cx);
}

inline Shape*
NewGCShape(ExclusiveContext* cx)
{
    return gc::AllocateNonObject<Shape, CanGC>(cx);
}

inline Shape*
NewGCAccessorShape(ExclusiveContext* cx)
{
    return gc::AllocateNonObject<AccessorShape, CanGC>(cx);
}

} /* namespace js */

inline JSScript*
js_NewGCScript(js::ExclusiveContext* cx)
{
    return js::gc::AllocateNonObject<JSScript, js::CanGC>(cx);
}

inline js::LazyScript*
js_NewGCLazyScript(js::ExclusiveContext* cx)
{
    return js::gc::AllocateNonObject<js::LazyScript, js::CanGC>(cx);
}

template <js::AllowGC allowGC>
inline js::BaseShape*
js_NewGCBaseShape(js::ExclusiveContext* cx)
{
    return js::gc::AllocateNonObject<js::BaseShape, allowGC>(cx);
}

#endif /* jsgcinlines_h */
