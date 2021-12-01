/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
* vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef gc_GC_inl_h
#define gc_GC_inl_h

#include "gc/GC.h"

#include "mozilla/DebugOnly.h"
#include "mozilla/Maybe.h"

#include "gc/Zone.h"

#include "gc/ArenaList-inl.h"

namespace js {
namespace gc {

class AutoAssertEmptyNursery;

class ArenaIter
{
    Arena* arena;
    Arena* unsweptArena;
    Arena* sweptArena;
    mozilla::DebugOnly<bool> initialized;

  public:
    ArenaIter()
      : arena(nullptr), unsweptArena(nullptr), sweptArena(nullptr), initialized(false) {}

    ArenaIter(JS::Zone* zone, AllocKind kind) : initialized(false) { init(zone, kind); }

    void init(JS::Zone* zone, AllocKind kind) {
        MOZ_ASSERT(!initialized);
        MOZ_ASSERT(zone);
        initialized = true;
        arena = zone->arenas.getFirstArena(kind);
        unsweptArena = zone->arenas.getFirstArenaToSweep(kind);
        sweptArena = zone->arenas.getFirstSweptArena(kind);
        if (!unsweptArena) {
            unsweptArena = sweptArena;
            sweptArena = nullptr;
        }
        if (!arena) {
            arena = unsweptArena;
            unsweptArena = sweptArena;
            sweptArena = nullptr;
        }
    }

    bool done() const {
        MOZ_ASSERT(initialized);
        return !arena;
    }

    Arena* get() const {
        MOZ_ASSERT(!done());
        return arena;
    }

    void next() {
        MOZ_ASSERT(!done());
        arena = arena->next;
        if (!arena) {
            arena = unsweptArena;
            unsweptArena = sweptArena;
            sweptArena = nullptr;
        }
    }
};

enum CellIterNeedsBarrier : uint8_t
{
    CellIterDoesntNeedBarrier = 0,
    CellIterMayNeedBarrier = 1
};

class ArenaCellIterImpl
{
    size_t firstThingOffset;
    size_t thingSize;
    Arena* arenaAddr;
    FreeSpan span;
    uint_fast16_t thing;
    JS::TraceKind traceKind;
    bool needsBarrier;
    mozilla::DebugOnly<bool> initialized;

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
            span = *span.nextSpan(arenaAddr);
        }
    }

  public:
    ArenaCellIterImpl()
      : firstThingOffset(0),
        thingSize(0),
        arenaAddr(nullptr),
        thing(0),
        traceKind(JS::TraceKind::Null),
        needsBarrier(false),
        initialized(false)
    {}

    explicit ArenaCellIterImpl(Arena* arena, CellIterNeedsBarrier mayNeedBarrier)
      : initialized(false)
    {
        init(arena, mayNeedBarrier);
    }

    void init(Arena* arena, CellIterNeedsBarrier mayNeedBarrier) {
        MOZ_ASSERT(!initialized);
        MOZ_ASSERT(arena);
        initialized = true;
        AllocKind kind = arena->getAllocKind();
        firstThingOffset = Arena::firstThingOffset(kind);
        thingSize = Arena::thingSize(kind);
        traceKind = MapAllocToTraceKind(kind);
        needsBarrier = mayNeedBarrier && !JS::CurrentThreadIsHeapCollecting();
        reset(arena);
    }

    // Use this to move from an Arena of a particular kind to another Arena of
    // the same kind.
    void reset(Arena* arena) {
        MOZ_ASSERT(initialized);
        MOZ_ASSERT(arena);
        arenaAddr = arena;
        span = *arena->getFirstFreeSpan();
        thing = firstThingOffset;
        moveForwardIfFree();
    }

    bool done() const {
        MOZ_ASSERT(initialized);
        MOZ_ASSERT(thing <= ArenaSize);
        return thing == ArenaSize;
    }

    TenuredCell* getCell() const {
        MOZ_ASSERT(!done());
        TenuredCell* cell = reinterpret_cast<TenuredCell*>(uintptr_t(arenaAddr) + thing);

        // This can result in a a new reference being created to an object that
        // an ongoing incremental GC may find to be unreachable, so we may need
        // a barrier here.
        if (needsBarrier)
            ExposeGCThingToActiveJS(JS::GCCellPtr(cell, traceKind));

        return cell;
    }

    template<typename T> T* get() const {
        MOZ_ASSERT(!done());
        MOZ_ASSERT(JS::MapTypeToTraceKind<T>::kind == traceKind);
        return reinterpret_cast<T*>(getCell());
    }

    void next() {
        MOZ_ASSERT(!done());
        thing += thingSize;
        if (thing < ArenaSize)
            moveForwardIfFree();
    }
};

template<>
JSObject*
ArenaCellIterImpl::get<JSObject>() const;

class ArenaCellIter : public ArenaCellIterImpl
{
  public:
    explicit ArenaCellIter(Arena* arena)
      : ArenaCellIterImpl(arena, CellIterMayNeedBarrier)
    {
        MOZ_ASSERT(JS::CurrentThreadIsHeapTracing());
    }
};

template <typename T>
class ZoneCellIter;

template <>
class ZoneCellIter<TenuredCell> {
    ArenaIter arenaIter;
    ArenaCellIterImpl cellIter;
    mozilla::Maybe<JS::AutoAssertNoGC> nogc;

  protected:
    // For use when a subclass wants to insert some setup before init().
    ZoneCellIter() {}

    void init(JS::Zone* zone, AllocKind kind) {
        MOZ_ASSERT_IF(IsNurseryAllocable(kind),
                      zone->isAtomsZone() || zone->group()->nursery().isEmpty());
        initForTenuredIteration(zone, kind);
    }

    void initForTenuredIteration(JS::Zone* zone, AllocKind kind) {
        JSRuntime* rt = zone->runtimeFromAnyThread();

        // If called from outside a GC, ensure that the heap is in a state
        // that allows us to iterate.
        if (!JS::CurrentThreadIsHeapBusy()) {
            // Assert that no GCs can occur while a ZoneCellIter is live.
            nogc.emplace();
        }

        // We have a single-threaded runtime, so there's no need to protect
        // against other threads iterating or allocating. However, we do have
        // background finalization; we may have to wait for this to finish if
        // it's currently active.
        if (IsBackgroundFinalized(kind) && zone->arenas.needBackgroundFinalizeWait(kind))
            rt->gc.waitBackgroundSweepEnd();
        arenaIter.init(zone, kind);
        if (!arenaIter.done())
            cellIter.init(arenaIter.get(), CellIterMayNeedBarrier);
    }

  public:
    ZoneCellIter(JS::Zone* zone, AllocKind kind) {
        // If we are iterating a nursery-allocated kind then we need to
        // evict first so that we can see all things.
        if (IsNurseryAllocable(kind))
            zone->runtimeFromActiveCooperatingThread()->gc.evictNursery();

        init(zone, kind);
    }

    ZoneCellIter(JS::Zone* zone, AllocKind kind, const js::gc::AutoAssertEmptyNursery&) {
        // No need to evict the nursery. (This constructor is known statically
        // to not GC.)
        init(zone, kind);
    }

    bool done() const {
        return arenaIter.done();
    }

    template<typename T>
    T* get() const {
        MOZ_ASSERT(!done());
        return cellIter.get<T>();
    }

    TenuredCell* getCell() const {
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

// Iterator over the cells in a Zone, where the GC type (JSString, JSObject) is
// known, for a single AllocKind. Example usages:
//
//   for (auto obj = zone->cellIter<JSObject>(AllocKind::OBJECT0); !obj.done(); obj.next())
//       ...
//
//   for (auto script = zone->cellIter<JSScript>(); !script.done(); script.next())
//       f(script->code());
//
// As this code demonstrates, you can use 'script' as if it were a JSScript*.
// Its actual type is ZoneCellIter<JSScript>, but for most purposes it will
// autoconvert to JSScript*.
//
// Note that in the JSScript case, ZoneCellIter is able to infer the AllocKind
// from the type 'JSScript', whereas in the JSObject case, the kind must be
// given (because there are multiple AllocKinds for objects).
//
// Also, the static rooting hazard analysis knows that the JSScript case will
// not GC during construction. The JSObject case needs to GC, or more precisely
// to empty the nursery and clear out the store buffer, so that it can see all
// objects to iterate over (the nursery is not iterable) and remove the
// possibility of having pointers from the store buffer to data hanging off
// stuff we're iterating over that we are going to delete. (The latter should
// not be a problem, since such instances should be using RelocatablePtr do
// remove themselves from the store buffer on deletion, but currently for
// subtle reasons that isn't good enough.)
//
// If the iterator is used within a GC, then there is no need to evict the
// nursery (again). You may select a variant that will skip the eviction either
// by specializing on a GCType that is never allocated in the nursery, or
// explicitly by passing in a trailing AutoAssertEmptyNursery argument.
//
template <typename GCType>
class ZoneCellIter : public ZoneCellIter<TenuredCell> {
  public:
    // Non-nursery allocated (equivalent to having an entry in
    // MapTypeToFinalizeKind). The template declaration here is to discard this
    // constructor overload if MapTypeToFinalizeKind<GCType>::kind does not
    // exist. Note that there will be no remaining overloads that will work,
    // which makes sense given that you haven't specified which of the
    // AllocKinds to use for GCType.
    //
    // If we later add a nursery allocable GCType with a single AllocKind, we
    // will want to add an overload of this constructor that does the right
    // thing (ie, it empties the nursery before iterating.)
    explicit ZoneCellIter(JS::Zone* zone) : ZoneCellIter<TenuredCell>() {
        init(zone, MapTypeToFinalizeKind<GCType>::kind);
    }

    // Non-nursery allocated, nursery is known to be empty: same behavior as above.
    ZoneCellIter(JS::Zone* zone, const js::gc::AutoAssertEmptyNursery&) : ZoneCellIter(zone) {
    }

    // Arbitrary kind, which will be assumed to be nursery allocable (and
    // therefore the nursery will be emptied before iterating.)
    ZoneCellIter(JS::Zone* zone, AllocKind kind) : ZoneCellIter<TenuredCell>(zone, kind) {
    }

    // Arbitrary kind, which will be assumed to be nursery allocable, but the
    // nursery is known to be empty already: same behavior as non-nursery types.
    ZoneCellIter(JS::Zone* zone, AllocKind kind, const js::gc::AutoAssertEmptyNursery& empty)
      : ZoneCellIter<TenuredCell>(zone, kind, empty)
    {
    }

    GCType* get() const { return ZoneCellIter<TenuredCell>::get<GCType>(); }
    operator GCType*() const { return get(); }
    GCType* operator ->() const { return get(); }
};

} /* namespace gc */
} /* namespace js */

#endif /* gc_GC_inl_h */
