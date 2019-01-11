/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "gc/Allocator.h"

#include "gc/GCInternals.h"
#include "gc/GCTrace.h"
#include "gc/Nursery.h"
#include "jit/JitCompartment.h"
#include "threading/CpuCount.h"
#include "vm/JSContext.h"
#include "vm/Runtime.h"
#include "vm/StringType.h"

#include "gc/ArenaList-inl.h"
#include "gc/Heap-inl.h"
#include "gc/PrivateIterators-inl.h"
#include "vm/JSObject-inl.h"

using namespace js;
using namespace gc;

template <typename T, AllowGC allowGC /* = CanGC */>
JSObject*
js::Allocate(JSContext* cx, AllocKind kind, size_t nDynamicSlots, InitialHeap heap,
             const Class* clasp)
{
    static_assert(mozilla::IsConvertible<T*, JSObject*>::value, "must be JSObject derived");
    MOZ_ASSERT(IsObjectAllocKind(kind));
    size_t thingSize = Arena::thingSize(kind);

    MOZ_ASSERT(thingSize == Arena::thingSize(kind));
    MOZ_ASSERT(thingSize >= sizeof(JSObject_Slots0));
    static_assert(sizeof(JSObject_Slots0) >= MinCellSize,
                  "All allocations must be at least the allocator-imposed minimum size.");

    MOZ_ASSERT_IF(nDynamicSlots != 0, clasp->isNative());

    // We cannot trigger GC or make runtime assertions when nursery allocation
    // is suppressed, either explicitly or because we are off-thread.
    if (cx->isNurseryAllocSuppressed()) {
        JSObject* obj = GCRuntime::tryNewTenuredObject<NoGC>(cx, kind, thingSize, nDynamicSlots);
        if (MOZ_UNLIKELY(allowGC && !obj))
            ReportOutOfMemory(cx);
        return obj;
    }

    JSRuntime* rt = cx->runtime();
    if (!rt->gc.checkAllocatorState<allowGC>(cx, kind))
        return nullptr;

    if (cx->nursery().isEnabled() && heap != TenuredHeap) {
        JSObject* obj = rt->gc.tryNewNurseryObject<allowGC>(cx, thingSize, nDynamicSlots, clasp);
        if (obj)
            return obj;

        // Our most common non-jit allocation path is NoGC; thus, if we fail the
        // alloc and cannot GC, we *must* return nullptr here so that the caller
        // will do a CanGC allocation to clear the nursery. Failing to do so will
        // cause all allocations on this path to land in Tenured, and we will not
        // get the benefit of the nursery.
        if (!allowGC)
            return nullptr;
    }

    return GCRuntime::tryNewTenuredObject<allowGC>(cx, kind, thingSize, nDynamicSlots);
}
template JSObject* js::Allocate<JSObject, NoGC>(JSContext* cx, gc::AllocKind kind,
                                                size_t nDynamicSlots, gc::InitialHeap heap,
                                                const Class* clasp);
template JSObject* js::Allocate<JSObject, CanGC>(JSContext* cx, gc::AllocKind kind,
                                                 size_t nDynamicSlots, gc::InitialHeap heap,
                                                 const Class* clasp);

// Attempt to allocate a new JSObject out of the nursery. If there is not
// enough room in the nursery or there is an OOM, this method will return
// nullptr.
template <AllowGC allowGC>
JSObject*
GCRuntime::tryNewNurseryObject(JSContext* cx, size_t thingSize, size_t nDynamicSlots, const Class* clasp)
{
    MOZ_ASSERT(cx->isNurseryAllocAllowed());
    MOZ_ASSERT(!cx->helperThread());
    MOZ_ASSERT(!cx->isNurseryAllocSuppressed());
    MOZ_ASSERT(!IsAtomsCompartment(cx->compartment()));
    JSObject* obj = cx->nursery().allocateObject(cx, thingSize, nDynamicSlots, clasp);
    if (obj)
        return obj;

    if (allowGC && !cx->suppressGC) {
        cx->runtime()->gc.minorGC(JS::gcreason::OUT_OF_NURSERY);

        // Exceeding gcMaxBytes while tenuring can disable the Nursery.
        if (cx->nursery().isEnabled()) {
            JSObject* obj = cx->nursery().allocateObject(cx, thingSize, nDynamicSlots, clasp);
            MOZ_ASSERT(obj);
            return obj;
        }
    }
    return nullptr;
}

template <AllowGC allowGC>
JSObject*
GCRuntime::tryNewTenuredObject(JSContext* cx, AllocKind kind, size_t thingSize,
                               size_t nDynamicSlots)
{
    HeapSlot* slots = nullptr;
    if (nDynamicSlots) {
        slots = cx->zone()->pod_malloc<HeapSlot>(nDynamicSlots);
        if (MOZ_UNLIKELY(!slots)) {
            if (allowGC)
                ReportOutOfMemory(cx);
            return nullptr;
        }
        Debug_SetSlotRangeToCrashOnTouch(slots, nDynamicSlots);
    }

    JSObject* obj = tryNewTenuredThing<JSObject, allowGC>(cx, kind, thingSize);

    if (obj) {
        if (nDynamicSlots)
            static_cast<NativeObject*>(obj)->initSlots(slots);
    } else {
        js_free(slots);
    }

    return obj;
}

// Attempt to allocate a new string out of the nursery. If there is not enough
// room in the nursery or there is an OOM, this method will return nullptr.
template <AllowGC allowGC>
JSString*
GCRuntime::tryNewNurseryString(JSContext* cx, size_t thingSize, AllocKind kind)
{
    MOZ_ASSERT(IsNurseryAllocable(kind));
    MOZ_ASSERT(cx->isNurseryAllocAllowed());
    MOZ_ASSERT(!cx->helperThread());
    MOZ_ASSERT(!cx->isNurseryAllocSuppressed());
    MOZ_ASSERT(!IsAtomsCompartment(cx->compartment()));

    Cell* cell = cx->nursery().allocateString(cx->zone(), thingSize, kind);
    if (cell)
        return static_cast<JSString*>(cell);

    if (allowGC && !cx->suppressGC) {
        cx->runtime()->gc.minorGC(JS::gcreason::OUT_OF_NURSERY);

        // Exceeding gcMaxBytes while tenuring can disable the Nursery.
        if (cx->nursery().isEnabled()) {
            cell = cx->nursery().allocateString(cx->zone(), thingSize, kind);
            MOZ_ASSERT(cell);
            return static_cast<JSString*>(cell);
        }
    }
    return nullptr;
}

template <typename StringAllocT, AllowGC allowGC /* = CanGC */>
StringAllocT*
js::AllocateString(JSContext* cx, InitialHeap heap)
{
    static_assert(mozilla::IsConvertible<StringAllocT*, JSString*>::value, "must be JSString derived");

    AllocKind kind = MapTypeToFinalizeKind<StringAllocT>::kind;
    size_t size = sizeof(StringAllocT);
    MOZ_ASSERT(size == Arena::thingSize(kind));
    MOZ_ASSERT(size == sizeof(JSString) || size == sizeof(JSFatInlineString));

    // Off-thread alloc cannot trigger GC or make runtime assertions.
    if (cx->isNurseryAllocSuppressed()) {
        StringAllocT* str = GCRuntime::tryNewTenuredThing<StringAllocT, NoGC>(cx, kind, size);
        if (MOZ_UNLIKELY(allowGC && !str))
            ReportOutOfMemory(cx);
        return str;
    }

    JSRuntime* rt = cx->runtime();
    if (!rt->gc.checkAllocatorState<allowGC>(cx, kind))
        return nullptr;

    if (cx->nursery().isEnabled() &&
        heap != TenuredHeap &&
        cx->nursery().canAllocateStrings() &&
        cx->zone()->allocNurseryStrings)
    {
        auto str = static_cast<StringAllocT*>(rt->gc.tryNewNurseryString<allowGC>(cx, size, kind));
        if (str)
            return str;

        // Our most common non-jit allocation path is NoGC; thus, if we fail the
        // alloc and cannot GC, we *must* return nullptr here so that the caller
        // will do a CanGC allocation to clear the nursery. Failing to do so will
        // cause all allocations on this path to land in Tenured, and we will not
        // get the benefit of the nursery.
        if (!allowGC)
            return nullptr;
    }

    return GCRuntime::tryNewTenuredThing<StringAllocT, allowGC>(cx, kind, size);
}

#define DECL_ALLOCATOR_INSTANCES(allocKind, traceKind, type, sizedType, bgfinal, nursery) \
    template type* js::AllocateString<type, NoGC>(JSContext* cx, InitialHeap heap);\
    template type* js::AllocateString<type, CanGC>(JSContext* cx, InitialHeap heap);
FOR_EACH_NURSERY_STRING_ALLOCKIND(DECL_ALLOCATOR_INSTANCES)
#undef DECL_ALLOCATOR_INSTANCES

template <typename T, AllowGC allowGC /* = CanGC */>
T*
js::Allocate(JSContext* cx)
{
    static_assert(!mozilla::IsConvertible<T*, JSObject*>::value, "must not be JSObject derived");
    static_assert(sizeof(T) >= MinCellSize,
                  "All allocations must be at least the allocator-imposed minimum size.");

    AllocKind kind = MapTypeToFinalizeKind<T>::kind;
    size_t thingSize = sizeof(T);
    MOZ_ASSERT(thingSize == Arena::thingSize(kind));

    if (!cx->helperThread()) {
        if (!cx->runtime()->gc.checkAllocatorState<allowGC>(cx, kind))
            return nullptr;
    }

    return GCRuntime::tryNewTenuredThing<T, allowGC>(cx, kind, thingSize);
}

#define DECL_ALLOCATOR_INSTANCES(allocKind, traceKind, type, sizedType, bgFinal, nursery) \
    template type* js::Allocate<type, NoGC>(JSContext* cx);\
    template type* js::Allocate<type, CanGC>(JSContext* cx);
FOR_EACH_NONOBJECT_NONNURSERY_ALLOCKIND(DECL_ALLOCATOR_INSTANCES)
#undef DECL_ALLOCATOR_INSTANCES

template <typename T, AllowGC allowGC>
/* static */ T*
GCRuntime::tryNewTenuredThing(JSContext* cx, AllocKind kind, size_t thingSize)
{
    // Bump allocate in the arena's current free-list span.
    T* t = reinterpret_cast<T*>(cx->arenas()->allocateFromFreeList(kind, thingSize));
    if (MOZ_UNLIKELY(!t)) {
        // Get the next available free list and allocate out of it. This may
        // acquire a new arena, which will lock the chunk list. If there are no
        // chunks available it may also allocate new memory directly.
        t = reinterpret_cast<T*>(refillFreeListFromAnyThread(cx, kind));

        if (MOZ_UNLIKELY(!t && allowGC && !cx->helperThread())) {
            // We have no memory available for a new chunk; perform an
            // all-compartments, non-incremental, shrinking GC and wait for
            // sweeping to finish.
            JS::PrepareForFullGC(cx);
            cx->runtime()->gc.gc(GC_SHRINK, JS::gcreason::LAST_DITCH);
            cx->runtime()->gc.waitBackgroundSweepOrAllocEnd();

            t = tryNewTenuredThing<T, NoGC>(cx, kind, thingSize);
            if (!t)
                ReportOutOfMemory(cx);
        }
    }

    checkIncrementalZoneState(cx, t);
    TraceTenuredAlloc(t, kind);
    return t;
}

template <AllowGC allowGC>
bool
GCRuntime::checkAllocatorState(JSContext* cx, AllocKind kind)
{
    if (allowGC) {
        if (!gcIfNeededAtAllocation(cx))
            return false;
    }

#if defined(JS_GC_ZEAL) || defined(DEBUG)
    MOZ_ASSERT_IF(cx->compartment()->isAtomsCompartment(),
                  kind == AllocKind::ATOM ||
                  kind == AllocKind::FAT_INLINE_ATOM ||
                  kind == AllocKind::SYMBOL ||
                  kind == AllocKind::JITCODE ||
                  kind == AllocKind::SCOPE);
    MOZ_ASSERT_IF(!cx->compartment()->isAtomsCompartment(),
                  kind != AllocKind::ATOM &&
                  kind != AllocKind::FAT_INLINE_ATOM);
    MOZ_ASSERT(!JS::CurrentThreadIsHeapBusy());
    MOZ_ASSERT(cx->isAllocAllowed());
#endif

    // Crash if we perform a GC action when it is not safe.
    if (allowGC && !cx->suppressGC)
        cx->verifyIsSafeToGC();

    // For testing out of memory conditions
    if (js::oom::ShouldFailWithOOM()) {
        // If we are doing a fallible allocation, percolate up the OOM
        // instead of reporting it.
        if (allowGC)
            ReportOutOfMemory(cx);
        return false;
    }

    return true;
}

bool
GCRuntime::gcIfNeededAtAllocation(JSContext* cx)
{
#ifdef JS_GC_ZEAL
    if (needZealousGC())
        runDebugGC();
#endif

    // Invoking the interrupt callback can fail and we can't usefully
    // handle that here. Just check in case we need to collect instead.
    if (cx->hasPendingInterrupt())
        gcIfRequested();

    // If we have grown past our GC heap threshold while in the middle of
    // an incremental GC, we're growing faster than we're GCing, so stop
    // the world and do a full, non-incremental GC right now, if possible.
    if (isIncrementalGCInProgress() &&
        cx->zone()->usage.gcBytes() > cx->zone()->threshold.gcTriggerBytes())
    {
        PrepareZoneForGC(cx->zone());
        gc(GC_NORMAL, JS::gcreason::INCREMENTAL_TOO_SLOW);
    }

    return true;
}

template <typename T>
/* static */ void
GCRuntime::checkIncrementalZoneState(JSContext* cx, T* t)
{
#ifdef DEBUG
    if (cx->helperThread() || !t)
        return;

    TenuredCell* cell = &t->asTenured();
    Zone* zone = cell->zone();
    if (zone->isGCMarking() || zone->isGCSweeping())
        MOZ_ASSERT(cell->isMarkedBlack());
    else
        MOZ_ASSERT(!cell->isMarkedAny());
#endif
}


// ///////////  Arena -> Thing Allocator  //////////////////////////////////////

void
GCRuntime::startBackgroundAllocTaskIfIdle()
{
    AutoLockHelperThreadState helperLock;
    if (allocTask.isRunningWithLockHeld(helperLock))
        return;

    // Join the previous invocation of the task. This will return immediately
    // if the thread has never been started.
    allocTask.joinWithLockHeld(helperLock);
    allocTask.startWithLockHeld(helperLock);
}

/* static */ TenuredCell*
GCRuntime::refillFreeListFromAnyThread(JSContext* cx, AllocKind thingKind)
{
    cx->arenas()->checkEmptyFreeList(thingKind);

    if (!cx->helperThread())
        return refillFreeListFromActiveCooperatingThread(cx, thingKind);

    return refillFreeListFromHelperThread(cx, thingKind);
}

/* static */ TenuredCell*
GCRuntime::refillFreeListFromActiveCooperatingThread(JSContext* cx, AllocKind thingKind)
{
    // It should not be possible to allocate on the active thread while we are
    // inside a GC.
    Zone *zone = cx->zone();
    MOZ_ASSERT(!JS::CurrentThreadIsHeapBusy(), "allocating while under GC");

    return cx->arenas()->allocateFromArena(zone, thingKind, ShouldCheckThresholds::CheckThresholds);
}

/* static */ TenuredCell*
GCRuntime::refillFreeListFromHelperThread(JSContext* cx, AllocKind thingKind)
{
    // A GC may be happening on the active thread, but zones used by off thread
    // tasks are never collected.
    Zone* zone = cx->zone();
    MOZ_ASSERT(!zone->wasGCStarted());

    return cx->arenas()->allocateFromArena(zone, thingKind, ShouldCheckThresholds::CheckThresholds);
}

/* static */ TenuredCell*
GCRuntime::refillFreeListInGC(Zone* zone, AllocKind thingKind)
{
    /*
     * Called by compacting GC to refill a free list while we are in a GC.
     */

    zone->arenas.checkEmptyFreeList(thingKind);
    mozilla::DebugOnly<JSRuntime*> rt = zone->runtimeFromActiveCooperatingThread();
    MOZ_ASSERT(JS::CurrentThreadIsHeapCollecting());
    MOZ_ASSERT_IF(!JS::CurrentThreadIsHeapMinorCollecting(), !rt->gc.isBackgroundSweeping());

    return zone->arenas.allocateFromArena(zone, thingKind, ShouldCheckThresholds::DontCheckThresholds);
}

TenuredCell*
ArenaLists::allocateFromArena(JS::Zone* zone, AllocKind thingKind,
                              ShouldCheckThresholds checkThresholds)
{
    JSRuntime* rt = zone->runtimeFromAnyThread();

    mozilla::Maybe<AutoLockGCBgAlloc> maybeLock;

    // See if we can proceed without taking the GC lock.
    if (backgroundFinalizeState(thingKind) != BFS_DONE)
        maybeLock.emplace(rt);

    ArenaList& al = arenaLists(thingKind);
    Arena* arena = al.takeNextArena();
    if (arena) {
        // Empty arenas should be immediately freed.
        MOZ_ASSERT(!arena->isEmpty());

        return allocateFromArenaInner(zone, arena, thingKind);
    }

    // Parallel threads have their own ArenaLists, but chunks are shared;
    // if we haven't already, take the GC lock now to avoid racing.
    if (maybeLock.isNothing())
        maybeLock.emplace(rt);

    Chunk* chunk = rt->gc.pickChunk(maybeLock.ref());
    if (!chunk)
        return nullptr;

    // Although our chunk should definitely have enough space for another arena,
    // there are other valid reasons why Chunk::allocateArena() may fail.
    arena = rt->gc.allocateArena(chunk, zone, thingKind, checkThresholds, maybeLock.ref());
    if (!arena)
        return nullptr;

    MOZ_ASSERT(al.isCursorAtEnd());
    al.insertBeforeCursor(arena);

    return allocateFromArenaInner(zone, arena, thingKind);
}

inline TenuredCell*
ArenaLists::allocateFromArenaInner(JS::Zone* zone, Arena* arena, AllocKind kind)
{
    size_t thingSize = Arena::thingSize(kind);

    setFreeList(kind, arena->getFirstFreeSpan());

    if (MOZ_UNLIKELY(zone->wasGCStarted()))
        zone->runtimeFromAnyThread()->gc.arenaAllocatedDuringGC(zone, arena);
    TenuredCell* thing = freeList(kind)->allocate(thingSize);

    MOZ_ASSERT(thing); // This allocation is infallible.
    return thing;
}

void
GCRuntime::arenaAllocatedDuringGC(JS::Zone* zone, Arena* arena)
{
    // Ensure that anything allocated during the mark or sweep phases of an
    // incremental GC will be marked black by pre-marking all free cells in the
    // arena we are about to allocate from.

    if (zone->needsIncrementalBarrier() || zone->isGCSweeping()) {
        for (ArenaFreeCellIter iter(arena); !iter.done(); iter.next()) {
            TenuredCell* cell = iter.getCell();
            MOZ_ASSERT(!cell->isMarkedAny());
            cell->markBlack();
        }
    }
}


// ///////////  Chunk -> Arena Allocator  //////////////////////////////////////

bool
GCRuntime::wantBackgroundAllocation(const AutoLockGC& lock) const
{
    // To minimize memory waste, we do not want to run the background chunk
    // allocation if we already have some empty chunks or when the runtime has
    // a small heap size (and therefore likely has a small growth rate).
    return allocTask.enabled() &&
           emptyChunks(lock).count() < tunables.minEmptyChunkCount(lock) &&
           (fullChunks(lock).count() + availableChunks(lock).count()) >= 4;
}

Arena*
GCRuntime::allocateArena(Chunk* chunk, Zone* zone, AllocKind thingKind,
                         ShouldCheckThresholds checkThresholds, const AutoLockGC& lock)
{
    MOZ_ASSERT(chunk->hasAvailableArenas());

    // Fail the allocation if we are over our heap size limits.
    if ((checkThresholds != ShouldCheckThresholds::DontCheckThresholds) &&
        (usage.gcBytes() >= tunables.gcMaxBytes()))
        return nullptr;

    Arena* arena = chunk->allocateArena(rt, zone, thingKind, lock);
    zone->usage.addGCArena();

    // Trigger an incremental slice if needed.
    if (checkThresholds != ShouldCheckThresholds::DontCheckThresholds)
        maybeAllocTriggerZoneGC(zone, lock);

    return arena;
}

Arena*
Chunk::allocateArena(JSRuntime* rt, Zone* zone, AllocKind thingKind, const AutoLockGC& lock)
{
    Arena* arena = info.numArenasFreeCommitted > 0
                   ? fetchNextFreeArena(rt)
                   : fetchNextDecommittedArena();
    arena->init(zone, thingKind);
    updateChunkListAfterAlloc(rt, lock);
    return arena;
}

inline void
GCRuntime::updateOnFreeArenaAlloc(const ChunkInfo& info)
{
    MOZ_ASSERT(info.numArenasFreeCommitted <= numArenasFreeCommitted);
    --numArenasFreeCommitted;
}

Arena*
Chunk::fetchNextFreeArena(JSRuntime* rt)
{
    MOZ_ASSERT(info.numArenasFreeCommitted > 0);
    MOZ_ASSERT(info.numArenasFreeCommitted <= info.numArenasFree);

    Arena* arena = info.freeArenasHead;
    info.freeArenasHead = arena->next;
    --info.numArenasFreeCommitted;
    --info.numArenasFree;
    rt->gc.updateOnFreeArenaAlloc(info);

    return arena;
}

Arena*
Chunk::fetchNextDecommittedArena()
{
    MOZ_ASSERT(info.numArenasFreeCommitted == 0);
    MOZ_ASSERT(info.numArenasFree > 0);

    unsigned offset = findDecommittedArenaOffset();
    info.lastDecommittedArenaOffset = offset + 1;
    --info.numArenasFree;
    decommittedArenas.unset(offset);

    Arena* arena = &arenas[offset];
    MarkPagesInUse(arena, ArenaSize);
    arena->setAsNotAllocated();

    return arena;
}

/*
 * Search for and return the next decommitted Arena. Our goal is to keep
 * lastDecommittedArenaOffset "close" to a free arena. We do this by setting
 * it to the most recently freed arena when we free, and forcing it to
 * the last alloc + 1 when we allocate.
 */
uint32_t
Chunk::findDecommittedArenaOffset()
{
    /* Note: lastFreeArenaOffset can be past the end of the list. */
    for (unsigned i = info.lastDecommittedArenaOffset; i < ArenasPerChunk; i++) {
        if (decommittedArenas.get(i))
            return i;
    }
    for (unsigned i = 0; i < info.lastDecommittedArenaOffset; i++) {
        if (decommittedArenas.get(i))
            return i;
    }
    MOZ_CRASH("No decommitted arenas found.");
}


// ///////////  System -> Chunk Allocator  /////////////////////////////////////

Chunk*
GCRuntime::getOrAllocChunk(AutoLockGCBgAlloc& lock)
{
    Chunk* chunk = emptyChunks(lock).pop();
    if (!chunk) {
        chunk = Chunk::allocate(rt);
        if (!chunk)
            return nullptr;
        MOZ_ASSERT(chunk->info.numArenasFreeCommitted == 0);
    }

    if (wantBackgroundAllocation(lock))
        lock.tryToStartBackgroundAllocation();

    return chunk;
}

void
GCRuntime::recycleChunk(Chunk* chunk, const AutoLockGC& lock)
{
    emptyChunks(lock).push(chunk);
}

Chunk*
GCRuntime::pickChunk(AutoLockGCBgAlloc& lock)
{
    if (availableChunks(lock).count())
        return availableChunks(lock).head();

    Chunk* chunk = getOrAllocChunk(lock);
    if (!chunk)
        return nullptr;

    chunk->init(rt);
    MOZ_ASSERT(chunk->info.numArenasFreeCommitted == 0);
    MOZ_ASSERT(chunk->unused());
    MOZ_ASSERT(!fullChunks(lock).contains(chunk));
    MOZ_ASSERT(!availableChunks(lock).contains(chunk));

    chunkAllocationSinceLastGC = true;

    availableChunks(lock).push(chunk);

    return chunk;
}

BackgroundAllocTask::BackgroundAllocTask(JSRuntime* rt, ChunkPool& pool)
  : GCParallelTaskHelper(rt),
    chunkPool_(pool),
    enabled_(CanUseExtraThreads() && GetCPUCount() >= 2)
{
}

/* virtual */ void
BackgroundAllocTask::run()
{
    TraceLoggerThread* logger = TraceLoggerForCurrentThread();
    AutoTraceLog logAllocation(logger, TraceLogger_GCAllocation);

    AutoLockGC lock(runtime());
    while (!cancel_ && runtime()->gc.wantBackgroundAllocation(lock)) {
        Chunk* chunk;
        {
            AutoUnlockGC unlock(lock);
            chunk = Chunk::allocate(runtime());
            if (!chunk)
                break;
            chunk->init(runtime());
        }
        chunkPool_.ref().push(chunk);
    }
}

/* static */ Chunk*
Chunk::allocate(JSRuntime* rt)
{
    Chunk* chunk = static_cast<Chunk*>(MapAlignedPages(ChunkSize, ChunkSize));
    if (!chunk)
        return nullptr;
    rt->gc.stats().count(gcstats::STAT_NEW_CHUNK);
    return chunk;
}

void
Chunk::init(JSRuntime* rt)
{
    JS_POISON(this, JS_FRESH_TENURED_PATTERN, ChunkSize);

    /*
     * We clear the bitmap to guard against JS::GCThingIsMarkedGray being called
     * on uninitialized data, which would happen before the first GC cycle.
     */
    bitmap.clear();

    /*
     * Decommit the arenas. We do this after poisoning so that if the OS does
     * not have to recycle the pages, we still get the benefit of poisoning.
     */
    decommitAllArenas();

    /* Initialize the chunk info. */
    info.init();
    new (&trailer) ChunkTrailer(rt);

    /* The rest of info fields are initialized in pickChunk. */
}

void Chunk::decommitAllArenas()
{
    decommittedArenas.clear(true);
    MarkPagesUnused(&arenas[0], ArenasPerChunk * ArenaSize);

    info.freeArenasHead = nullptr;
    info.lastDecommittedArenaOffset = 0;
    info.numArenasFree = ArenasPerChunk;
    info.numArenasFreeCommitted = 0;
}
