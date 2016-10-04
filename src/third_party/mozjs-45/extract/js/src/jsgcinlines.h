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
namespace gc {

static inline AllocKind
GetGCObjectKind(const Class* clasp)
{
    if (clasp == FunctionClassPtr)
        return AllocKind::FUNCTION;
    uint32_t nslots = JSCLASS_RESERVED_SLOTS(clasp);
    if (clasp->flags & JSCLASS_HAS_PRIVATE)
        nslots++;
    return GetGCObjectKind(nslots);
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

} /* namespace gc */
} /* namespace js */

#endif /* jsgcinlines_h */
