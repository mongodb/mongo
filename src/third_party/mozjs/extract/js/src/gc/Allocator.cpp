/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "gc/Allocator.h"

#include "mozilla/DebugOnly.h"
#include "mozilla/OperatorNewExtensions.h"
#include "mozilla/TimeStamp.h"

#include "gc/GCInternals.h"
#include "gc/GCLock.h"
#include "gc/GCProbes.h"
#include "gc/Nursery.h"
#include "threading/CpuCount.h"
#include "util/Poison.h"
#include "vm/BigIntType.h"
#include "vm/JSContext.h"
#include "vm/Runtime.h"
#include "vm/StringType.h"

#include "gc/ArenaList-inl.h"
#include "gc/Heap-inl.h"
#include "gc/PrivateIterators-inl.h"
#include "vm/JSContext-inl.h"

using mozilla::TimeDuration;
using mozilla::TimeStamp;

using namespace js;
using namespace js::gc;

// Return a Heap value that can be compared numerically with an
// allocation's requested heap to determine whether to allocate in the nursery
// or the tenured heap.
//
// If nursery allocation is allowed this returns Heap::Tenured, meaning only
// Heap::Tenured allocations will be tenured. If nursery allocation is not
// allowed this returns Heap::Default, meaning all allocations are tenured.
static Heap MinHeapToTenure(bool allowNurseryAlloc) {
  static_assert(Heap::Tenured > Heap::Default);
  return allowNurseryAlloc ? Heap::Tenured : Heap::Default;
}

void Zone::setNurseryAllocFlags(bool allocObjects, bool allocStrings,
                                bool allocBigInts) {
  allocNurseryObjects_ = allocObjects;
  allocNurseryStrings_ = allocStrings;
  allocNurseryBigInts_ = allocBigInts;

  minObjectHeapToTenure_ = MinHeapToTenure(allocNurseryObjects());
  minStringHeapToTenure_ = MinHeapToTenure(allocNurseryStrings());
  minBigintHeapToTenure_ = MinHeapToTenure(allocNurseryBigInts());
}

template <JS::TraceKind traceKind, AllowGC allowGC /* = CanGC */>
void* gc::CellAllocator::AllocNurseryOrTenuredCell(JSContext* cx,
                                                   AllocKind allocKind,
                                                   gc::Heap heap,
                                                   AllocSite* site) {
  MOZ_ASSERT(!cx->isHelperThreadContext());
  MOZ_ASSERT_IF(heap != gc::Heap::Tenured, IsNurseryAllocable(allocKind));
  MOZ_ASSERT(MapAllocToTraceKind(allocKind) == traceKind);
  MOZ_ASSERT_IF(site && site->initialHeap() == Heap::Tenured,
                heap == Heap::Tenured);

  size_t thingSize = Arena::thingSize(allocKind);

  JSRuntime* rt = cx->runtime();
  if (!rt->gc.checkAllocatorState<allowGC>(cx, allocKind)) {
    return nullptr;
  }

  if (heap < cx->zone()->minHeapToTenure(traceKind)) {
    if (!site) {
      site = cx->zone()->unknownAllocSite(traceKind);
    }

    void* obj =
        rt->gc.tryNewNurseryCell<traceKind, allowGC>(cx, thingSize, site);
    if (obj) {
      return obj;
    }

    // Our most common non-jit allocation path is NoGC; thus, if we fail the
    // alloc and cannot GC, we *must* return nullptr here so that the caller
    // will do a CanGC allocation to clear the nursery. Failing to do so will
    // cause all allocations on this path to land in Tenured, and we will not
    // get the benefit of the nursery.
    if (!allowGC) {
      return nullptr;
    }
  }

  return GCRuntime::tryNewTenuredThing<allowGC>(cx, allocKind, thingSize);
}

#define INSTANTIATE_ALLOC_NURSERY_CELL(traceKind, allowGc)          \
  template void*                                                    \
  gc::CellAllocator::AllocNurseryOrTenuredCell<traceKind, allowGc>( \
      JSContext*, AllocKind, gc::Heap, AllocSite*);
INSTANTIATE_ALLOC_NURSERY_CELL(JS::TraceKind::Object, NoGC)
INSTANTIATE_ALLOC_NURSERY_CELL(JS::TraceKind::Object, CanGC)
INSTANTIATE_ALLOC_NURSERY_CELL(JS::TraceKind::String, NoGC)
INSTANTIATE_ALLOC_NURSERY_CELL(JS::TraceKind::String, CanGC)
INSTANTIATE_ALLOC_NURSERY_CELL(JS::TraceKind::BigInt, NoGC)
INSTANTIATE_ALLOC_NURSERY_CELL(JS::TraceKind::BigInt, CanGC)
#undef INSTANTIATE_ALLOC_NURSERY_CELL

// Attempt to allocate a new cell in the nursery. If there is not enough room in
// the nursery or there is an OOM, this method will return nullptr.
template <JS::TraceKind kind, AllowGC allowGC>
void* GCRuntime::tryNewNurseryCell(JSContext* cx, size_t thingSize,
                                   AllocSite* site) {
  MOZ_ASSERT(cx->isNurseryAllocAllowed());
  MOZ_ASSERT(cx->zone() == site->zone());
  MOZ_ASSERT(!cx->zone()->isAtomsZone());
  MOZ_ASSERT(cx->zone()->allocKindInNursery(kind));

  void* ptr = cx->nursery().allocateCell(site, thingSize, kind);
  if (ptr) {
    return ptr;
  }

  if constexpr (allowGC) {
    if (!cx->suppressGC) {
      cx->runtime()->gc.minorGC(JS::GCReason::OUT_OF_NURSERY);

      // Exceeding gcMaxBytes while tenuring can disable the Nursery.
      if (cx->zone()->allocKindInNursery(kind)) {
        return cx->nursery().allocateCell(site, thingSize, kind);
      }
    }
  }

  return nullptr;
}

template <AllowGC allowGC /* = CanGC */>
void* gc::CellAllocator::AllocTenuredCell(JSContext* cx, gc::AllocKind kind,
                                          size_t size) {
  MOZ_ASSERT(!cx->isHelperThreadContext());
  MOZ_ASSERT(!IsNurseryAllocable(kind));
  MOZ_ASSERT(size == Arena::thingSize(kind));
  MOZ_ASSERT(
      size >= gc::MinCellSize,
      "All allocations must be at least the allocator-imposed minimum size.");

  if (!cx->runtime()->gc.checkAllocatorState<allowGC>(cx, kind)) {
    return nullptr;
  }

  return GCRuntime::tryNewTenuredThing<allowGC>(cx, kind, size);
}
template void* gc::CellAllocator::AllocTenuredCell<NoGC>(JSContext*, AllocKind,
                                                         size_t);
template void* gc::CellAllocator::AllocTenuredCell<CanGC>(JSContext*, AllocKind,
                                                          size_t);

template <AllowGC allowGC>
/* static */
void* GCRuntime::tryNewTenuredThing(JSContext* cx, AllocKind kind,
                                    size_t thingSize) {
  // Bump allocate in the arena's current free-list span.
  Zone* zone = cx->zone();
  void* ptr = zone->arenas.freeLists().allocate(kind);
  if (MOZ_UNLIKELY(!ptr)) {
    // Get the next available free list and allocate out of it. This may
    // acquire a new arena, which will lock the chunk list. If there are no
    // chunks available it may also allocate new memory directly.
    ptr = refillFreeList(cx, kind);

    if (MOZ_UNLIKELY(!ptr)) {
      if constexpr (allowGC) {
        cx->runtime()->gc.attemptLastDitchGC(cx);
        ptr = tryNewTenuredThing<NoGC>(cx, kind, thingSize);
        if (ptr) {
          return ptr;
        }
        ReportOutOfMemory(cx);
      }

      return nullptr;
    }
  }

#ifdef DEBUG
  checkIncrementalZoneState(cx, ptr);
#endif

  gcprobes::TenuredAlloc(ptr, kind);

  // We count this regardless of the profiler's state, assuming that it costs
  // just as much to count it, as to check the profiler's state and decide not
  // to count it.
  zone->noteTenuredAlloc();

  return ptr;
}

void GCRuntime::attemptLastDitchGC(JSContext* cx) {
  // Either there was no memory available for a new chunk or the heap hit its
  // size limit. Try to perform an all-compartments, non-incremental, shrinking
  // GC and wait for it to finish.

  if (!lastLastDitchTime.IsNull() &&
      TimeStamp::Now() - lastLastDitchTime <= tunables.minLastDitchGCPeriod()) {
    return;
  }

  JS::PrepareForFullGC(cx);
  gc(JS::GCOptions::Shrink, JS::GCReason::LAST_DITCH);
  waitBackgroundAllocEnd();
  waitBackgroundFreeEnd();

  lastLastDitchTime = mozilla::TimeStamp::Now();
}

template <AllowGC allowGC>
bool GCRuntime::checkAllocatorState(JSContext* cx, AllocKind kind) {
  MOZ_ASSERT_IF(cx->zone()->isAtomsZone(),
                kind == AllocKind::ATOM || kind == AllocKind::FAT_INLINE_ATOM ||
                    kind == AllocKind::SYMBOL || kind == AllocKind::JITCODE ||
                    kind == AllocKind::SCOPE);
  MOZ_ASSERT_IF(!cx->zone()->isAtomsZone(),
                kind != AllocKind::ATOM && kind != AllocKind::FAT_INLINE_ATOM);
  MOZ_ASSERT(!JS::RuntimeHeapIsBusy());

  if constexpr (allowGC) {
    // Crash if we could perform a GC action when it is not safe.
    if (!cx->suppressGC) {
      cx->verifyIsSafeToGC();
    }

    gcIfNeededAtAllocation(cx);
  }

  // For testing out of memory conditions.
  if (js::oom::ShouldFailWithOOM()) {
    // If we are doing a fallible allocation, percolate up the OOM instead of
    // reporting it.
    if constexpr (allowGC) {
      ReportOutOfMemory(cx);
    }
    return false;
  }

  return true;
}

inline void GCRuntime::gcIfNeededAtAllocation(JSContext* cx) {
#ifdef JS_GC_ZEAL
  if (needZealousGC()) {
    runDebugGC();
  }
#endif

  // Invoking the interrupt callback can fail and we can't usefully
  // handle that here. Just check in case we need to collect instead.
  if (cx->hasAnyPendingInterrupt()) {
    gcIfRequested();
  }
}

#ifdef DEBUG
void GCRuntime::checkIncrementalZoneState(JSContext* cx, void* ptr) {
  MOZ_ASSERT(ptr);
  TenuredCell* cell = reinterpret_cast<TenuredCell*>(ptr);
  TenuredChunkBase* chunk = detail::GetCellChunkBase(cell);
  if (cx->zone()->isGCMarkingOrSweeping()) {
    MOZ_ASSERT(chunk->markBits.isMarkedBlack(cell));
  } else {
    MOZ_ASSERT(!chunk->markBits.isMarkedAny(cell));
  }
}
#endif

void* js::gc::AllocateCellInGC(Zone* zone, AllocKind thingKind) {
  void* ptr = zone->arenas.allocateFromFreeList(thingKind);
  if (!ptr) {
    AutoEnterOOMUnsafeRegion oomUnsafe;
    ptr = GCRuntime::refillFreeListInGC(zone, thingKind);
    if (!ptr) {
      oomUnsafe.crash(ChunkSize, "Failed to allocate new chunk during GC");
    }
  }
  return ptr;
}

// ///////////  Arena -> Thing Allocator  //////////////////////////////////////

void GCRuntime::startBackgroundAllocTaskIfIdle() {
  AutoLockHelperThreadState lock;
  if (!allocTask.wasStarted(lock)) {
    // Join the previous invocation of the task. This will return immediately
    // if the thread has never been started.
    allocTask.joinWithLockHeld(lock);
    allocTask.startWithLockHeld(lock);
  }
}

/* static */
void* GCRuntime::refillFreeList(JSContext* cx, AllocKind thingKind) {
  MOZ_ASSERT(cx->zone()->arenas.freeLists().isEmpty(thingKind));
  MOZ_ASSERT(!cx->isHelperThreadContext());

  // It should not be possible to allocate on the main thread while we are
  // inside a GC.
  MOZ_ASSERT(!JS::RuntimeHeapIsBusy(), "allocating while under GC");

  return cx->zone()->arenas.refillFreeListAndAllocate(
      thingKind, ShouldCheckThresholds::CheckThresholds);
}

/* static */
void* GCRuntime::refillFreeListInGC(Zone* zone, AllocKind thingKind) {
  // Called by compacting GC to refill a free list while we are in a GC.
  MOZ_ASSERT(JS::RuntimeHeapIsCollecting());
  MOZ_ASSERT_IF(!JS::RuntimeHeapIsMinorCollecting(),
                !zone->runtimeFromMainThread()->gc.isBackgroundSweeping());

  return zone->arenas.refillFreeListAndAllocate(
      thingKind, ShouldCheckThresholds::DontCheckThresholds);
}

void* ArenaLists::refillFreeListAndAllocate(
    AllocKind thingKind, ShouldCheckThresholds checkThresholds) {
  MOZ_ASSERT(freeLists().isEmpty(thingKind));

  JSRuntime* rt = runtimeFromAnyThread();

  mozilla::Maybe<AutoLockGCBgAlloc> maybeLock;

  // See if we can proceed without taking the GC lock.
  if (concurrentUse(thingKind) != ConcurrentUse::None) {
    maybeLock.emplace(rt);
  }

  Arena* arena = arenaList(thingKind).takeNextArena();
  if (arena) {
    // Empty arenas should be immediately freed.
    MOZ_ASSERT(!arena->isEmpty());

    return freeLists().setArenaAndAllocate(arena, thingKind);
  }

  // Parallel threads have their own ArenaLists, but chunks are shared;
  // if we haven't already, take the GC lock now to avoid racing.
  if (maybeLock.isNothing()) {
    maybeLock.emplace(rt);
  }

  TenuredChunk* chunk = rt->gc.pickChunk(maybeLock.ref());
  if (!chunk) {
    return nullptr;
  }

  // Although our chunk should definitely have enough space for another arena,
  // there are other valid reasons why TenuredChunk::allocateArena() may fail.
  arena = rt->gc.allocateArena(chunk, zone_, thingKind, checkThresholds,
                               maybeLock.ref());
  if (!arena) {
    return nullptr;
  }

  ArenaList& al = arenaList(thingKind);
  MOZ_ASSERT(al.isCursorAtEnd());
  al.insertBeforeCursor(arena);

  return freeLists().setArenaAndAllocate(arena, thingKind);
}

inline void* FreeLists::setArenaAndAllocate(Arena* arena, AllocKind kind) {
#ifdef DEBUG
  auto old = freeLists_[kind];
  if (!old->isEmpty()) {
    old->getArena()->checkNoMarkedFreeCells();
  }
#endif

  FreeSpan* span = arena->getFirstFreeSpan();
  freeLists_[kind] = span;

  Zone* zone = arena->zone;
  if (MOZ_UNLIKELY(zone->isGCMarkingOrSweeping())) {
    arena->arenaAllocatedDuringGC();
  }

  TenuredCell* thing = span->allocate(Arena::thingSize(kind));
  MOZ_ASSERT(thing);  // This allocation is infallible.

  return thing;
}

void Arena::arenaAllocatedDuringGC() {
  // Ensure that anything allocated during the mark or sweep phases of an
  // incremental GC will be marked black by pre-marking all free cells in the
  // arena we are about to allocate from.

  MOZ_ASSERT(zone->isGCMarkingOrSweeping());
  for (ArenaFreeCellIter cell(this); !cell.done(); cell.next()) {
    MOZ_ASSERT(!cell->isMarkedAny());
    cell->markBlack();
  }
}

// ///////////  TenuredChunk -> Arena Allocator  ///////////////////////////////

bool GCRuntime::wantBackgroundAllocation(const AutoLockGC& lock) const {
  // To minimize memory waste, we do not want to run the background chunk
  // allocation if we already have some empty chunks or when the runtime has
  // a small heap size (and therefore likely has a small growth rate).
  return allocTask.enabled() &&
         emptyChunks(lock).count() < minEmptyChunkCount(lock) &&
         (fullChunks(lock).count() + availableChunks(lock).count()) >= 4;
}

Arena* GCRuntime::allocateArena(TenuredChunk* chunk, Zone* zone,
                                AllocKind thingKind,
                                ShouldCheckThresholds checkThresholds,
                                const AutoLockGC& lock) {
  MOZ_ASSERT(chunk->hasAvailableArenas());

  // Fail the allocation if we are over our heap size limits.
  if ((checkThresholds != ShouldCheckThresholds::DontCheckThresholds) &&
      (heapSize.bytes() >= tunables.gcMaxBytes())) {
    return nullptr;
  }

  Arena* arena = chunk->allocateArena(this, zone, thingKind, lock);
  zone->gcHeapSize.addGCArena(heapSize);

  // Trigger an incremental slice if needed.
  if (checkThresholds != ShouldCheckThresholds::DontCheckThresholds) {
    maybeTriggerGCAfterAlloc(zone);
  }

  return arena;
}

Arena* TenuredChunk::allocateArena(GCRuntime* gc, Zone* zone,
                                   AllocKind thingKind,
                                   const AutoLockGC& lock) {
  if (info.numArenasFreeCommitted == 0) {
    commitOnePage(gc);
    MOZ_ASSERT(info.numArenasFreeCommitted == ArenasPerPage);
  }

  MOZ_ASSERT(info.numArenasFreeCommitted > 0);
  Arena* arena = fetchNextFreeArena(gc);

  arena->init(zone, thingKind, lock);
  updateChunkListAfterAlloc(gc, lock);

  verify();

  return arena;
}

template <size_t N>
static inline size_t FindFirstBitSet(
    const mozilla::BitSet<N, uint32_t>& bitset) {
  MOZ_ASSERT(!bitset.IsEmpty());

  const auto& words = bitset.Storage();
  for (size_t i = 0; i < words.Length(); i++) {
    uint32_t word = words[i];
    if (word) {
      return i * 32 + mozilla::CountTrailingZeroes32(word);
    }
  }

  MOZ_CRASH("No bits found");
}

void TenuredChunk::commitOnePage(GCRuntime* gc) {
  MOZ_ASSERT(info.numArenasFreeCommitted == 0);
  MOZ_ASSERT(info.numArenasFree >= ArenasPerPage);

  uint32_t pageIndex = FindFirstBitSet(decommittedPages);
  MOZ_ASSERT(decommittedPages[pageIndex]);

  if (DecommitEnabled()) {
    MarkPagesInUseSoft(pageAddress(pageIndex), PageSize);
  }

  decommittedPages[pageIndex] = false;

  for (size_t i = 0; i < ArenasPerPage; i++) {
    size_t arenaIndex = pageIndex * ArenasPerPage + i;
    MOZ_ASSERT(!freeCommittedArenas[arenaIndex]);
    freeCommittedArenas[arenaIndex] = true;
    arenas[arenaIndex].setAsNotAllocated();
    ++info.numArenasFreeCommitted;
    gc->updateOnArenaFree();
  }

  verify();
}

inline void GCRuntime::updateOnFreeArenaAlloc(const TenuredChunkInfo& info) {
  MOZ_ASSERT(info.numArenasFreeCommitted <= numArenasFreeCommitted);
  --numArenasFreeCommitted;
}

Arena* TenuredChunk::fetchNextFreeArena(GCRuntime* gc) {
  MOZ_ASSERT(info.numArenasFreeCommitted > 0);
  MOZ_ASSERT(info.numArenasFreeCommitted <= info.numArenasFree);

  size_t index = FindFirstBitSet(freeCommittedArenas);
  MOZ_ASSERT(freeCommittedArenas[index]);

  freeCommittedArenas[index] = false;
  --info.numArenasFreeCommitted;
  --info.numArenasFree;
  gc->updateOnFreeArenaAlloc(info);

  return &arenas[index];
}

// ///////////  System -> TenuredChunk Allocator  //////////////////////////////

TenuredChunk* GCRuntime::getOrAllocChunk(AutoLockGCBgAlloc& lock) {
  TenuredChunk* chunk = emptyChunks(lock).pop();
  if (chunk) {
    // Reinitialize ChunkBase; arenas are all free and may or may not be
    // committed.
    SetMemCheckKind(chunk, sizeof(ChunkBase), MemCheckKind::MakeUndefined);
    chunk->initBase(rt, nullptr);
    MOZ_ASSERT(chunk->unused());
  } else {
    void* ptr = TenuredChunk::allocate(this);
    if (!ptr) {
      return nullptr;
    }

    chunk = TenuredChunk::emplace(ptr, this, /* allMemoryCommitted = */ true);
    MOZ_ASSERT(chunk->info.numArenasFreeCommitted == 0);
  }

  if (wantBackgroundAllocation(lock)) {
    lock.tryToStartBackgroundAllocation();
  }

  return chunk;
}

void GCRuntime::recycleChunk(TenuredChunk* chunk, const AutoLockGC& lock) {
#ifdef DEBUG
  MOZ_ASSERT(chunk->unused());
  chunk->verify();
#endif

  // Poison ChunkBase to catch use after free.
  AlwaysPoison(chunk, JS_FREED_CHUNK_PATTERN, sizeof(ChunkBase),
               MemCheckKind::MakeNoAccess);

  emptyChunks(lock).push(chunk);
}

TenuredChunk* GCRuntime::pickChunk(AutoLockGCBgAlloc& lock) {
  if (availableChunks(lock).count()) {
    return availableChunks(lock).head();
  }

  TenuredChunk* chunk = getOrAllocChunk(lock);
  if (!chunk) {
    return nullptr;
  }

#ifdef DEBUG
  chunk->verify();
  MOZ_ASSERT(chunk->unused());
  MOZ_ASSERT(!fullChunks(lock).contains(chunk));
  MOZ_ASSERT(!availableChunks(lock).contains(chunk));
#endif

  availableChunks(lock).push(chunk);

  return chunk;
}

BackgroundAllocTask::BackgroundAllocTask(GCRuntime* gc, ChunkPool& pool)
    : GCParallelTask(gc, gcstats::PhaseKind::NONE),
      chunkPool_(pool),
      enabled_(CanUseExtraThreads() && GetCPUCount() >= 2) {
  // This can occur outside GCs so doesn't have a stats phase.
}

void BackgroundAllocTask::run(AutoLockHelperThreadState& lock) {
  AutoUnlockHelperThreadState unlock(lock);

  AutoLockGC gcLock(gc);
  while (!isCancelled() && gc->wantBackgroundAllocation(gcLock)) {
    TenuredChunk* chunk;
    {
      AutoUnlockGC unlock(gcLock);
      void* ptr = TenuredChunk::allocate(gc);
      if (!ptr) {
        break;
      }
      chunk = TenuredChunk::emplace(ptr, gc, /* allMemoryCommitted = */ true);
    }
    chunkPool_.ref().push(chunk);
  }
}

/* static */
void* TenuredChunk::allocate(GCRuntime* gc) {
  void* chunk = MapAlignedPages(ChunkSize, ChunkSize);
  if (!chunk) {
    return nullptr;
  }

  gc->stats().count(gcstats::COUNT_NEW_CHUNK);
  return chunk;
}

static inline bool ShouldDecommitNewChunk(bool allMemoryCommitted,
                                          const GCSchedulingState& state) {
  if (!DecommitEnabled()) {
    return false;
  }

  return !allMemoryCommitted || !state.inHighFrequencyGCMode();
}

TenuredChunk* TenuredChunk::emplace(void* ptr, GCRuntime* gc,
                                    bool allMemoryCommitted) {
  /* The chunk may still have some regions marked as no-access. */
  MOZ_MAKE_MEM_UNDEFINED(ptr, ChunkSize);

  /*
   * Poison the chunk. Note that decommitAllArenas() below will mark the
   * arenas as inaccessible (for memory sanitizers).
   */
  Poison(ptr, JS_FRESH_TENURED_PATTERN, ChunkSize, MemCheckKind::MakeUndefined);

  TenuredChunk* chunk = new (mozilla::KnownNotNull, ptr) TenuredChunk(gc->rt);

  if (ShouldDecommitNewChunk(allMemoryCommitted, gc->schedulingState)) {
    // Decommit the arenas. We do this after poisoning so that if the OS does
    // not have to recycle the pages, we still get the benefit of poisoning.
    chunk->decommitAllArenas();
  } else {
    // The chunk metadata is initialized as decommitted regardless, to avoid
    // having to initialize the arenas at this time.
    chunk->initAsDecommitted();
  }

  chunk->verify();

  return chunk;
}

void TenuredChunk::decommitAllArenas() {
  MOZ_ASSERT(unused());
  MarkPagesUnusedSoft(&arenas[0], ArenasPerChunk * ArenaSize);
  initAsDecommitted();
}

void TenuredChunkBase::initAsDecommitted() {
  // Set the state of all arenas to free and decommitted. They might not
  // actually be decommitted, but in that case the re-commit operation is a
  // no-op so it doesn't matter.
  decommittedPages.SetAll();
  freeCommittedArenas.ResetAll();
  info.numArenasFree = ArenasPerChunk;
  info.numArenasFreeCommitted = 0;
}
