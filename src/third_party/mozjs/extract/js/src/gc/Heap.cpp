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
 *  - TenuredChunk
 *  - ChunkPool
 */

#include "gc/Heap-inl.h"

#include "gc/GCLock.h"
#include "gc/Memory.h"
#include "jit/Assembler.h"
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
  static_assert(sizeof(sizedType) >= SortedArenaList::MinThingSize,            \
                #sizedType " is smaller than SortedArenaList::MinThingSize!"); \
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
  fprintf(stderr, "ArenaList %p:", this);
  if (cursorp_ == &head_) {
    fprintf(stderr, " *");
  }
  for (Arena* arena = head(); arena; arena = arena->next) {
    fprintf(stderr, " %p", arena);
    if (cursorp_ == &arena->next) {
      fprintf(stderr, " *");
    }
  }
  fprintf(stderr, "\n");
}
#endif

Arena* ArenaList::removeRemainingArenas(Arena** arenap) {
  // This is only ever called to remove arenas that are after the cursor, so
  // we don't need to update it.
#ifdef DEBUG
  for (Arena* arena = *arenap; arena; arena = arena->next) {
    MOZ_ASSERT(cursorp_ != &arena->next);
  }
#endif
  Arena* remainingArenas = *arenap;
  *arenap = nullptr;
  check();
  return remainingArenas;
}

FreeLists::FreeLists() {
  for (auto i : AllAllocKinds()) {
    freeLists_[i] = &emptySentinel;
  }
}

ArenaLists::ArenaLists(Zone* zone)
    : zone_(zone),
      incrementalSweptArenaKind(AllocKind::LIMIT),
      gcCompactPropMapArenasToUpdate(nullptr),
      gcNormalPropMapArenasToUpdate(nullptr),
      savedEmptyArenas(nullptr) {
  for (auto i : AllAllocKinds()) {
    concurrentUse(i) = ConcurrentUse::None;
  }
}

void ReleaseArenas(JSRuntime* rt, Arena* arena, const AutoLockGC& lock) {
  Arena* next;
  for (; arena; arena = next) {
    next = arena->next;
    rt->gc.releaseArena(arena, lock);
  }
}

void ReleaseArenaList(JSRuntime* rt, ArenaList& arenaList,
                      const AutoLockGC& lock) {
  ReleaseArenas(rt, arenaList.head(), lock);
  arenaList.clear();
}

ArenaLists::~ArenaLists() {
  AutoLockGC lock(runtime());

  for (auto i : AllAllocKinds()) {
    /*
     * We can only call this during the shutdown after the last GC when
     * the background finalization is disabled.
     */
    MOZ_ASSERT(concurrentUse(i) == ConcurrentUse::None);
    ReleaseArenaList(runtime(), arenaList(i), lock);
  }
  ReleaseArenaList(runtime(), incrementalSweptArenas.ref(), lock);

  ReleaseArenas(runtime(), savedEmptyArenas, lock);
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
    collectingArenaList(kind).insertListWithCursorAtEnd(arenaList(kind));
    arenaList(kind) = std::move(collectingArenaList(kind));
    MOZ_ASSERT(collectingArenaList(kind).isEmpty());
  }
}

Arena* ArenaLists::takeSweptEmptyArenas() {
  Arena* arenas = savedEmptyArenas;
  savedEmptyArenas = nullptr;
  return arenas;
}

void ArenaLists::setIncrementalSweptArenas(AllocKind kind,
                                           SortedArenaList& arenas) {
  incrementalSweptArenaKind = kind;
  incrementalSweptArenas.ref().clear();
  incrementalSweptArenas = arenas.toArenaList();
}

void ArenaLists::clearIncrementalSweptArenas() {
  incrementalSweptArenaKind = AllocKind::LIMIT;
  incrementalSweptArenas.ref().clear();
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
  MOZ_ASSERT(incrementalSweptArenaKind == AllocKind::LIMIT);
  MOZ_ASSERT(incrementalSweptArenas.ref().isEmpty());
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

inline bool TenuredChunk::canDecommitPage(size_t pageIndex) const {
  if (decommittedPages[pageIndex]) {
    return false;
  }

  size_t arenaIndex = pageIndex * ArenasPerPage;
  for (size_t i = 0; i < ArenasPerPage; i++) {
    if (!freeCommittedArenas[arenaIndex + i]) {
      return false;
    }
  }

  return true;
}

void TenuredChunk::decommitFreeArenas(GCRuntime* gc, const bool& cancel,
                                      AutoLockGC& lock) {
  MOZ_ASSERT(DecommitEnabled());

  for (size_t i = 0; i < PagesPerChunk; i++) {
    if (cancel) {
      break;
    }

    if (canDecommitPage(i) && !decommitOneFreePage(gc, i, lock)) {
      break;
    }
  }
}

void TenuredChunk::recycleArena(Arena* arena, SortedArenaList& dest,
                                size_t thingsPerArena) {
  arena->setAsFullyUnused();
  dest.insertAt(arena, thingsPerArena);
}

void TenuredChunk::releaseArena(GCRuntime* gc, Arena* arena,
                                const AutoLockGC& lock) {
  MOZ_ASSERT(!arena->allocated());
  MOZ_ASSERT(!freeCommittedArenas[arenaIndex(arena)]);

  freeCommittedArenas[arenaIndex(arena)] = true;
  ++info.numArenasFreeCommitted;
  ++info.numArenasFree;
  gc->updateOnArenaFree();

  verify();

  updateChunkListAfterFree(gc, 1, lock);
}

bool TenuredChunk::decommitOneFreePage(GCRuntime* gc, size_t pageIndex,
                                       AutoLockGC& lock) {
  MOZ_ASSERT(DecommitEnabled());
  MOZ_ASSERT(canDecommitPage(pageIndex));
  MOZ_ASSERT(info.numArenasFreeCommitted >= ArenasPerPage);

  // Temporarily mark the page as allocated while we decommit.
  for (size_t i = 0; i < ArenasPerPage; i++) {
    size_t arenaIndex = pageIndex * ArenasPerPage + i;
    MOZ_ASSERT(freeCommittedArenas[arenaIndex]);
    freeCommittedArenas[arenaIndex] = false;
  }
  info.numArenasFreeCommitted -= ArenasPerPage;
  info.numArenasFree -= ArenasPerPage;
  updateChunkListAfterAlloc(gc, lock);

  verify();

  bool ok;
  {
    AutoUnlockGC unlock(lock);
    ok = !oom::ShouldFailWithOOM() &&
         MarkPagesUnusedSoft(pageAddress(pageIndex), PageSize);
  }

  // Mark the page as decommited if successful or restore the original free
  // state.
  if (ok) {
    decommittedPages[pageIndex] = true;
  } else {
    for (size_t i = 0; i < ArenasPerPage; i++) {
      size_t arenaIndex = pageIndex * ArenasPerPage + i;
      MOZ_ASSERT(!freeCommittedArenas[arenaIndex]);
      freeCommittedArenas[arenaIndex] = true;
    }
    info.numArenasFreeCommitted += ArenasPerPage;
  }

  info.numArenasFree += ArenasPerPage;
  updateChunkListAfterFree(gc, ArenasPerPage, lock);

  verify();

  return ok;
}

void TenuredChunk::decommitFreeArenasWithoutUnlocking(const AutoLockGC& lock) {
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
      size_t arenaIndex = i * ArenasPerPage + j;
      MOZ_ASSERT(freeCommittedArenas[arenaIndex]);
      freeCommittedArenas[arenaIndex] = false;
    }
    info.numArenasFreeCommitted -= ArenasPerPage;
  }

  verify();
}

void TenuredChunk::updateChunkListAfterAlloc(GCRuntime* gc,
                                             const AutoLockGC& lock) {
  if (MOZ_UNLIKELY(!hasAvailableArenas())) {
    gc->availableChunks(lock).remove(this);
    gc->fullChunks(lock).push(this);
  }
}

void TenuredChunk::updateChunkListAfterFree(GCRuntime* gc, size_t numArenasFree,
                                            const AutoLockGC& lock) {
  if (info.numArenasFree == numArenasFree) {
    gc->fullChunks(lock).remove(this);
    gc->availableChunks(lock).push(this);
  } else if (!unused()) {
    MOZ_ASSERT(gc->availableChunks(lock).contains(this));
  } else {
    MOZ_ASSERT(unused());
    gc->availableChunks(lock).remove(this);
    gc->recycleChunk(this, lock);
  }
}

TenuredChunk* ChunkPool::pop() {
  MOZ_ASSERT(bool(head_) == bool(count_));
  if (!count_) {
    return nullptr;
  }
  return remove(head_);
}

void ChunkPool::push(TenuredChunk* chunk) {
  MOZ_ASSERT(!chunk->info.next);
  MOZ_ASSERT(!chunk->info.prev);

  chunk->info.next = head_;
  if (head_) {
    head_->info.prev = chunk;
  }
  head_ = chunk;
  ++count_;
}

TenuredChunk* ChunkPool::remove(TenuredChunk* chunk) {
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
    TenuredChunk* prev = nullptr;
    for (TenuredChunk* cur = head_; cur; cur = cur->info.next) {
      cur->info.prev = prev;
      prev = cur;
    }
  }

  MOZ_ASSERT(verify());
  MOZ_ASSERT(isSorted());
}

TenuredChunk* ChunkPool::mergeSort(TenuredChunk* list, size_t count) {
  MOZ_ASSERT(bool(list) == bool(count));

  if (count < 2) {
    return list;
  }

  size_t half = count / 2;

  // Split;
  TenuredChunk* front = list;
  TenuredChunk* back;
  {
    TenuredChunk* cur = list;
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
  TenuredChunk** cur = &list;
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
  for (TenuredChunk* cursor = head_; cursor; cursor = cursor->info.next) {
    if (cursor->info.numArenasFree < last) {
      return false;
    }
    last = cursor->info.numArenasFree;
  }
  return true;
}

#ifdef DEBUG

bool ChunkPool::contains(TenuredChunk* chunk) const {
  verify();
  for (TenuredChunk* cursor = head_; cursor; cursor = cursor->info.next) {
    if (cursor == chunk) {
      return true;
    }
  }
  return false;
}

bool ChunkPool::verify() const {
  MOZ_ASSERT(bool(head_) == bool(count_));
  uint32_t count = 0;
  for (TenuredChunk* cursor = head_; cursor;
       cursor = cursor->info.next, ++count) {
    MOZ_ASSERT_IF(cursor->info.prev, cursor->info.prev->info.next == cursor);
    MOZ_ASSERT_IF(cursor->info.next, cursor->info.next->info.prev == cursor);
  }
  MOZ_ASSERT(count_ == count);
  return true;
}

void ChunkPool::verifyChunks() const {
  for (TenuredChunk* chunk = head_; chunk; chunk = chunk->info.next) {
    chunk->verify();
  }
}

void TenuredChunk::verify() const {
  MOZ_ASSERT(info.numArenasFree <= ArenasPerChunk);
  MOZ_ASSERT(info.numArenasFreeCommitted <= info.numArenasFree);

  size_t decommittedCount = decommittedPages.Count() * ArenasPerPage;
  size_t freeCommittedCount = freeCommittedArenas.Count();
  size_t freeCount = freeCommittedCount + decommittedCount;

  MOZ_ASSERT(freeCount == info.numArenasFree);
  MOZ_ASSERT(freeCommittedCount == info.numArenasFreeCommitted);

  for (size_t i = 0; i < ArenasPerChunk; ++i) {
    MOZ_ASSERT(!(decommittedPages[pageIndex(i)] && freeCommittedArenas[i]));
    MOZ_ASSERT_IF(freeCommittedArenas[i], !arenas[i].allocated());
  }
}

#endif

void ChunkPool::Iter::next() {
  MOZ_ASSERT(!done());
  current_ = current_->info.next;
}
