/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * GC-internal definitions of ArenaList and associated heap data structures.
 */

#ifndef gc_ArenaList_h
#define gc_ArenaList_h

#include <utility>

#include "ds/SinglyLinkedList.h"
#include "gc/AllocKind.h"
#include "gc/Memory.h"
#include "js/GCAPI.h"
#include "js/HeapAPI.h"
#include "js/TypeDecls.h"
#include "threading/ProtectedData.h"

namespace JS {
class SliceBudget;
}

namespace js {

class Nursery;

namespace gcstats {
struct Statistics;
}

namespace gc {

class Arena;
class ArenaIter;
class ArenaIterInGC;
class AutoGatherSweptArenas;
class BackgroundUnmarkTask;
struct FinalizePhase;
class FreeSpan;
class TenuredCell;
class TenuringTracer;

// Whether or not to release empty arenas while sweeping.
enum class ReleaseEmpty : bool { No = false, Yes = true };

/*
 * Arena lists contain a singly linked lists of arenas.
 *
 * Arenas with free space are positioned at the front of the list, sorted in
 * order of increasing free space (which is the order we intend to use them):
 *
 * For example, the following shows a possible arrangement of arenas with count
 * of used cells on Y axis:
 *
 *   |                                       +----+----+----+----+----+
 *   |                                       |    |    |    |    |    |
 *   |                                       |    |    |    |    |    |
 *   +----+----+                             |    |    |    |    |    |
 *   |    |    +----+                        |    |    |    |    |    |
 *   |    |    |    +----+                   |    |    |    |    |    |
 *   |    |    |    |    |                   |    |    |    |    |    |
 *   |    |    |    |    +----+              |    |    |    |    |    |
 *   |    |    |    |    |    |              |    |    |    |    |    |
 *   |    |    |    |    |    +----+----+    |    |    |    |    |    |
 *   |    |    |    |    |    |    |    +----+    |    |    |    |    |
 *   +----+----+----+----+----+----+----+----+----+----+----+----+----+
 *
 * This provides a convenient way of getting the next arena with free space when
 * allocating: the arena at the front of the list is taken, marked as fully
 * allocated and moved to the end of the list.
 *
 * The ordering is chosen to try and fill up arenas as much as possible and
 * leave more empty arenas to be reclaimed when their contents die.
 *
 * The ordering should not be treated as an invariant, however, as the free
 * lists may be cleared, leaving arenas previously used for allocation partially
 * full. Sorting order is restored during sweeping.
 */
class ArenaList : public SinglyLinkedList<Arena> {
 public:
  inline bool hasNonFullArenas() const;

  // This returns the first arena if it has any free space, moving it to the
  // back of the list. If there are no arenas or if the first one is full then
  // return nullptr.
  inline Arena* takeInitialNonFullArena();

  std::pair<Arena*, Arena*> pickArenasToRelocate(AllocKind kind,
                                                 size_t& arenaTotalOut,
                                                 size_t& relocTotalOut);

  Arena* relocateArenas(Arena* toRelocate, Arena* relocated,
                        JS::SliceBudget& sliceBudget,
                        gcstats::Statistics& stats);

#ifdef DEBUG
  void dump();
#endif
};

/*
 * A class that is used to sort arenas of a single AllocKind into increasing
 * order of free space.
 *
 * It works by adding arenas to a bucket corresponding to the number of free
 * things in the arena. Each bucket is an independent linked list.
 *
 * The buckets can be linked up to form a sorted ArenaList.
 */
class SortedArenaList {
 public:
  static_assert(ArenaSize <= 4096,
                "When increasing the Arena size, please consider how"
                " this will affect the size of a SortedArenaList.");

  static_assert(MinCellSize >= 16,
                "When decreasing the minimum thing size, please consider"
                " how this will affect the size of a SortedArenaList.");

  // The maximum number of GC things that an arena can hold.
  static const size_t MaxThingsPerArena =
      (ArenaSize - ArenaHeaderSize) / MinCellSize;

  // The number of buckets required: one full arenas, one for empty arenas and
  // half the number of remaining size classes.
  static const size_t BucketCount = HowMany(MaxThingsPerArena - 1, 2) + 2;

 private:
  using Bucket = SinglyLinkedList<Arena>;

  const size_t thingsPerArena_;
  Bucket buckets[BucketCount];

#ifdef DEBUG
  AllocKind allocKind_;
  bool isConvertedToArenaList = false;
#endif

 public:
  inline explicit SortedArenaList(AllocKind allocKind);

  size_t thingsPerArena() const { return thingsPerArena_; }

  // Inserts an arena, which has room for |nfree| more things, in its bucket.
  inline void insertAt(Arena* arena, size_t nfree);

  inline bool hasEmptyArenas() const;

  // Remove any empty arenas and prepend them to the list pointed to by
  // |destListHeadPtr|.
  inline void extractEmptyTo(Arena** destListHeadPtr);

  // Converts the contents of this data structure to a single list, by linking
  // up the tail of each non-empty bucket to the head of the next non-empty
  // bucket.
  //
  // Optionally saves internal state to |maybeBucketLastOut| so that it can be
  // restored later by calling restoreFromArenaList. It is not valid to use this
  // class in the meantime.
  inline ArenaList convertToArenaList(
      Arena* maybeBucketLastOut[BucketCount] = nullptr);

  // Restore the internal state of this class following conversion to an
  // ArenaList by the previous method.
  inline void restoreFromArenaList(ArenaList& list,
                                   Arena* bucketLast[BucketCount]);

#ifdef DEBUG
  AllocKind allocKind() const { return allocKind_; }
#endif

 private:
  inline size_t index(size_t nfree, bool* frontOut) const;
  inline size_t emptyIndex() const;
  inline size_t bucketsUsed() const;

  inline void check() const;
};

// Gather together any swept arenas for the given zone and alloc kind.
class MOZ_RAII AutoGatherSweptArenas {
  SortedArenaList* sortedList = nullptr;

  // Internal state from SortedArenaList so we can restore it later.
  Arena* bucketLastPointers[SortedArenaList::BucketCount];

  // Single result list.
  ArenaList linked;

 public:
  AutoGatherSweptArenas(JS::Zone* zone, AllocKind kind);
  ~AutoGatherSweptArenas();

  ArenaList& sweptArenas() { return linked; }
};

enum class ShouldCheckThresholds {
  DontCheckThresholds = 0,
  CheckThresholds = 1
};

// For each arena kind its free list is represented as the first span with free
// things. Initially all the spans are initialized as empty. After we find a new
// arena with available things we move its first free span into the list and set
// the arena as fully allocated. That way we do not need to update the arena
// after the initial allocation. When starting the GC we only move the head of
// the of the list of spans back to the arena only for the arena that was not
// fully allocated.
class FreeLists {
  AllAllocKindArray<FreeSpan*> freeLists_;

 public:
  // Because the JITs can allocate from the free lists, they cannot be null.
  // We use a placeholder FreeSpan that is empty (and wihout an associated
  // Arena) so the JITs can fall back gracefully.
  static FreeSpan emptySentinel;

  FreeLists();

#ifdef DEBUG
  inline bool allEmpty() const;
  inline bool isEmpty(AllocKind kind) const;
#endif

  inline void clear();

  MOZ_ALWAYS_INLINE TenuredCell* allocate(AllocKind kind);

  inline void* setArenaAndAllocate(Arena* arena, AllocKind kind);

  inline void unmarkPreMarkedFreeCells(AllocKind kind);

  FreeSpan** addressOfFreeList(AllocKind thingKind) {
    return &freeLists_[thingKind];
  }
};

class ArenaLists {
  enum class ConcurrentUse : uint32_t {
    None,
    BackgroundFinalize,
    BackgroundFinalizeFinished
  };

  using ConcurrentUseState = mozilla::Atomic<ConcurrentUse, mozilla::Relaxed>;

  JS::Zone* zone_;

  // Whether this structure can be accessed by other threads.
  UnprotectedData<AllAllocKindArray<ConcurrentUseState>> concurrentUseState_;

  MainThreadData<FreeLists> freeLists_;

  /* The main list of arenas for each alloc kind. */
  MainThreadOrGCTaskData<AllAllocKindArray<ArenaList>> arenaLists_;

  /*
   * Arenas which are currently being collected. The collector can move arenas
   * from arenaLists_ here and back again at various points in collection.
   */
  MainThreadOrGCTaskData<AllAllocKindArray<ArenaList>> collectingArenaLists_;

  // Arena lists which have yet to be swept, but need additional foreground
  // processing before they are swept.
  MainThreadData<Arena*> gcCompactPropMapArenasToUpdate;
  MainThreadData<Arena*> gcNormalPropMapArenasToUpdate;

  // The list of empty arenas which are collected during the sweep phase and
  // released at the end of sweeping every sweep group.
  MainThreadOrGCTaskData<Arena*> savedEmptyArenas;

 public:
  explicit ArenaLists(JS::Zone* zone);
  ~ArenaLists();

  FreeLists& freeLists() { return freeLists_.ref(); }
  const FreeLists& freeLists() const { return freeLists_.ref(); }

  FreeSpan** addressOfFreeList(AllocKind thingKind) {
    return freeLists_.refNoCheck().addressOfFreeList(thingKind);
  }

  inline Arena* getFirstArena(AllocKind thingKind) const;
  inline Arena* getFirstCollectingArena(AllocKind thingKind) const;

  inline bool arenaListsAreEmpty() const;

  inline bool doneBackgroundFinalize(AllocKind kind) const;

  /* Clear the free lists so we won't try to allocate from swept arenas. */
  inline void clearFreeLists();

  inline void unmarkPreMarkedFreeCells();

  MOZ_ALWAYS_INLINE TenuredCell* allocateFromFreeList(AllocKind thingKind);

  inline void checkEmptyFreeLists();
  inline void checkEmptyArenaLists();
  inline void checkEmptyFreeList(AllocKind kind);

  void checkEmptyArenaList(AllocKind kind);

  bool relocateArenas(Arena*& relocatedListOut, JS::GCReason reason,
                      JS::SliceBudget& sliceBudget, gcstats::Statistics& stats);

  void queueForegroundObjectsForSweep(JS::GCContext* gcx);
  void queueForegroundThingsForSweep();

  bool foregroundFinalize(JS::GCContext* gcx, AllocKind thingKind,
                          JS::SliceBudget& sliceBudget,
                          SortedArenaList& sweepList);
  template <ReleaseEmpty releaseEmpty>
  void backgroundFinalize(JS::GCContext* gcx, AllocKind kind,
                          Arena** empty = nullptr);

  Arena* takeSweptEmptyArenas();

  void mergeBackgroundSweptArenas();
  void maybeMergeSweptArenas(AllocKind thingKind);
  void mergeSweptArenas(AllocKind thingKind, ArenaList& sweptArenas);

  void moveArenasToCollectingLists();
  void mergeArenasFromCollectingLists();

  void checkGCStateNotInUse();
  void checkSweepStateNotInUse();
  void checkNoArenasToUpdate();
  void checkNoArenasToUpdateForKind(AllocKind kind);

 private:
  ArenaList& arenaList(AllocKind i) { return arenaLists_.ref()[i]; }
  const ArenaList& arenaList(AllocKind i) const { return arenaLists_.ref()[i]; }

  ArenaList& collectingArenaList(AllocKind i) {
    return collectingArenaLists_.ref()[i];
  }
  const ArenaList& collectingArenaList(AllocKind i) const {
    return collectingArenaLists_.ref()[i];
  }

  ConcurrentUseState& concurrentUse(AllocKind i) {
    return concurrentUseState_.ref()[i];
  }
  ConcurrentUse concurrentUse(AllocKind i) const {
    return concurrentUseState_.ref()[i];
  }

  inline JSRuntime* runtime();
  inline JSRuntime* runtimeFromAnyThread();

  void initBackgroundSweep(AllocKind thingKind);

  void* refillFreeListAndAllocate(AllocKind thingKind,
                                  ShouldCheckThresholds checkThresholds,
                                  StallAndRetry stallAndRetry);

  friend class ArenaIter;
  friend class ArenaIterInGC;
  friend class BackgroundUnmarkTask;
  friend class GCRuntime;
  friend class js::Nursery;
  friend class TenuringTracer;
};

} /* namespace gc */
} /* namespace js */

#endif /* gc_ArenaList_h */
