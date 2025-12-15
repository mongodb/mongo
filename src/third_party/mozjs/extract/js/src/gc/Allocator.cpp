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
#include "vm/FrameIter.h"
#include "vm/Runtime.h"
#include "vm/StringType.h"

#include "gc/ArenaList-inl.h"
#include "gc/Heap-inl.h"
#include "gc/PrivateIterators-inl.h"
#include "vm/JSContext-inl.h"
#include "vm/JSScript-inl.h"

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

#define INSTANTIATE_ALLOC_NURSERY_CELL(traceKind, allowGc)          \
  template void*                                                    \
  gc::CellAllocator::AllocNurseryOrTenuredCell<traceKind, allowGc>( \
      JSContext*, AllocKind, size_t, gc::Heap, AllocSite*);
INSTANTIATE_ALLOC_NURSERY_CELL(JS::TraceKind::Object, NoGC)
INSTANTIATE_ALLOC_NURSERY_CELL(JS::TraceKind::Object, CanGC)
INSTANTIATE_ALLOC_NURSERY_CELL(JS::TraceKind::String, NoGC)
INSTANTIATE_ALLOC_NURSERY_CELL(JS::TraceKind::String, CanGC)
INSTANTIATE_ALLOC_NURSERY_CELL(JS::TraceKind::BigInt, NoGC)
INSTANTIATE_ALLOC_NURSERY_CELL(JS::TraceKind::BigInt, CanGC)
#undef INSTANTIATE_ALLOC_NURSERY_CELL

// Attempt to allocate a new cell in the nursery. If there is not enough room in
// the nursery or there is an OOM, this method will return nullptr.
template <AllowGC allowGC>
/* static */
MOZ_NEVER_INLINE void* CellAllocator::RetryNurseryAlloc(JSContext* cx,
                                                        JS::TraceKind traceKind,
                                                        AllocKind allocKind,
                                                        size_t thingSize,
                                                        AllocSite* site) {
  MOZ_ASSERT(cx->isNurseryAllocAllowed());

  Zone* zone = site->zone();
  MOZ_ASSERT(!zone->isAtomsZone());
  MOZ_ASSERT(zone->allocKindInNursery(traceKind));

  Nursery& nursery = cx->nursery();
  JS::GCReason reason = nursery.handleAllocationFailure();
  if (reason == JS::GCReason::NO_REASON) {
    void* ptr = nursery.tryAllocateCell(site, thingSize, traceKind);
    MOZ_ASSERT(ptr);
    return ptr;
  }

  // Our most common non-jit allocation path is NoGC; thus, if we fail the
  // alloc and cannot GC, we *must* return nullptr here so that the caller
  // will do a CanGC allocation to clear the nursery. Failing to do so will
  // cause all allocations on this path to land in Tenured, and we will not
  // get the benefit of the nursery.
  if constexpr (!allowGC) {
    return nullptr;
  }

  if (!cx->suppressGC) {
    cx->runtime()->gc.minorGC(reason);

    // Exceeding gcMaxBytes while tenuring can disable the Nursery.
    if (zone->allocKindInNursery(traceKind)) {
      void* ptr = cx->nursery().allocateCell(site, thingSize, traceKind);
      if (ptr) {
        return ptr;
      }
    }
  }

  // As a final fallback, allocate the cell in the tenured heap.
  return AllocTenuredCellForNurseryAlloc<allowGC>(cx, allocKind);
}

template void* CellAllocator::RetryNurseryAlloc<NoGC>(JSContext* cx,
                                                      JS::TraceKind traceKind,
                                                      AllocKind allocKind,
                                                      size_t thingSize,
                                                      AllocSite* site);
template void* CellAllocator::RetryNurseryAlloc<CanGC>(JSContext* cx,
                                                       JS::TraceKind traceKind,
                                                       AllocKind allocKind,
                                                       size_t thingSize,
                                                       AllocSite* site);

static inline void MajorGCIfRequested(JSContext* cx) {
  // Invoking the interrupt callback can fail and we can't usefully
  // handle that here. Just check in case we need to collect instead.
  if (cx->hasPendingInterrupt(InterruptReason::MajorGC)) {
    cx->runtime()->gc.gcIfRequested();
  }
}

template <AllowGC allowGC>
MOZ_NEVER_INLINE void* gc::CellAllocator::AllocTenuredCellForNurseryAlloc(
    JSContext* cx, gc::AllocKind kind) {
  if constexpr (allowGC) {
    MajorGCIfRequested(cx);
  }

  return AllocTenuredCellUnchecked<allowGC>(cx->zone(), kind);
}
template void* gc::CellAllocator::AllocTenuredCellForNurseryAlloc<NoGC>(
    JSContext*, AllocKind);
template void* gc::CellAllocator::AllocTenuredCellForNurseryAlloc<CanGC>(
    JSContext*, AllocKind);

#ifdef DEBUG
static bool IsAtomsZoneKind(AllocKind kind) {
  return kind == AllocKind::ATOM || kind == AllocKind::FAT_INLINE_ATOM ||
         kind == AllocKind::SYMBOL;
}
#endif

template <AllowGC allowGC>
void* gc::CellAllocator::AllocTenuredCell(JSContext* cx, gc::AllocKind kind) {
  MOZ_ASSERT(!IsNurseryAllocable(kind));
  MOZ_ASSERT_IF(cx->zone()->isAtomsZone(),
                IsAtomsZoneKind(kind) || kind == AllocKind::JITCODE);
  MOZ_ASSERT_IF(!cx->zone()->isAtomsZone(), !IsAtomsZoneKind(kind));
  MOZ_ASSERT(CurrentThreadCanAccessRuntime(cx->runtime()));

  if constexpr (allowGC) {
    PreAllocGCChecks(cx);
  }

  if (!CheckForSimulatedFailure(cx, allowGC)) {
    return nullptr;
  }

  if constexpr (allowGC) {
    MajorGCIfRequested(cx);
  }

  return AllocTenuredCellUnchecked<allowGC>(cx->zone(), kind);
}
template void* gc::CellAllocator::AllocTenuredCell<NoGC>(JSContext*, AllocKind);
template void* gc::CellAllocator::AllocTenuredCell<CanGC>(JSContext*,
                                                          AllocKind);

template <AllowGC allowGC>
/* static */
void* CellAllocator::AllocTenuredCellUnchecked(JS::Zone* zone, AllocKind kind) {
  // Bump allocate in the arena's current free-list span.
  void* ptr = zone->arenas.freeLists().allocate(kind);
  if (MOZ_UNLIKELY(!ptr)) {
    // Get the next available free list and allocate out of it. This may acquire
    // a new arena, which will lock the chunk list. If there are no chunks
    // available it may also allocate new memory directly.
    ptr = GCRuntime::refillFreeList(zone, kind);

    if (MOZ_UNLIKELY(!ptr)) {
      if constexpr (allowGC) {
        return RetryTenuredAlloc(zone, kind);
      }

      return nullptr;
    }
  }

#ifdef DEBUG
  CheckIncrementalZoneState(zone, ptr);
#endif

  gcprobes::TenuredAlloc(ptr, kind);

  // We count this regardless of the profiler's state, assuming that it costs
  // just as much to count it, as to check the profiler's state and decide not
  // to count it.
  zone->noteTenuredAlloc();

  return ptr;
}
template void* CellAllocator::AllocTenuredCellUnchecked<NoGC>(JS::Zone* zone,
                                                              AllocKind kind);
template void* CellAllocator::AllocTenuredCellUnchecked<CanGC>(JS::Zone* zone,
                                                               AllocKind kind);
/* static */
MOZ_NEVER_INLINE void* CellAllocator::RetryTenuredAlloc(JS::Zone* zone,
                                                        AllocKind kind) {
  JSRuntime* runtime = zone->runtimeFromMainThread();
  runtime->gc.attemptLastDitchGC();

  void* ptr = AllocTenuredCellUnchecked<NoGC>(zone, kind);
  if (!ptr) {
    ReportOutOfMemory(runtime->mainContextFromOwnThread());
    return nullptr;
  }

  return ptr;
}

void GCRuntime::attemptLastDitchGC() {
  // Either there was no memory available for a new chunk or the heap hit its
  // size limit. Try to perform an all-compartments, non-incremental, shrinking
  // GC and wait for it to finish.

  if (!lastLastDitchTime.IsNull() &&
      TimeStamp::Now() - lastLastDitchTime <= tunables.minLastDitchGCPeriod()) {
    return;
  }

  JS::PrepareForFullGC(rt->mainContextFromOwnThread());
  gc(JS::GCOptions::Shrink, JS::GCReason::LAST_DITCH);
  waitBackgroundAllocEnd();
  waitBackgroundFreeEnd();

  lastLastDitchTime = mozilla::TimeStamp::Now();
}

#ifdef JS_GC_ZEAL

/* static */
AllocSite* CellAllocator::MaybeGenerateMissingAllocSite(JSContext* cx,
                                                        JS::TraceKind traceKind,
                                                        AllocSite* site) {
  MOZ_ASSERT(site);

  if (!cx->runtime()->gc.tunables.generateMissingAllocSites()) {
    return site;
  }

  if (!site->isUnknown()) {
    return site;
  }

  if (cx->inUnsafeCallWithABI) {
    return site;
  }

  FrameIter frame(cx);
  if (frame.done() || !frame.isBaseline()) {
    return site;
  }

  MOZ_ASSERT(site == cx->zone()->unknownAllocSite(traceKind));
  MOZ_ASSERT(frame.hasScript());

  JSScript* script = frame.script();
  if (cx->zone() != script->zone()) {
    return site;  // Skip cross-zone allocation.
  }

  uint32_t pcOffset = script->pcToOffset(frame.pc());
  if (!script->hasBaselineScript() || pcOffset > AllocSite::MaxValidPCOffset) {
    return site;
  }

  AllocSite* missingSite =
      GetOrCreateMissingAllocSite(cx, script, pcOffset, traceKind);
  if (!missingSite) {
    return site;
  }

  return missingSite;
}

#endif  // JS_GC_ZEAL

#ifdef DEBUG
/* static */
void CellAllocator::CheckIncrementalZoneState(JS::Zone* zone, void* ptr) {
  MOZ_ASSERT(ptr);
  TenuredCell* cell = reinterpret_cast<TenuredCell*>(ptr);
  ArenaChunkBase* chunk = detail::GetCellChunkBase(cell);
  if (zone->isGCMarkingOrSweeping()) {
    MOZ_ASSERT(chunk->markBits.isMarkedBlack(cell));
  } else {
    MOZ_ASSERT(!chunk->markBits.isMarkedAny(cell));
  }
}
#endif

void* js::gc::AllocateTenuredCellInGC(Zone* zone, AllocKind thingKind) {
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
void* GCRuntime::refillFreeList(JS::Zone* zone, AllocKind thingKind) {
  MOZ_ASSERT(zone->arenas.freeLists().isEmpty(thingKind));

  // It should not be possible to allocate on the main thread while we are
  // inside a GC.
  MOZ_ASSERT(!JS::RuntimeHeapIsCollecting(), "allocating while under GC");

  return zone->arenas.refillFreeListAndAllocate(
      thingKind, ShouldCheckThresholds::CheckThresholds, StallAndRetry::No);
}

/* static */
void* GCRuntime::refillFreeListInGC(Zone* zone, AllocKind thingKind) {
  // Called when tenuring nursery cells and during compacting GC.
  MOZ_ASSERT_IF(!JS::RuntimeHeapIsMinorCollecting(),
                !zone->runtimeFromMainThread()->gc.isBackgroundSweeping());

  // Since this needs to succeed we pass StallAndRetry::Yes.
  return zone->arenas.refillFreeListAndAllocate(
      thingKind, ShouldCheckThresholds::DontCheckThresholds,
      StallAndRetry::Yes);
}

void* ArenaLists::refillFreeListAndAllocate(
    AllocKind thingKind, ShouldCheckThresholds checkThresholds,
    StallAndRetry stallAndRetry) {
  MOZ_ASSERT(freeLists().isEmpty(thingKind));

  GCRuntime* gc = &runtimeFromAnyThread()->gc;

retry_loop:
  Arena* arena = arenaList(thingKind).takeInitialNonFullArena();
  if (arena) {
    // Empty arenas should be immediately freed.
    MOZ_ASSERT(!arena->isEmpty());
    return freeLists().setArenaAndAllocate(arena, thingKind);
  }

  // If we have just finished background sweep then merge the swept arenas in
  // and retry.
  if (MOZ_UNLIKELY(concurrentUse(thingKind) ==
                   ConcurrentUse::BackgroundFinalizeFinished)) {
    ArenaList sweptArenas;
    {
      AutoLockGC lock(gc);
      sweptArenas = std::move(collectingArenaList(thingKind));
    }
    concurrentUse(thingKind) = ConcurrentUse::None;
    if (!sweptArenas.isEmpty()) {
      mergeSweptArenas(thingKind, sweptArenas);
      goto retry_loop;
    }
  }

  // Use the current chunk if set.
  ArenaChunk* chunk = gc->currentChunk_;
  MOZ_ASSERT_IF(chunk, gc->isCurrentChunk(chunk));

  if (!chunk) {
    // The chunk lists can be accessed by background sweeping and background
    // chunk allocation. Take the GC lock to synchronize access.
    AutoLockGCBgAlloc lock(gc);

    chunk = gc->pickChunk(stallAndRetry, lock);
    if (!chunk) {
      return nullptr;
    }

    gc->setCurrentChunk(chunk, lock);
  }

  MOZ_ASSERT(gc->isCurrentChunk(chunk));

  // Although our chunk should definitely have enough space for another arena,
  // there are other valid reasons why ArenaChunk::allocateArena() may fail.
  arena = gc->allocateArena(chunk, zone_, thingKind, checkThresholds);
  if (!arena) {
    return nullptr;
  }

  arena->init(gc, zone_, thingKind);

  ArenaList& al = arenaList(thingKind);
  MOZ_ASSERT(!al.hasNonFullArenas());
  al.pushBack(arena);

  return freeLists().setArenaAndAllocate(arena, thingKind);
}

inline void* FreeLists::setArenaAndAllocate(Arena* arena, AllocKind kind) {
#ifdef DEBUG
  auto* old = freeLists_[kind];
  if (!old->isEmpty()) {
    old->getArena()->checkNoMarkedFreeCells();
  }
#endif

  FreeSpan* span = arena->getFirstFreeSpan();
  freeLists_[kind] = span;

  Zone* zone = arena->zone();
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

  MOZ_ASSERT(zone()->isGCMarkingOrSweeping());
  for (ArenaFreeCellIter cell(this); !cell.done(); cell.next()) {
    MOZ_ASSERT(!cell->isMarkedAny());
    cell->markBlack();
  }
}

// ///////////  ArenaChunk -> Arena Allocator  /////////////////////////////////

bool GCRuntime::wantBackgroundAllocation(const AutoLockGC& lock) const {
  // To minimize memory waste, we do not want to run the background chunk
  // allocation if we already have some empty chunks or when the runtime has
  // a small heap size (and therefore likely has a small growth rate).
  return allocTask.enabled() &&
         emptyChunks(lock).count() < minEmptyChunkCount(lock) &&
         (fullChunks(lock).count() + availableChunks(lock).count()) >= 4;
}

// Allocate a new arena but don't initialize it.
Arena* GCRuntime::allocateArena(ArenaChunk* chunk, Zone* zone,
                                AllocKind thingKind,
                                ShouldCheckThresholds checkThresholds) {
  MOZ_ASSERT(chunk->hasAvailableArenas());

  // Fail the allocation if we are over our heap size limits.
  if ((checkThresholds != ShouldCheckThresholds::DontCheckThresholds) &&
      (heapSize.bytes() >= tunables.gcMaxBytes())) {
    return nullptr;
  }

  Arena* arena = chunk->allocateArena(this, zone, thingKind);

  if (IsBufferAllocKind(thingKind)) {
    // Try to keep GC scheduling the same to minimize benchmark noise.
    // Keep this in sync with Arena::release.
    size_t usableSize = ArenaSize - Arena::firstThingOffset(thingKind);
    zone->mallocHeapSize.addBytes(usableSize);
  } else {
    zone->gcHeapSize.addGCArena(heapSize);
  }

  // Trigger an incremental slice if needed.
  if (checkThresholds != ShouldCheckThresholds::DontCheckThresholds) {
    maybeTriggerGCAfterAlloc(zone);
  }

  return arena;
}

Arena* ArenaChunk::allocateArena(GCRuntime* gc, Zone* zone,
                                 AllocKind thingKind) {
  MOZ_ASSERT(info.isCurrentChunk);
  MOZ_ASSERT(hasAvailableArenas());

  if (info.numArenasFreeCommitted == 0) {
    commitOnePage(gc);
    MOZ_ASSERT(info.numArenasFreeCommitted == ArenasPerPage);
  }

  MOZ_ASSERT(info.numArenasFreeCommitted > 0);
  Arena* arena = fetchNextFreeArena(gc);

  updateCurrentChunkAfterAlloc(gc);

  verify();

  return arena;
}

void ArenaChunk::commitOnePage(GCRuntime* gc) {
  MOZ_ASSERT(info.numArenasFreeCommitted == 0);
  MOZ_ASSERT(info.numArenasFree >= ArenasPerPage);

  uint32_t pageIndex = decommittedPages.FindFirst();
  MOZ_ASSERT(pageIndex < PagesPerChunk);
  MOZ_ASSERT(decommittedPages[pageIndex]);

  if (DecommitEnabled()) {
    MarkPagesInUseSoft(pageAddress(pageIndex), PageSize);
  }

  decommittedPages[pageIndex] = false;

  for (size_t i = 0; i < ArenasPerPage; i++) {
    size_t arenaIndex = pageToArenaIndex(pageIndex) + i;
    MOZ_ASSERT(!freeCommittedArenas[arenaIndex]);
    freeCommittedArenas[arenaIndex] = true;
    ++info.numArenasFreeCommitted;
  }

  verify();
}

Arena* ArenaChunk::fetchNextFreeArena(GCRuntime* gc) {
  MOZ_ASSERT(info.numArenasFreeCommitted > 0);
  MOZ_ASSERT(info.numArenasFreeCommitted <= info.numArenasFree);

  size_t index = freeCommittedArenas.FindFirst();
  MOZ_ASSERT(index < ArenasPerChunk);

  MOZ_ASSERT(freeCommittedArenas[index]);
  freeCommittedArenas[index] = false;

  return &arenas[index];
}

// ///////////  System -> ArenaChunk Allocator  ////////////////////////////////

ArenaChunk* GCRuntime::takeOrAllocChunk(StallAndRetry stallAndRetry,
                                        AutoLockGCBgAlloc& lock) {
  ArenaChunk* chunk = getOrAllocChunk(stallAndRetry, lock);
  if (!chunk) {
    return nullptr;
  }

  emptyChunks(lock).remove(chunk);
  return chunk;
}

ArenaChunk* GCRuntime::getOrAllocChunk(StallAndRetry stallAndRetry,
                                       AutoLockGCBgAlloc& lock) {
  ArenaChunk* chunk;
  if (!emptyChunks(lock).empty()) {
    chunk = emptyChunks(lock).head();
    // Reinitialize ChunkBase; arenas are all free and may or may not be
    // committed.
    SetMemCheckKind(chunk, sizeof(ChunkBase), MemCheckKind::MakeUndefined);
    chunk->initBaseForArenaChunk(rt);
    MOZ_ASSERT(chunk->isEmpty());
  } else {
    void* ptr = ArenaChunk::allocate(this, stallAndRetry);
    if (!ptr) {
      return nullptr;
    }

    chunk = ArenaChunk::emplace(ptr, this, /* allMemoryCommitted = */ true);
    MOZ_ASSERT(chunk->isEmpty());
    emptyChunks(lock).push(chunk);
  }

  if (wantBackgroundAllocation(lock)) {
    lock.tryToStartBackgroundAllocation();
  }

  return chunk;
}

void GCRuntime::recycleChunk(ArenaChunk* chunk, const AutoLockGC& lock) {
#ifdef DEBUG
  MOZ_ASSERT(chunk->isEmpty());
  MOZ_ASSERT(!chunk->info.isCurrentChunk);
  chunk->verify();
#endif

  // Poison ChunkBase to catch use after free.
  AlwaysPoison(chunk, JS_FREED_CHUNK_PATTERN, sizeof(ChunkBase),
               MemCheckKind::MakeNoAccess);

  emptyChunks(lock).push(chunk);
}

ArenaChunk* GCRuntime::pickChunk(StallAndRetry stallAndRetry,
                                 AutoLockGCBgAlloc& lock) {
  if (availableChunks(lock).count()) {
    ArenaChunk* chunk = availableChunks(lock).head();
    availableChunks(lock).remove(chunk);
    return chunk;
  }

  ArenaChunk* chunk = takeOrAllocChunk(stallAndRetry, lock);
  if (!chunk) {
    return nullptr;
  }

#ifdef DEBUG
  chunk->verify();
  MOZ_ASSERT(chunk->isEmpty());
#endif

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
    ArenaChunk* chunk;
    {
      AutoUnlockGC unlock(gcLock);
      void* ptr = ArenaChunk::allocate(gc, StallAndRetry::No);
      if (!ptr) {
        break;
      }
      chunk = ArenaChunk::emplace(ptr, gc, /* allMemoryCommitted = */ true);
    }
    chunkPool_.ref().push(chunk);
  }
}

/* static */
void* ArenaChunk::allocate(GCRuntime* gc, StallAndRetry stallAndRetry) {
  void* chunk = MapAlignedPages(ChunkSize, ChunkSize, stallAndRetry);
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

ArenaChunk* ArenaChunk::emplace(void* ptr, GCRuntime* gc,
                                bool allMemoryCommitted) {
  /* The chunk may still have some regions marked as no-access. */
  MOZ_MAKE_MEM_UNDEFINED(ptr, ChunkSize);

  /*
   * Poison the chunk. Note that decommitAllArenas() below will mark the
   * arenas as inaccessible (for memory sanitizers).
   */
  Poison(ptr, JS_FRESH_TENURED_PATTERN, ChunkSize, MemCheckKind::MakeUndefined);

  ArenaChunk* chunk = new (mozilla::KnownNotNull, ptr) ArenaChunk(gc->rt);

  if (ShouldDecommitNewChunk(allMemoryCommitted, gc->schedulingState)) {
    // Decommit the arenas. We do this after poisoning so that if the OS does
    // not have to recycle the pages, we still get the benefit of poisoning.
    chunk->decommitAllArenas();
  } else {
    chunk->initAsCommitted();
  }

  MOZ_ASSERT(chunk->isEmpty());
  chunk->verify();

  return chunk;
}

void ArenaChunk::decommitAllArenas() {
  MOZ_ASSERT(isEmpty());
  MarkPagesUnusedSoft(&arenas[0], ArenasPerChunk * ArenaSize);
  initAsDecommitted();
}

void ArenaChunkBase::initAsDecommitted() {
  // Set the state of all arenas to free and decommitted. They might not
  // actually be decommitted, but in that case the re-commit operation is a
  // no-op so it doesn't matter.
  decommittedPages.SetAll();
  freeCommittedArenas.ResetAll();
  info.numArenasFree = ArenasPerChunk;
  info.numArenasFreeCommitted = 0;
}

void ArenaChunkBase::initAsCommitted() {
  // Set the state of all arenas to free and committed. They might have
  // been decommitted, but in that case the re-commit operation is a
  // no-op so it doesn't matter.
  decommittedPages.ResetAll();
  freeCommittedArenas.SetAll();
  info.numArenasFree = ArenasPerChunk;
  info.numArenasFreeCommitted = ArenasPerChunk;
}
