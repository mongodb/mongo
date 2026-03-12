/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// GC-internal header file for the buffer allocator.

#ifndef gc_BufferAllocator_h
#define gc_BufferAllocator_h

#include "mozilla/Array.h"
#include "mozilla/Atomics.h"
#include "mozilla/BitSet.h"
#include "mozilla/HashTable.h"
#include "mozilla/Maybe.h"
#include "mozilla/TimeStamp.h"

#include <cstdint>
#include <stddef.h>
#include <utility>

#include "jstypes.h"  // JS_PUBLIC_API

#include "ds/SlimLinkedList.h"
#include "js/HeapAPI.h"
#include "threading/LockGuard.h"
#include "threading/Mutex.h"
#include "threading/ProtectedData.h"

class JS_PUBLIC_API JSTracer;

namespace JS {
class JS_PUBLIC_API Zone;
}  // namespace JS

namespace js {

class GCMarker;
class Nursery;

namespace gc {

enum class AllocKind : uint8_t;

struct BufferChunk;
struct Cell;
class GCRuntime;
struct LargeBuffer;

// BufferAllocator allocates dynamically sized blocks of memory which can be
// reclaimed by the garbage collector and are associated with GC things.
//
// Although these blocks can be reclaimed by GC, explicit free and resize is
// also supported. This is important for buffers that can grow or shrink.
//
// The allocator uses a different strategy depending on the size of the
// allocation requested. There are three size ranges, divided as follows:
//
//   Size:            Kind:   Allocator implementation:
//    16 B  - 128 B   Small   Uses the cell (arena) allocator
//   256 B  - 512 KB  Medium  Uses a free list allocator
//     1 MB -         Large   Uses the OS page allocator (e.g. mmap)
//
// The smallest supported allocation size is 16 bytes. This will be used for a
// dynamic slots allocation with zero slots to set a unique ID on a native
// object. This will be the size of two Values, i.e. 16 bytes.
//
// Supported operations
// --------------------
//
//  - Allocate a buffer. Buffers are always owned by a GC cell, and the
//    allocator tracks whether the owner is in the nursery or the tenured heap.
//
//  - Trace an edge to buffer. When the owning cell is traced it must trace the
//    edge to the buffer. This will mark the buffer in a GC and prevent it from
//    being swept.
//
//  - Free a buffer. This allows uniquely owned buffers to be freed and reused
//    without waiting for the next GC. It is a hint only and is not supported
//    for all buffer kinds.
//
//  - Reallocate a buffer. This allows uniquely owned buffers to be resized,
//    possibly in-place. This is important for performance on some benchmarks.
//
// Integration with the rest of the GC
// -----------------------------------
//
// The GC calls the allocator at several points during minor and major GC (at
// the start, when sweeping starts and at the end). Allocations are swept on a
// background thread and the memory used by unmarked allocations is reclaimed.
//
// The allocator tries hard to avoid locking, and where it is necessary tries to
// minimize the time spent holding locks. A lock is required to allocate a chunk
// but after that no locks are required to allocate on the main thread, even
// when off-thread sweeping is taking place. This is achieved by moving data to
// be swept to separate containers from those used for allocation on the main
// thread. Synchronization is required to merge data about swept allocations at
// the end of sweeping.
//
// Small allocations
// -----------------
//
// These are implemented by the cell allocator and are allocated in arenas (slab
// allocation).
//
// Note: currently we do not allocate these in the nursery because nursery-owned
// buffers are allocated directly in the nursery without going through this
// allocation API.
//
// Medium allocations
// ------------------
//
// These are allocated in their own zone-specific chunks using a segregated free
// list strategy. This is the main part of the allocator.
//
// The requested allocation size is used to pick a size class, which is used to
// index into an array giving a list of free regions of suitable size. The first
// region in the list is used and its start address updated; it may also be
// moved to a different list if it is now empty or too small to satisfy further
// allocations for this size class. A bitmap in the chunk header is used to
// track the start offsets of allocations.
//
// Sweeping works by processing a list of chunks, scanning each one for
// allocated but unmarked buffers and rebuilding the free region data for that
// chunk. Sweeping happens separately for minor and major GC and only
// nursery-owned or tenured-owned buffers are swept at one time. This means that
// chunks containing nursery-owned allocations are swept twice during a major
// GC, once to sweep nursery-owned allocations and once for tenured-owned
// allocations. This is required because the sweeping happens at different
// times.
//
// Chunks containing nursery-owned buffers are stored in a separate list to
// chunks that only contain tenured-owned buffers to reduce the number of chunks
// that need to be swept for minor GC. They are stored in |mediumMixedChunks|
// and are moved to |mediumMixedChunksToSweep| at the start of minor GC. They
// are then swept on a background thread and are placed in
// |sweptMediumMixedChunks| if they are not freed. From there they can merged
// back into one of the main thread lists (since they may no longer contain
// nursery-owned buffers).
//
// Chunks containing tenured-owned buffers are stored in |mediumTenuredChunks|
// and are moved to |mediumTenuredChunksToSweep| at the start of major GC. They
// are unavailable for allocation after this point and will be swept on a
// background thread and placed in |sweptMediumTenuredChunks| if they are not
// freed. From there they will be merged back into |mediumTenuredChunks|. This
// means that allocation during an incremental GC will allocate a new
// chunk.
//
// Merging swept data requires taking a lock and so only happens when
// necessary. This happens when a new chunk is needed or at various points
// during GC. During sweeping no additional synchronization is required for
// allocation.
//
// Since major GC requires doing a minor GC at the start and we don't want to
// have to wait for minor GC sweeping to finish there is an optimization where
// chunks containing nursery-owned buffers swept as part of minor GC at the
// start of a major GC are moved directly from |sweptMediumMixedChunks| to
// |mediumTenuredChunksToSweep| at the end of minor GC sweeping. This is
// controlled by the |majorStartedWhileMinorSweeping| flag.
//
// Similarly, if a major GC finishes while minor GC is sweeping then rather than
// waiting for it to finish we set the |majorFinishedWhileMinorSweeping| flag so
// that we clear the |allocatedDuringCollection| for these chunks the end of the
// minor sweeping.
//
// Free works by extending neighboring free regions to cover the freed
// allocation or adding a new one if necessary. Free regions are found by
// checking the allocated bitmap. Free is not supported if the containing chunk
// is currently being swept off-thread.
//
// Reallocation works by resizing in place if possible. It is always possible to
// shrink a medium allocation in place if the target size is still
// medium. In-place growth is possible if there is enough free space following
// the allocation. This is not supported if the containing chunk is currently
// being swept off-thread.  If not possible, a new allocation is made and the
// contents of the buffer copied.
//
// Large allocations
// -----------------
//
// These are implemented using the OS page allocator. Allocations of this size
// are relatively rare and not much attempt is made to optimize them. They are
// chunk aligned which allows them to be distinguished from the other allocation
// kinds by checking the low bits the pointer.
//
// Naming conventions
// ------------------
//
// The following conventions are used in the code:
//  - alloc:  client pointer to allocated memory
//  - buffer: pointer to per-allocation metadata
//

class BufferAllocator : public SlimLinkedListElement<BufferAllocator> {
 public:
  static constexpr size_t MinMediumAllocShift = 8;   // 256 B
  static constexpr size_t MaxMediumAllocShift = 19;  // 512 KB

  static constexpr size_t MediumAllocClasses =
      MaxMediumAllocShift - MinMediumAllocShift + 1;

  // An RAII guard to lock and unlock the buffer allocator lock.
  class AutoLock : public LockGuard<Mutex> {
   public:
    explicit AutoLock(GCRuntime* gc);
    explicit AutoLock(BufferAllocator* allocator);
    friend class UnlockGuard<AutoLock>;
  };

  // A lock guard that is locked only when needed.
  using MaybeLock = mozilla::Maybe<AutoLock>;

 private:
  using BufferChunkList = SlimLinkedList<BufferChunk>;

  struct FreeRegion;
  using FreeList = SlimLinkedList<FreeRegion>;

  // Segregated free list: an array of free lists, one per size class.
  class FreeLists {
    using FreeListArray = mozilla::Array<FreeList, MediumAllocClasses>;
    FreeListArray lists;
    mozilla::BitSet<MediumAllocClasses, uint32_t> available;

   public:
    FreeLists() = default;

    FreeLists(FreeLists&& other);
    FreeLists& operator=(FreeLists&& other);

    using const_iterator = FreeListArray::const_iterator;
    auto begin() const { return lists.begin(); }
    auto end() const { return lists.end(); }

    // Returns SIZE_MAX if none available.
    size_t getFirstAvailableSizeClass(size_t minSizeClass) const;

    FreeRegion* getFirstRegion(size_t sizeClass);

    void pushFront(size_t sizeClass, FreeRegion* region);
    void pushBack(size_t sizeClass, FreeRegion* region);

    void append(FreeLists&& other);
    void prepend(FreeLists&& other);

    void remove(size_t sizeClass, FreeRegion* region);

    void clear();

    template <typename Pred>
    void eraseIf(Pred&& pred);

    void assertEmpty() const;
    void assertContains(size_t sizeClass, FreeRegion* region) const;
    void checkAvailable() const;
  };

  using LargeAllocList = SlimLinkedList<LargeBuffer>;

  using LargeAllocMap =
      mozilla::HashMap<void*, LargeBuffer*, PointerHasher<void*>>;

  enum class State : uint8_t { NotCollecting, Marking, Sweeping };

  enum class OwnerKind : uint8_t { Tenured = 0, Nursery, None };

  // The zone this allocator is associated with.
  MainThreadOrGCTaskData<JS::Zone*> zone;

  // Chunks that contain medium buffers. May contain both nursery-owned and
  // tenured-owned buffers.
  MainThreadData<BufferChunkList> mediumMixedChunks;

  // Chunks that contain only tenured-owned medium sized buffers.
  MainThreadData<BufferChunkList> mediumTenuredChunks;

  // Free lists for medium sized buffers. Used for allocation.
  MainThreadData<FreeLists> mediumFreeLists;

  // Chunks that may contain nursery-owned buffers waiting to be swept during a
  // minor GC. Populated from |mediumMixedChunks|.
  MainThreadOrGCTaskData<BufferChunkList> mediumMixedChunksToSweep;

  // Chunks that contain only tenured-owned buffers waiting to be swept during a
  // major GC. Populated from |mediumTenuredChunks|.
  MainThreadOrGCTaskData<BufferChunkList> mediumTenuredChunksToSweep;

  // Chunks that have been swept and their associated free lists.
  MutexData<BufferChunkList> sweptMediumMixedChunks;
  MutexData<BufferChunkList> sweptMediumTenuredChunks;
  MutexData<FreeLists> sweptMediumNurseryFreeLists;
  MutexData<FreeLists> sweptMediumTenuredFreeLists;

  // List of large nursery-owned buffers.
  MainThreadData<LargeAllocList> largeNurseryAllocs;

  // List of large tenured-owned buffers.
  MainThreadData<LargeAllocList> largeTenuredAllocs;

  // Map from allocation pointer to buffer metadata for large buffers.
  // Access requires holding the mutex during sweeping.
  MainThreadOrGCTaskData<LargeAllocMap> largeAllocMap;

  // Large buffers waiting to be swept.
  MainThreadOrGCTaskData<LargeAllocList> largeNurseryAllocsToSweep;
  MainThreadOrGCTaskData<LargeAllocList> largeTenuredAllocsToSweep;

  // Large buffers that have been swept.
  MutexData<LargeAllocList> sweptLargeTenuredAllocs;

  // Flag to indicate that data from minor sweeping is available to be
  // merged. This includes chunks in the sweptMediumMixedChunks or
  // sweptMediumTenuredChunks lists and the minorSweepingFinished flag.
  mozilla::Atomic<bool, mozilla::Relaxed> hasMinorSweepDataToMerge;

  // GC state for minor and major GC.
  MainThreadData<State> minorState;
  MainThreadData<State> majorState;

  // Flags to tell the main thread that sweeping has finished and the state
  // should be updated.
  MutexData<bool> minorSweepingFinished;
  MutexData<bool> majorSweepingFinished;

  // A major GC was started while a minor GC was still sweeping. Chunks by the
  // minor GC will be moved directly to the list of chunks to sweep for the
  // major GC. This happens for the minor GC at the start of every major GC.
  MainThreadData<bool> majorStartedWhileMinorSweeping;

  // A major GC finished while a minor GC was still sweeping. Some post major GC
  // cleanup will be deferred to the end of the minor sweeping.
  MainThreadData<bool> majorFinishedWhileMinorSweeping;

#ifdef DEBUG
  MainThreadData<bool> movingGCInProgress;
#endif

 public:
  explicit BufferAllocator(JS::Zone* zone);
  ~BufferAllocator();

  static inline size_t GetGoodAllocSize(size_t requiredBytes);
  static inline size_t GetGoodElementCount(size_t requiredElements,
                                           size_t elementSize);
  static inline size_t GetGoodPower2AllocSize(size_t requiredBytes);
  static inline size_t GetGoodPower2ElementCount(size_t requiredElements,
                                                 size_t elementSize);
  static bool IsBufferAlloc(void* alloc);

  void* alloc(size_t bytes, bool nurseryOwned);
  void* allocInGC(size_t bytes, bool nurseryOwned);
  void* realloc(void* alloc, size_t bytes, bool nurseryOwned);
  void free(void* alloc);
  size_t getAllocSize(void* alloc);
  bool isNurseryOwned(void* alloc);

  void startMinorCollection(MaybeLock& lock);
  bool startMinorSweeping();
  void sweepForMinorCollection();

  void startMajorCollection(MaybeLock& lock);
  void startMajorSweeping(MaybeLock& lock);
  void sweepForMajorCollection(bool shouldDecommit);
  void finishMajorCollection(const AutoLock& lock);
  void prepareForMovingGC();
  void fixupAfterMovingGC();
  void clearMarkStateAfterBarrierVerification();

  bool isEmpty() const;

  void traceEdge(JSTracer* trc, Cell* owner, void** bufferp, const char* name);
  bool markTenuredAlloc(void* alloc);
  bool isMarkedBlack(void* alloc);

  // For debugging, used to implement GetMarkInfo. Returns false for allocations
  // being swept on another thread.
  bool isPointerWithinMediumOrLargeBuffer(void* ptr);

  Mutex& lock() const;

  size_t getSizeOfNurseryBuffers();

  void addSizeOfExcludingThis(size_t* usedBytesOut, size_t* freeBytesOut,
                              size_t* adminBytesOut);

  static void printStatsHeader(FILE* file);
  static void printStats(GCRuntime* gc, mozilla::TimeStamp creationTime,
                         bool isMajorGC, FILE* file);
  void getStats(size_t& usedBytes, size_t& freeBytes, size_t& adminBytes,
                size_t& mediumNurseryChunkCount,
                size_t& mediumTenuredChunkCount, size_t& freeRegions,
                size_t& largeNurseryAllocCount, size_t& largeTenuredAllocCount);

#ifdef DEBUG
  void checkGCStateNotInUse();
  void checkGCStateNotInUse(MaybeLock& lock);
  void checkGCStateNotInUse(const AutoLock& lock);
#endif

 private:
  void markNurseryOwnedAlloc(void* alloc, bool ownerWasTenured);
  friend class js::Nursery;

  void maybeMergeSweptData();
  void maybeMergeSweptData(MaybeLock& lock);
  void mergeSweptData();
  void mergeSweptData(const AutoLock& lock);
  void abortMajorSweeping(const AutoLock& lock);
  void clearAllocatedDuringCollectionState(const AutoLock& lock);

  // Small allocation methods:

  static inline bool IsSmallAllocSize(size_t bytes);
  static bool IsSmallAlloc(void* alloc);

  static SmallBuffer* GetSmallBuffer(void* alloc);
  friend struct LargeBuffer;

  void* allocSmall(size_t bytes, bool nurseryOwned);
  void* allocSmallInGC(size_t bytes, bool nurseryOwned);
  void traceSmallAlloc(JSTracer* trc, Cell* owner, void** allocp,
                       const char* name);
  void markSmallNurseryOwnedBuffer(SmallBuffer* buffer, bool ownerWasTenured);
  bool markSmallTenuredAlloc(void* alloc);

  static AllocKind AllocKindForSmallAlloc(size_t bytes);

  // Medium allocation methods:

  static bool IsMediumAlloc(void* alloc);

  void* allocMedium(size_t bytes, bool nurseryOwned, bool inGC);
  void* bumpAllocOrRetry(size_t sizeClass, bool inGC);
  void* bumpAlloc(size_t sizeClass);
  void* allocFromRegion(FreeRegion* region, size_t requestedBytes,
                        size_t sizeClass);
  void updateFreeListsAfterAlloc(FreeLists* freeLists, FreeRegion* region,
                                 size_t sizeClass);
  void recommitRegion(FreeRegion* region);
  bool allocNewChunk(bool inGC);
  bool sweepChunk(BufferChunk* chunk, OwnerKind ownerKindToSweep,
                  bool shouldDecommit, FreeLists& freeLists);
  void addSweptRegion(BufferChunk* chunk, uintptr_t freeStart,
                      uintptr_t freeEnd, bool shouldDecommit,
                      bool expectUnchanged, FreeLists& freeLists);
  void freeMedium(void* alloc);
  bool growMedium(void* alloc, size_t newBytes);
  bool shrinkMedium(void* alloc, size_t newBytes);
  FreeRegion* findFollowingFreeRegion(uintptr_t start);
  FreeRegion* findPrecedingFreeRegion(uintptr_t start);
  enum class ListPosition { Front, Back };
  FreeRegion* addFreeRegion(FreeLists* freeLists, size_t sizeClass,
                            uintptr_t start, uintptr_t end, bool anyDecommitted,
                            ListPosition position,
                            bool expectUnchanged = false);
  void updateFreeRegionStart(FreeLists* freeLists, FreeRegion* region,
                             uintptr_t newStart);
  FreeLists* getChunkFreeLists(BufferChunk* chunk);
  bool isSweepingChunk(BufferChunk* chunk);
  void traceMediumAlloc(JSTracer* trc, Cell* owner, void** allocp,
                        const char* name);
  bool isMediumBufferNurseryOwned(void* alloc) const;
  void markMediumNurseryOwnedBuffer(void* alloc, bool ownerWasTenured);
  bool markMediumTenuredAlloc(void* alloc);

  // Get the size class for an allocation. This rounds up to a class that is
  // large enough to hold the required size.
  static size_t SizeClassForAlloc(size_t bytes);

  // Get the maximum size class of allocations that can use a free region. This
  // rounds down to the largest class that can fit in this region.
  static size_t SizeClassForFreeRegion(size_t bytes);

  static size_t SizeClassBytes(size_t sizeClass);
  friend struct BufferChunk;

  // Large allocation methods:

  static inline bool IsLargeAllocSize(size_t bytes);
  static bool IsLargeAlloc(void* alloc);

  void* allocLarge(size_t bytes, bool nurseryOwned, bool inGC);
  bool sweepLargeTenured(LargeBuffer* buffer);
  void freeLarge(void* alloc);
  bool shrinkLarge(LargeBuffer* buffer, size_t newBytes);
  void unmapLarge(LargeBuffer* buffer, bool isSweeping, MaybeLock& lock);
  void traceLargeAlloc(JSTracer* trc, Cell* owner, void** allocp,
                       const char* name);
  void markLargeNurseryOwnedBuffer(LargeBuffer* buffer, bool ownerWasTenured);
  bool markLargeTenuredBuffer(LargeBuffer* buffer);
  bool isLargeAllocMarked(void* alloc);

  // Lookup a large buffer by pointer in the map.
  LargeBuffer* lookupLargeBuffer(void* alloc);
  LargeBuffer* lookupLargeBuffer(void* alloc, MaybeLock& lock);
  bool needLockToAccessBufferMap() const;

  void updateHeapSize(size_t bytes, bool checkThresholds,
                      bool updateRetainedSize);

#ifdef DEBUG
  void checkChunkListGCStateNotInUse(BufferChunkList& chunks,
                                     bool hasNurseryOwnedAllocs,
                                     bool allowAllocatedDuringCollection);
  void checkChunkGCStateNotInUse(BufferChunk* chunk,
                                 bool allowAllocatedDuringCollection);
  void checkAllocListGCStateNotInUse(LargeAllocList& list, bool isNurseryOwned);
  void verifyChunk(BufferChunk* chunk, bool hasNurseryOwnedAllocs);
  void verifyFreeRegion(BufferChunk* chunk, uintptr_t endOffset,
                        size_t expectedSize);
#endif
};

// Internal data structures defined here so that users can get their size.

#ifdef DEBUG
// Magic check values used debug builds.
static constexpr uint32_t LargeBufferCheckValue = 0xBFA110C2;
static constexpr uint32_t FreeRegionCheckValue = 0xBFA110C3;
#endif

}  // namespace gc
}  // namespace js

#endif  // gc_BufferAllocator_h
