/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * Tenured heap management.
 *
 * This file contains method definitions for the following classes for code that
 * is not specific to a particular phase of GC:
 *
 *  - Arena
 *  - ArenaList
 *  - FreeLists
 *  - ArenaLists
 *  - ArenaChunk
 *  - ChunkPool
 */

#include "gc/Heap-inl.h"

#include "gc/GCLock.h"
#include "gc/Memory.h"
#include "jit/Assembler.h"
#include "threading/Thread.h"
#include "vm/BigIntType.h"
#include "vm/RegExpShared.h"
#include "vm/Scope.h"

#include "gc/ArenaList-inl.h"
#include "gc/PrivateIterators-inl.h"

using namespace js;
using namespace js::gc;

// Check that reserved bits of a Cell are compatible with our typical allocators
// since most derived classes will store a pointer in the first word.
static const size_t MinFirstWordAlignment = 1u << CellFlagBitsReservedForGC;
static_assert(js::detail::LIFO_ALLOC_ALIGN >= MinFirstWordAlignment,
              "CellFlagBitsReservedForGC should support LifoAlloc");
static_assert(CellAlignBytes >= MinFirstWordAlignment,
              "CellFlagBitsReservedForGC should support gc::Cell");
static_assert(js::jit::CodeAlignment >= MinFirstWordAlignment,
              "CellFlagBitsReservedForGC should support JIT code");
static_assert(js::gc::JSClassAlignBytes >= MinFirstWordAlignment,
              "CellFlagBitsReservedForGC should support JSClass pointers");
static_assert(js::ScopeDataAlignBytes >= MinFirstWordAlignment,
              "CellFlagBitsReservedForGC should support scope data pointers");

#define CHECK_THING_SIZE(allocKind, traceKind, type, sizedType, bgFinal,       \
                         nursery, compact)                                     \
  static_assert(sizeof(sizedType) >= sizeof(FreeSpan),                         \
                #sizedType " is smaller than FreeSpan");                       \
  static_assert(sizeof(sizedType) % CellAlignBytes == 0,                       \
                "Size of " #sizedType " is not a multiple of CellAlignBytes"); \
  static_assert(sizeof(sizedType) >= MinCellSize,                              \
                "Size of " #sizedType " is smaller than the minimum size");
FOR_EACH_ALLOCKIND(CHECK_THING_SIZE);
#undef CHECK_THING_SIZE

FreeSpan FreeLists::emptySentinel;

template <typename T>
struct ArenaLayout {
  static constexpr size_t thingSize() { return sizeof(T); }
  static constexpr size_t thingsPerArena() {
    return (ArenaSize - ArenaHeaderSize) / thingSize();
  }
  static constexpr size_t firstThingOffset() {
    return ArenaSize - thingSize() * thingsPerArena();
  }
};

const uint8_t Arena::ThingSizes[] = {
#define EXPAND_THING_SIZE(_1, _2, _3, sizedType, _4, _5, _6) \
  ArenaLayout<sizedType>::thingSize(),
    FOR_EACH_ALLOCKIND(EXPAND_THING_SIZE)
#undef EXPAND_THING_SIZE
};

const uint8_t Arena::FirstThingOffsets[] = {
#define EXPAND_FIRST_THING_OFFSET(_1, _2, _3, sizedType, _4, _5, _6) \
  ArenaLayout<sizedType>::firstThingOffset(),
    FOR_EACH_ALLOCKIND(EXPAND_FIRST_THING_OFFSET)
#undef EXPAND_FIRST_THING_OFFSET
};

const uint8_t Arena::ThingsPerArena[] = {
#define EXPAND_THINGS_PER_ARENA(_1, _2, _3, sizedType, _4, _5, _6) \
  ArenaLayout<sizedType>::thingsPerArena(),
    FOR_EACH_ALLOCKIND(EXPAND_THINGS_PER_ARENA)
#undef EXPAND_THINGS_PER_ARENA
};

bool Arena::allocated() const {
#if defined(DEBUG) && defined(MOZ_VALGRIND)
  // In debug builds, valgrind complains about the access to `allocKind` even
  // though it is legitimate, so temporarily disable reporting of addressing
  // errors in that range.  Note this doesn't change the state of the address
  // range, as tracked by valgrind, so subsequent checking against its state is
  // unaffected.  See bug 1932412.
  VALGRIND_DISABLE_ADDR_ERROR_REPORTING_IN_RANGE(&allocKind, sizeof(void*));
#endif

  size_t arenaIndex = ArenaChunk::arenaIndex(this);
  size_t pageIndex = ArenaChunk::arenaToPageIndex(arenaIndex);
  bool result = !chunk()->decommittedPages[pageIndex] &&
                !chunk()->freeCommittedArenas[arenaIndex] &&
                IsValidAllocKind(allocKind);
  MOZ_ASSERT_IF(result, zone_);
  MOZ_ASSERT_IF(result, (uintptr_t(zone_) & 7) == 0);

#if defined(DEBUG) && defined(MOZ_VALGRIND)
  // Reenable error reporting for the range we just said to ignore.
  VALGRIND_ENABLE_ADDR_ERROR_REPORTING_IN_RANGE(&allocKind, sizeof(void*));
#endif
  return result;
}

void Arena::unmarkAll() {
  MarkBitmapWord* arenaBits = chunk()->markBits.arenaBits(this);
  for (size_t i = 0; i < ArenaBitmapWords; i++) {
    arenaBits[i] = 0;
  }
}

void Arena::unmarkPreMarkedFreeCells() {
  for (ArenaFreeCellIter cell(this); !cell.done(); cell.next()) {
    MOZ_ASSERT(cell->isMarkedBlack());
    cell->unmark();
  }
}

#ifdef DEBUG

void Arena::checkNoMarkedFreeCells() {
  for (ArenaFreeCellIter cell(this); !cell.done(); cell.next()) {
    MOZ_ASSERT(!cell->isMarkedAny());
  }
}

void Arena::checkAllCellsMarkedBlack() {
  for (ArenaCellIter cell(this); !cell.done(); cell.next()) {
    MOZ_ASSERT(cell->isMarkedBlack());
  }
}

#endif

#if defined(DEBUG) || defined(JS_GC_ZEAL)
void Arena::checkNoMarkedCells() {
  for (ArenaCellIter cell(this); !cell.done(); cell.next()) {
    MOZ_ASSERT(!cell->isMarkedAny());
  }
}
#endif

/* static */
void Arena::staticAsserts() {
  static_assert(size_t(AllocKind::LIMIT) <= 255,
                "All AllocKinds and AllocKind::LIMIT must fit in a uint8_t.");
  static_assert(std::size(ThingSizes) == AllocKindCount,
                "We haven't defined all thing sizes.");
  static_assert(std::size(FirstThingOffsets) == AllocKindCount,
                "We haven't defined all offsets.");
  static_assert(std::size(ThingsPerArena) == AllocKindCount,
                "We haven't defined all counts.");
  static_assert(ArenaZoneOffset == offsetof(Arena, zone_),
                "The hardcoded API zone offset must match the actual offset.");
  static_assert(sizeof(Arena) == ArenaSize,
                "ArenaSize must match the actual size of the Arena structure.");
  static_assert(
      offsetof(Arena, data) == ArenaHeaderSize,
      "ArenaHeaderSize must match the actual size of the header fields.");
}

/* static */
void Arena::checkLookupTables() {
#ifdef DEBUG
  for (size_t i = 0; i < AllocKindCount; i++) {
    MOZ_ASSERT(
        FirstThingOffsets[i] + ThingsPerArena[i] * ThingSizes[i] == ArenaSize,
        "Inconsistent arena lookup table data");
  }
#endif
}

#ifdef DEBUG
void js::gc::ArenaList::dump() {
  fprintf(stderr, "ArenaList %p:\n", this);
  for (auto arena = iter(); !arena.done(); arena.next()) {
    fprintf(stderr, "  %p %zu", arena.get(), arena->countFreeCells());
    if (arena->isEmpty()) {
      fprintf(stderr, " (empty)");
    }
    if (arena->isFull()) {
      fprintf(stderr, " (full)");
    }
    fprintf(stderr, "\n");
  }
}
#endif

AutoGatherSweptArenas::AutoGatherSweptArenas(JS::Zone* zone, AllocKind kind) {
  GCRuntime& gc = zone->runtimeFromMainThread()->gc;
  sortedList = gc.maybeGetForegroundFinalizedArenas(zone, kind);
  if (!sortedList) {
    return;
  }

  // Link individual sorted arena lists together for iteration, saving the
  // internal state so we can restore it later.
  linked = sortedList->convertToArenaList(bucketLastPointers);
}

AutoGatherSweptArenas::~AutoGatherSweptArenas() {
  if (!sortedList) {
    MOZ_ASSERT(linked.isEmpty());
    return;
  }

  sortedList->restoreFromArenaList(linked, bucketLastPointers);
}

FreeLists::FreeLists() {
  for (auto i : AllAllocKinds()) {
    freeLists_[i] = &emptySentinel;
  }
}

ArenaLists::ArenaLists(Zone* zone)
    : zone_(zone),
      gcCompactPropMapArenasToUpdate(nullptr),
      gcNormalPropMapArenasToUpdate(nullptr),
      savedEmptyArenas(nullptr) {
  for (auto i : AllAllocKinds()) {
    concurrentUse(i) = ConcurrentUse::None;
  }
}

ArenaLists::~ArenaLists() {
  AutoLockGC lock(runtime());

  for (auto i : AllAllocKinds()) {
    /*
     * We can only call this during the shutdown after the last GC when
     * the background finalization is disabled.
     */
    MOZ_ASSERT(concurrentUse(i) == ConcurrentUse::None);
    runtime()->gc.releaseArenaList(arenaList(i), lock);
  }

  runtime()->gc.releaseArenas(savedEmptyArenas, lock);
}

void ArenaLists::moveArenasToCollectingLists() {
  checkEmptyFreeLists();
  for (AllocKind kind : AllAllocKinds()) {
    MOZ_ASSERT(collectingArenaList(kind).isEmpty());
    collectingArenaList(kind) = std::move(arenaList(kind));
    MOZ_ASSERT(arenaList(kind).isEmpty());
  }
}

void ArenaLists::mergeArenasFromCollectingLists() {
  for (AllocKind kind : AllAllocKinds()) {
    arenaList(kind).prepend(std::move(collectingArenaList(kind)));
    MOZ_ASSERT(collectingArenaList(kind).isEmpty());
  }
}

Arena* ArenaLists::takeSweptEmptyArenas() {
  Arena* arenas = savedEmptyArenas;
  savedEmptyArenas = nullptr;
  return arenas;
}

void ArenaLists::checkGCStateNotInUse() {
  // Called before and after collection to check the state is as expected.
#ifdef DEBUG
  checkSweepStateNotInUse();
  for (auto i : AllAllocKinds()) {
    MOZ_ASSERT(collectingArenaList(i).isEmpty());
  }
#endif
}

void ArenaLists::checkSweepStateNotInUse() {
#ifdef DEBUG
  checkNoArenasToUpdate();
  MOZ_ASSERT(!savedEmptyArenas);
  for (auto i : AllAllocKinds()) {
    MOZ_ASSERT(concurrentUse(i) == ConcurrentUse::None);
  }
#endif
}

void ArenaLists::checkNoArenasToUpdate() {
  MOZ_ASSERT(!gcCompactPropMapArenasToUpdate);
  MOZ_ASSERT(!gcNormalPropMapArenasToUpdate);
}

void ArenaLists::checkNoArenasToUpdateForKind(AllocKind kind) {
#ifdef DEBUG
  switch (kind) {
    case AllocKind::COMPACT_PROP_MAP:
      MOZ_ASSERT(!gcCompactPropMapArenasToUpdate);
      break;
    case AllocKind::NORMAL_PROP_MAP:
      MOZ_ASSERT(!gcNormalPropMapArenasToUpdate);
      break;
    default:
      break;
  }
#endif
}

inline bool ArenaChunk::canDecommitPage(size_t pageIndex) const {
  if (decommittedPages[pageIndex]) {
    return false;
  }

  size_t arenaIndex = pageToArenaIndex(pageIndex);
  for (size_t i = 0; i < ArenasPerPage; i++) {
    if (!freeCommittedArenas[arenaIndex + i]) {
      return false;
    }
  }

  return true;
}

void ArenaChunk::decommitFreeArenas(GCRuntime* gc, const bool& cancel,
                                    AutoLockGC& lock) {
  MOZ_ASSERT(DecommitEnabled());
  MOZ_ASSERT(!info.isCurrentChunk);

  for (size_t i = 0; i < PagesPerChunk; i++) {
    if (cancel) {
      break;
    }

    if (!canDecommitPage(i)) {
      continue;
    }

    if (!decommitOneFreePage(gc, i, lock)) {
      break;
    }

    {
      // Give main thread a chance to take the lock.
      AutoUnlockGC unlock(lock);
      ThisThread::SleepMilliseconds(0);
    }

    // Re-check whether the chunk is being used for allocation after releasing
    // the lock.
    if (info.isCurrentChunk) {
      break;
    }
  }
}

void ArenaChunk::releaseArena(GCRuntime* gc, Arena* arena,
                              const AutoLockGC& lock) {
  if (info.isCurrentChunk) {
    // The main thread is allocating out of this chunk without holding the
    // lock. Don't touch any data structures it is using but add the arena to a
    // pending set. This will be merged back by mergePendingFreeArenas.
    auto& bitmap = gc->pendingFreeCommittedArenas.ref();
    MOZ_ASSERT(!bitmap[arenaIndex(arena)]);
    bitmap[arenaIndex(arena)] = true;
    return;
  }

  MOZ_ASSERT(!arena->allocated());
  MOZ_ASSERT(!freeCommittedArenas[arenaIndex(arena)]);

  freeCommittedArenas[arenaIndex(arena)] = true;
  updateFreeCountsAfterFree(gc, 1, true, lock);

  verify();
}

bool ArenaChunk::decommitOneFreePage(GCRuntime* gc, size_t pageIndex,
                                     const AutoLockGC& lock) {
  MOZ_ASSERT(DecommitEnabled());
  MOZ_ASSERT(canDecommitPage(pageIndex));
  MOZ_ASSERT(info.numArenasFree >= info.numArenasFreeCommitted);
  MOZ_ASSERT(info.numArenasFreeCommitted >= ArenasPerPage);

  if (oom::ShouldFailWithOOM()) {
    return false;
  }

  if (!MarkPagesUnusedSoft(pageAddress(pageIndex), PageSize)) {
    return false;
  }

  // Mark the page as decommited.
  decommittedPages[pageIndex] = true;
  for (size_t i = 0; i < ArenasPerPage; i++) {
    size_t arenaIndex = pageToArenaIndex(pageIndex) + i;
    MOZ_ASSERT(freeCommittedArenas[arenaIndex]);
    freeCommittedArenas[arenaIndex] = false;
  }
  info.numArenasFreeCommitted -= ArenasPerPage;

  verify();

  return true;
}

void ArenaChunk::decommitFreeArenasWithoutUnlocking(const AutoLockGC& lock) {
  MOZ_ASSERT(DecommitEnabled());

  for (size_t i = 0; i < PagesPerChunk; i++) {
    if (!canDecommitPage(i)) {
      continue;
    }

    MOZ_ASSERT(!decommittedPages[i]);
    MOZ_ASSERT(info.numArenasFreeCommitted >= ArenasPerPage);

    if (js::oom::ShouldFailWithOOM() ||
        !MarkPagesUnusedSoft(pageAddress(i), SystemPageSize())) {
      break;
    }

    decommittedPages[i] = true;
    for (size_t j = 0; j < ArenasPerPage; ++j) {
      size_t arenaIndex = pageToArenaIndex(i) + j;
      MOZ_ASSERT(freeCommittedArenas[arenaIndex]);
      freeCommittedArenas[arenaIndex] = false;
    }
    info.numArenasFreeCommitted -= ArenasPerPage;
  }

  verify();
}

void ArenaChunk::updateFreeCountsAfterAlloc(GCRuntime* gc,
                                            size_t numArenasAlloced,
                                            const AutoLockGC& lock) {
  MOZ_ASSERT(!info.isCurrentChunk);
  MOZ_ASSERT(numArenasAlloced > 0);

  bool wasEmpty = isEmpty();

  MOZ_ASSERT(info.numArenasFree >= numArenasAlloced);
  MOZ_ASSERT(info.numArenasFreeCommitted >= numArenasAlloced);
  info.numArenasFreeCommitted -= numArenasAlloced;
  info.numArenasFree -= numArenasAlloced;

  if (MOZ_UNLIKELY(wasEmpty)) {
    gc->emptyChunks(lock).remove(this);
    gc->availableChunks(lock).push(this);
    return;
  }

  if (MOZ_UNLIKELY(isFull())) {
    gc->availableChunks(lock).remove(this);
    gc->fullChunks(lock).push(this);
    return;
  }

  MOZ_ASSERT(gc->availableChunks(lock).contains(this));
}

void ArenaChunk::updateCurrentChunkAfterAlloc(GCRuntime* gc) {
  MOZ_ASSERT(info.isCurrentChunk);  // Can access without holding lock.
  MOZ_ASSERT(gc->isCurrentChunk(this));

  MOZ_ASSERT(info.numArenasFree >= 1);
  MOZ_ASSERT(info.numArenasFreeCommitted >= 1);
  info.numArenasFreeCommitted--;
  info.numArenasFree--;

  if (MOZ_UNLIKELY(isFull())) {
    AutoLockGC lock(gc);
    mergePendingFreeArenas(gc, lock);
    if (isFull()) {
      gc->clearCurrentChunk(lock);
    }
  }
}

void ArenaChunk::updateFreeCountsAfterFree(GCRuntime* gc, size_t numArenasFreed,
                                           bool wasCommitted,
                                           const AutoLockGC& lock) {
  MOZ_ASSERT(!info.isCurrentChunk);
  MOZ_ASSERT(numArenasFreed > 0);
  MOZ_ASSERT(info.numArenasFree + numArenasFreed <= ArenasPerChunk);
  MOZ_ASSERT(info.numArenasFreeCommitted + numArenasFreed <= ArenasPerChunk);

  bool wasFull = isFull();

  info.numArenasFree += numArenasFreed;
  if (wasCommitted) {
    info.numArenasFreeCommitted += numArenasFreed;
  }

  if (MOZ_UNLIKELY(wasFull)) {
    gc->fullChunks(lock).remove(this);
    gc->availableChunks(lock).push(this);
    return;
  }

  if (MOZ_UNLIKELY(isEmpty())) {
    gc->availableChunks(lock).remove(this);
    gc->recycleChunk(this, lock);
    return;
  }

  MOZ_ASSERT(gc->availableChunks(lock).contains(this));
}

void GCRuntime::setCurrentChunk(ArenaChunk* chunk, const AutoLockGC& lock) {
  MOZ_ASSERT(!currentChunk_);
  MOZ_ASSERT(pendingFreeCommittedArenas.ref().IsEmpty());
  MOZ_ASSERT(chunk);
  MOZ_ASSERT(!chunk->info.isCurrentChunk);

  currentChunk_ = chunk;
  chunk->info.isCurrentChunk = true;  // Lock needed here.
}

void GCRuntime::clearCurrentChunk(const AutoLockGC& lock) {
  MOZ_ASSERT(CurrentThreadCanAccessRuntime(rt));

  ArenaChunk* chunk = currentChunk_;
  if (!chunk) {
    return;
  }

  chunk->mergePendingFreeArenas(this, lock);

  MOZ_ASSERT(chunk->info.isCurrentChunk);
  chunk->info.isCurrentChunk = false;  // Lock needed here.
  currentChunk_ = nullptr;

  if (chunk->isFull()) {
    fullChunks(lock).push(chunk);
    return;
  }

  if (chunk->isEmpty()) {
    emptyChunks(lock).push(chunk);
    return;
  }

  MOZ_ASSERT(chunk->hasAvailableArenas());
  availableChunks(lock).push(chunk);
}

void ArenaChunk::mergePendingFreeArenas(GCRuntime* gc, const AutoLockGC& lock) {
  MOZ_ASSERT(info.isCurrentChunk);

  auto& bitmap = gc->pendingFreeCommittedArenas.ref();
  if (bitmap.IsEmpty()) {
    return;
  }

  MOZ_ASSERT((freeCommittedArenas & bitmap).IsEmpty());
  size_t count = bitmap.Count();
  freeCommittedArenas |= bitmap;
  bitmap.ResetAll();

  info.numArenasFree += count;
  info.numArenasFreeCommitted += count;
}

ArenaChunk* ChunkPool::pop() {
  MOZ_ASSERT(bool(head_) == bool(count_));
  if (!count_) {
    return nullptr;
  }
  return remove(head_);
}

void ChunkPool::push(ArenaChunk* chunk) {
  MOZ_ASSERT(!chunk->info.next);
  MOZ_ASSERT(!chunk->info.prev);

  chunk->info.next = head_;
  if (head_) {
    head_->info.prev = chunk;
  }
  head_ = chunk;
  ++count_;
}

ArenaChunk* ChunkPool::remove(ArenaChunk* chunk) {
  MOZ_ASSERT(count_ > 0);
  MOZ_ASSERT(contains(chunk));

  if (head_ == chunk) {
    head_ = chunk->info.next;
  }
  if (chunk->info.prev) {
    chunk->info.prev->info.next = chunk->info.next;
  }
  if (chunk->info.next) {
    chunk->info.next->info.prev = chunk->info.prev;
  }
  chunk->info.next = chunk->info.prev = nullptr;
  --count_;

  return chunk;
}

// We could keep the chunk pool sorted, but that's likely to be more expensive.
// This sort is nlogn, but keeping it sorted is likely to be m*n, with m being
// the number of operations (likely higher than n).
void ChunkPool::sort() {
  // Only sort if the list isn't already sorted.
  if (!isSorted()) {
    head_ = mergeSort(head(), count());

    // Fixup prev pointers.
    ArenaChunk* prev = nullptr;
    for (ArenaChunk* cur = head_; cur; cur = cur->info.next) {
      cur->info.prev = prev;
      prev = cur;
    }
  }

  MOZ_ASSERT(verify());
  MOZ_ASSERT(isSorted());
}

ArenaChunk* ChunkPool::mergeSort(ArenaChunk* list, size_t count) {
  MOZ_ASSERT(bool(list) == bool(count));

  if (count < 2) {
    return list;
  }

  size_t half = count / 2;

  // Split;
  ArenaChunk* front = list;
  ArenaChunk* back;
  {
    ArenaChunk* cur = list;
    for (size_t i = 0; i < half - 1; i++) {
      MOZ_ASSERT(cur);
      cur = cur->info.next;
    }
    back = cur->info.next;
    cur->info.next = nullptr;
  }

  front = mergeSort(front, half);
  back = mergeSort(back, count - half);

  // Merge
  list = nullptr;
  ArenaChunk** cur = &list;
  while (front || back) {
    if (!front) {
      *cur = back;
      break;
    }
    if (!back) {
      *cur = front;
      break;
    }

    // Note that the sort is stable due to the <= here. Nothing depends on
    // this but it could.
    if (front->info.numArenasFree <= back->info.numArenasFree) {
      *cur = front;
      front = front->info.next;
      cur = &(*cur)->info.next;
    } else {
      *cur = back;
      back = back->info.next;
      cur = &(*cur)->info.next;
    }
  }

  return list;
}

bool ChunkPool::isSorted() const {
  uint32_t last = 1;
  for (ArenaChunk* cursor = head_; cursor; cursor = cursor->info.next) {
    if (cursor->info.numArenasFree < last) {
      return false;
    }
    last = cursor->info.numArenasFree;
  }
  return true;
}

#ifdef DEBUG

bool ChunkPool::contains(ArenaChunk* chunk) const {
  verify();
  for (ArenaChunk* cursor = head_; cursor; cursor = cursor->info.next) {
    if (cursor == chunk) {
      return true;
    }
  }
  return false;
}

bool ChunkPool::verify() const {
  MOZ_ASSERT(bool(head_) == bool(count_));
  uint32_t count = 0;
  for (ArenaChunk* cursor = head_; cursor;
       cursor = cursor->info.next, ++count) {
    MOZ_ASSERT_IF(cursor->info.prev, cursor->info.prev->info.next == cursor);
    MOZ_ASSERT_IF(cursor->info.next, cursor->info.next->info.prev == cursor);
  }
  MOZ_ASSERT(count_ == count);
  return true;
}

void ChunkPool::verifyChunks() const {
  for (ArenaChunk* chunk = head_; chunk; chunk = chunk->info.next) {
    chunk->verify();
    MOZ_ASSERT(!chunk->info.isCurrentChunk);
  }
}

void ArenaChunk::verify() const {
  // Check the mark bits for each arena are aligned to the cache line size.
  static_assert((offsetof(ArenaChunk, arenas) % ArenaSize) == 0);
  constexpr size_t CellBytesPerMarkByte = CellBytesPerMarkBit * 8;
  static_assert((ArenaSize % CellBytesPerMarkByte) == 0);
  constexpr size_t MarkBytesPerArena = ArenaSize / CellBytesPerMarkByte;
  static_assert((MarkBytesPerArena % TypicalCacheLineSize) == 0);
  static_assert((offsetof(ArenaChunk, markBits) % TypicalCacheLineSize) == 0);

  MOZ_ASSERT(info.numArenasFree <= ArenasPerChunk);
  MOZ_ASSERT(info.numArenasFreeCommitted <= info.numArenasFree);

  size_t decommittedCount = decommittedPages.Count() * ArenasPerPage;
  size_t freeCommittedCount = freeCommittedArenas.Count();
  size_t freeCount = freeCommittedCount + decommittedCount;

  MOZ_ASSERT(freeCount == info.numArenasFree);
  MOZ_ASSERT(freeCommittedCount == info.numArenasFreeCommitted);

  for (size_t i = 0; i < ArenasPerChunk; ++i) {
    MOZ_ASSERT(
        !(decommittedPages[arenaToPageIndex(i)] && freeCommittedArenas[i]));
  }
}

#endif

void ChunkPool::Iter::next() {
  MOZ_ASSERT(!done());
  current_ = current_->info.next;
}
