/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "gc/BufferAllocator-inl.h"

#include "mozilla/PodOperations.h"
#include "mozilla/ScopeExit.h"

#ifdef XP_DARWIN
#  include <mach/mach_init.h>
#  include <mach/vm_map.h>
#endif

#include "gc/GCInternals.h"
#include "gc/GCLock.h"
#include "gc/PublicIterators.h"
#include "gc/Zone.h"
#include "js/HeapAPI.h"
#include "util/Poison.h"

#include "gc/Heap-inl.h"
#include "gc/Marking-inl.h"

using namespace js;
using namespace js::gc;

namespace js::gc {

struct alignas(CellAlignBytes) LargeBuffer
    : public SlimLinkedListElement<LargeBuffer> {
  void* alloc;
  size_t bytes;
  bool isNurseryOwned;
  bool allocatedDuringCollection = false;

#ifdef DEBUG
  uint32_t checkValue = LargeBufferCheckValue;
#endif

  LargeBuffer(void* alloc, size_t bytes, bool nurseryOwned)
      : alloc(alloc), bytes(bytes), isNurseryOwned(nurseryOwned) {
    MOZ_ASSERT((bytes % ChunkSize) == 0);
  }

  inline void check() const;

  inline SmallBuffer* headerCell();

#ifdef DEBUG
  inline Zone* zone();
  inline Zone* zoneFromAnyThread();
#endif

  void* data() { return alloc; }
  size_t allocBytes() const { return bytes; }
  bool isPointerWithinAllocation(void* ptr) const;
};

bool SmallBuffer::isNurseryOwned() const {
  return header_.get() & NURSERY_OWNED_BIT;
}

void SmallBuffer::setNurseryOwned(bool value) {
  header_.set(value ? NURSERY_OWNED_BIT : 0);
}

size_t SmallBuffer::allocBytes() const {
  return arena()->getThingSize() - sizeof(SmallBuffer);
}

inline void LargeBuffer::check() const {
  MOZ_ASSERT(checkValue == LargeBufferCheckValue);
}

BufferAllocator::AutoLock::AutoLock(GCRuntime* gc)
    : LockGuard(gc->bufferAllocatorLock) {}

BufferAllocator::AutoLock::AutoLock(BufferAllocator* allocator)
    : LockGuard(allocator->lock()) {}

// Describes a free region in a buffer chunk. This structure is stored at the
// end of the region.
//
// Medium allocations are made in FreeRegions in increasing address order. The
// final allocation will contain the now empty and unused FreeRegion structure.
// FreeRegions are stored in buckets based on their size in FreeLists. Each
// bucket is a linked list of FreeRegions.
struct BufferAllocator::FreeRegion
    : public SlimLinkedListElement<BufferAllocator::FreeRegion> {
  uintptr_t startAddr;
  bool hasDecommittedPages;

#ifdef DEBUG
  uint32_t checkValue = FreeRegionCheckValue;
#endif

  explicit FreeRegion(uintptr_t startAddr, bool decommitted = false)
      : startAddr(startAddr), hasDecommittedPages(decommitted) {}

  static FreeRegion* fromEndOffset(BufferChunk* chunk, uintptr_t endOffset) {
    MOZ_ASSERT(endOffset <= ChunkSize);
    return fromEndAddr(uintptr_t(chunk) + endOffset);
  }
  static FreeRegion* fromEndAddr(uintptr_t endAddr) {
    MOZ_ASSERT((endAddr % MinMediumAllocSize) == 0);
    auto* region = reinterpret_cast<FreeRegion*>(endAddr - sizeof(FreeRegion));
    region->check();
    return region;
  }

  void check() const { MOZ_ASSERT(checkValue == FreeRegionCheckValue); }

  uintptr_t getEnd() const { return uintptr_t(this + 1); }
  size_t size() const { return getEnd() - startAddr; }
};

// A chunk containing medium buffer allocations for a single zone. Unlike
// ArenaChunk, allocations from different zones do not share chunks.
struct BufferChunk : public ChunkBase,
                     public SlimLinkedListElement<BufferChunk> {
#ifdef DEBUG
  MainThreadData<Zone*> zone;
#endif

  MainThreadOrGCTaskData<bool> allocatedDuringCollection;
  MainThreadData<bool> hasNurseryOwnedAllocs;
  MainThreadOrGCTaskData<bool> hasNurseryOwnedAllocsAfterSweep;

  static constexpr size_t MaxAllocsPerChunk = ChunkSize / MinMediumAllocSize;
  using PerAllocByteArray = uint8_t[MaxAllocsPerChunk];
  PerAllocByteArray sizeClassArray;

  // Mark bitmap: one bit minimum per allocation, no gray bits.
  static constexpr size_t BytesPerMarkBit = MinMediumAllocSize;
  using BufferMarkBitmap = MarkBitmap<BytesPerMarkBit, 0>;
  MainThreadOrGCTaskData<BufferMarkBitmap> markBits;

  using PerAllocBitmap = mozilla::BitSet<MaxAllocsPerChunk>;
  using AtomicPerAllocBitmap =
      mozilla::BitSet<MaxAllocsPerChunk,
                      mozilla::Atomic<size_t, mozilla::Relaxed>>;

  MainThreadOrGCTaskData<PerAllocBitmap> allocBitmap;
  MainThreadOrGCTaskData<AtomicPerAllocBitmap> nurseryOwnedBitmap;

  static constexpr size_t PagesPerChunk = ChunkSize / PageSize;
  using PerPageBitmap = mozilla::BitSet<PagesPerChunk, uint32_t>;
  MainThreadOrGCTaskData<PerPageBitmap> decommittedPages;

  static BufferChunk* from(void* alloc) {
    ChunkBase* chunk = js::gc::detail::GetGCAddressChunkBase(alloc);
    MOZ_ASSERT(chunk->kind == ChunkKind::MediumBuffers);
    return static_cast<BufferChunk*>(chunk);
  }

  explicit BufferChunk(Zone* zone);
  ~BufferChunk();

  void checkAlloc(const void* alloc) const;

  const void* ptrFromOffset(uintptr_t offset) const;

  void setAllocated(void* alloc, bool allocated);
  bool isAllocated(const void* alloc) const;
  bool isAllocated(uintptr_t offset) const;

  void setNurseryOwned(void* alloc, bool nurseryOwned);
  bool isNurseryOwned(const void* alloc) const;

  void setSizeClass(void* alloc, size_t sizeClass);
  size_t sizeClass(const void* alloc) const;
  size_t allocBytes(const void* alloc) const;

  // Find next/previous allocations from |offset|. Return ChunkSize on failure.
  size_t findNextAllocated(uintptr_t offset) const;
  size_t findPrevAllocated(uintptr_t offset) const;

  bool isPointerWithinAllocation(void* ptr) const;

 private:
  uintptr_t ptrToOffset(const void* alloc) const;
};

constexpr size_t FirstMediumAllocOffset =
    RoundUp(sizeof(BufferChunk), MinMediumAllocSize);

// Iterate allocations in a BufferChunk.
class BufferChunkIter {
  BufferChunk* chunk;
  size_t offset = FirstMediumAllocOffset;
  size_t size = 0;

 public:
  explicit BufferChunkIter(BufferChunk* chunk) : chunk(chunk) { settle(); }
  bool done() const { return offset == ChunkSize; }
  void next() {
    MOZ_ASSERT(!done());
    offset += size;
    MOZ_ASSERT(offset <= ChunkSize);
    if (!done()) {
      settle();
    }
  }
  size_t getOffset() const {
    MOZ_ASSERT(!done());
    return offset;
  }
  void* get() const {
    MOZ_ASSERT(!done());
    MOZ_ASSERT(offset < ChunkSize);
    MOZ_ASSERT((offset % MinMediumAllocSize) == 0);
    return reinterpret_cast<void*>(uintptr_t(chunk) + offset);
  }
  operator void*() { return get(); }

 private:
  void settle() {
    offset = chunk->findNextAllocated(offset);
    if (!done()) {
      size = chunk->allocBytes(get());
    }
  }
};

static void CheckHighBitsOfPointer(void* ptr) {
#ifdef JS_64BIT
  // We require bit 48 and higher be clear.
  MOZ_DIAGNOSTIC_ASSERT((uintptr_t(ptr) >> 47) == 0);
#endif
}

BufferAllocator::FreeLists::FreeLists(FreeLists&& other) {
  MOZ_ASSERT(this != &other);
  assertEmpty();
  std::swap(lists, other.lists);
  std::swap(available, other.available);
  other.assertEmpty();
}

BufferAllocator::FreeLists& BufferAllocator::FreeLists::operator=(
    FreeLists&& other) {
  MOZ_ASSERT(this != &other);
  assertEmpty();
  std::swap(lists, other.lists);
  std::swap(available, other.available);
  other.assertEmpty();
  return *this;
}

size_t BufferAllocator::FreeLists::getFirstAvailableSizeClass(
    size_t minSizeClass) const {
  size_t result = available.FindNext(minSizeClass);
  MOZ_ASSERT(result >= minSizeClass);
  MOZ_ASSERT_IF(result != SIZE_MAX, !lists[result].isEmpty());
  return result;
}

BufferAllocator::FreeRegion* BufferAllocator::FreeLists::getFirstRegion(
    size_t sizeClass) {
  MOZ_ASSERT(!lists[sizeClass].isEmpty());
  return lists[sizeClass].getFirst();
}

void BufferAllocator::FreeLists::pushFront(size_t sizeClass,
                                           FreeRegion* region) {
  MOZ_ASSERT(sizeClass < MediumAllocClasses);
  lists[sizeClass].pushFront(region);
  available[sizeClass] = true;
}

void BufferAllocator::FreeLists::pushBack(size_t sizeClass,
                                          FreeRegion* region) {
  MOZ_ASSERT(sizeClass < MediumAllocClasses);
  lists[sizeClass].pushBack(region);
  available[sizeClass] = true;
}

void BufferAllocator::FreeLists::append(FreeLists&& other) {
  for (size_t i = 0; i < MediumAllocClasses; i++) {
    if (!other.lists[i].isEmpty()) {
      lists[i].append(std::move(other.lists[i]));
      available[i] = true;
    }
  }
  other.available.ResetAll();
  other.assertEmpty();
}

void BufferAllocator::FreeLists::prepend(FreeLists&& other) {
  for (size_t i = 0; i < MediumAllocClasses; i++) {
    if (!other.lists[i].isEmpty()) {
      lists[i].prepend(std::move(other.lists[i]));
      available[i] = true;
    }
  }
  other.available.ResetAll();
  other.assertEmpty();
}

void BufferAllocator::FreeLists::remove(size_t sizeClass, FreeRegion* region) {
  MOZ_ASSERT(sizeClass < MediumAllocClasses);
  lists[sizeClass].remove(region);
  available[sizeClass] = !lists[sizeClass].isEmpty();
}

void BufferAllocator::FreeLists::clear() {
  for (auto& freeList : lists) {
    new (&freeList) FreeList;  // clear() is less efficient.
  }
  available.ResetAll();
}

template <typename Pred>
void BufferAllocator::FreeLists::eraseIf(Pred&& pred) {
  for (size_t i = 0; i < MediumAllocClasses; i++) {
    FreeList& freeList = lists[i];
    FreeRegion* region = freeList.getFirst();
    while (region) {
      FreeRegion* next = region->getNext();
      if (pred(region)) {
        freeList.remove(region);
      }
      region = next;
    }
    available[i] = !freeList.isEmpty();
  }
}

inline void BufferAllocator::FreeLists::assertEmpty() const {
#ifdef DEBUG
  for (size_t i = 0; i < MediumAllocClasses; i++) {
    MOZ_ASSERT(lists[i].isEmpty());
  }
  MOZ_ASSERT(available.IsEmpty());
#endif
}

inline void BufferAllocator::FreeLists::assertContains(
    size_t sizeClass, FreeRegion* region) const {
#ifdef DEBUG
  MOZ_ASSERT(available[sizeClass]);
  MOZ_ASSERT(lists[sizeClass].contains(region));
#endif
}

inline void BufferAllocator::FreeLists::checkAvailable() const {
#ifdef DEBUG
  for (size_t i = 0; i < MediumAllocClasses; i++) {
    MOZ_ASSERT(available[i] == !lists[i].isEmpty());
  }
#endif
}

}  // namespace js::gc

MOZ_ALWAYS_INLINE void PoisonAlloc(void* alloc, uint8_t value, size_t bytes,
                                   MemCheckKind kind) {
#ifndef EARLY_BETA_OR_EARLIER
  // Limit poisoning in release builds.
  bytes = std::min(bytes, size_t(256));
#endif
  AlwaysPoison(alloc, value, bytes, kind);
}

BufferChunk::BufferChunk(Zone* zone)
    : ChunkBase(zone->runtimeFromMainThread(), ChunkKind::MediumBuffers) {
  mozilla::PodArrayZero(sizeClassArray);
#ifdef DEBUG
  this->zone = zone;
  MOZ_ASSERT(allocBitmap.ref().IsEmpty());
  MOZ_ASSERT(nurseryOwnedBitmap.ref().IsEmpty());
  MOZ_ASSERT(decommittedPages.ref().IsEmpty());
#endif
}

BufferChunk::~BufferChunk() {
#ifdef DEBUG
  MOZ_ASSERT(allocBitmap.ref().IsEmpty());
  MOZ_ASSERT(nurseryOwnedBitmap.ref().IsEmpty());
  for (const auto& sizeClass : sizeClassArray) {
    MOZ_ASSERT(sizeClass == 0);
  }
#endif
}

uintptr_t BufferChunk::ptrToOffset(const void* alloc) const {
  checkAlloc(alloc);

  uintptr_t offset = uintptr_t(alloc) & ChunkMask;
  MOZ_ASSERT(offset >= FirstMediumAllocOffset);

  return offset;
}

const void* BufferChunk::ptrFromOffset(uintptr_t offset) const {
  void* alloc = reinterpret_cast<void*>(uintptr_t(this) + offset);
  checkAlloc(alloc);
  return alloc;
}

void BufferChunk::checkAlloc(const void* alloc) const {
  MOZ_ASSERT((uintptr_t(alloc) & ~ChunkMask) == uintptr_t(this));
  MOZ_ASSERT((uintptr_t(alloc) % MinMediumAllocSize) == 0);
}

void BufferChunk::setAllocated(void* alloc, bool allocated) {
  uintptr_t offset = ptrToOffset(alloc);
  size_t bit = offset / MinMediumAllocSize;
  MOZ_ASSERT(allocBitmap.ref()[bit] != allocated);
  allocBitmap.ref()[bit] = allocated;
}

bool BufferChunk::isAllocated(const void* alloc) const {
  return isAllocated(ptrToOffset(alloc));
}

bool BufferChunk::isAllocated(uintptr_t offset) const {
  MOZ_ASSERT(offset >= FirstMediumAllocOffset);
  MOZ_ASSERT(offset < ChunkSize);

  size_t bit = offset / MinMediumAllocSize;
  return allocBitmap.ref()[bit];
}

size_t BufferChunk::findNextAllocated(uintptr_t offset) const {
  MOZ_ASSERT(offset >= FirstMediumAllocOffset);
  MOZ_ASSERT(offset < ChunkSize);

  size_t bit = offset / MinMediumAllocSize;
  size_t next = allocBitmap.ref().FindNext(bit);
  if (next == SIZE_MAX) {
    return ChunkSize;
  }

  return next * MinMediumAllocSize;
}

size_t BufferChunk::findPrevAllocated(uintptr_t offset) const {
  MOZ_ASSERT(offset >= FirstMediumAllocOffset);
  MOZ_ASSERT(offset < ChunkSize);

  size_t bit = offset / MinMediumAllocSize;
  size_t prev = allocBitmap.ref().FindPrev(bit);
  if (prev == SIZE_MAX) {
    return ChunkSize;
  }

  return prev * MinMediumAllocSize;
}

void BufferChunk::setNurseryOwned(void* alloc, bool nurseryOwned) {
  MOZ_ASSERT(isAllocated(alloc));
  uintptr_t offset = ptrToOffset(alloc);
  size_t bit = offset / MinMediumAllocSize;
  nurseryOwnedBitmap.ref()[bit] = nurseryOwned;
}

bool BufferChunk::isNurseryOwned(const void* alloc) const {
  MOZ_ASSERT(isAllocated(alloc));
  uintptr_t offset = ptrToOffset(alloc);
  size_t bit = offset / MinMediumAllocSize;
  return nurseryOwnedBitmap.ref()[bit];
}

void BufferChunk::setSizeClass(void* alloc, size_t sizeClass) {
  MOZ_ASSERT(isAllocated(alloc));
  uintptr_t offset = ptrToOffset(alloc);
  size_t index = offset / MinMediumAllocSize;
  MOZ_ASSERT(index < std::size(sizeClassArray));
  sizeClassArray[index] = sizeClass;
}

size_t BufferChunk::sizeClass(const void* alloc) const {
  MOZ_ASSERT(isAllocated(alloc));
  uintptr_t offset = ptrToOffset(alloc);
  size_t index = offset / MinMediumAllocSize;
  MOZ_ASSERT(index < std::size(sizeClassArray));
  return sizeClassArray[index];
}

size_t BufferChunk::allocBytes(const void* alloc) const {
  return BufferAllocator::SizeClassBytes(sizeClass(alloc));
}

BufferAllocator::BufferAllocator(Zone* zone)
    : zone(zone),
      sweptMediumMixedChunks(lock()),
      sweptMediumTenuredChunks(lock()),
      sweptMediumNurseryFreeLists(lock()),
      sweptMediumTenuredFreeLists(lock()),
      sweptLargeTenuredAllocs(lock()),
      minorState(State::NotCollecting),
      majorState(State::NotCollecting),
      minorSweepingFinished(lock()),
      majorSweepingFinished(lock()) {}

BufferAllocator::~BufferAllocator() {
#ifdef DEBUG
  checkGCStateNotInUse();
  MOZ_ASSERT(mediumMixedChunks.ref().isEmpty());
  MOZ_ASSERT(mediumTenuredChunks.ref().isEmpty());
  mediumFreeLists.ref().assertEmpty();
  MOZ_ASSERT(largeNurseryAllocs.ref().isEmpty());
  MOZ_ASSERT(largeTenuredAllocs.ref().isEmpty());
#endif
}

bool BufferAllocator::isEmpty() const {
  MOZ_ASSERT(!zone->wasGCStarted() || zone->isGCFinished());
  MOZ_ASSERT(minorState == State::NotCollecting);
  MOZ_ASSERT(majorState == State::NotCollecting);
  return mediumMixedChunks.ref().isEmpty() &&
         mediumTenuredChunks.ref().isEmpty() &&
         largeNurseryAllocs.ref().isEmpty() &&
         largeTenuredAllocs.ref().isEmpty();
}

Mutex& BufferAllocator::lock() const {
  return zone->runtimeFromAnyThread()->gc.bufferAllocatorLock;
}

void* BufferAllocator::alloc(size_t bytes, bool nurseryOwned) {
  MOZ_ASSERT_IF(zone->isGCMarkingOrSweeping(), majorState == State::Marking);

  if (IsLargeAllocSize(bytes)) {
    return allocLarge(bytes, nurseryOwned, false);
  }

  if (IsSmallAllocSize(bytes)) {
    return allocSmall(bytes, nurseryOwned);
  }

  return allocMedium(bytes, nurseryOwned, false);
}

void* BufferAllocator::allocInGC(size_t bytes, bool nurseryOwned) {
  // Currently this is used during tenuring only.
  MOZ_ASSERT(minorState == State::Marking);

  MOZ_ASSERT_IF(zone->isGCMarkingOrSweeping(), majorState == State::Marking);

  void* result;
  if (IsLargeAllocSize(bytes)) {
    result = allocLarge(bytes, nurseryOwned, true);
  } else if (IsSmallAllocSize(bytes)) {
    result = allocSmallInGC(bytes, nurseryOwned);
  } else {
    result = allocMedium(bytes, nurseryOwned, true);
  }

  if (!result) {
    return nullptr;
  }

  // Barrier to mark nursery-owned allocations that happen during collection. We
  // don't need to do this for tenured-owned allocations because we don't sweep
  // tenured-owned allocations that happened after the start of a major
  // collection.
  if (nurseryOwned) {
    markNurseryOwnedAlloc(result, false);
  }

  return result;
}

/* static */
SmallBuffer* BufferAllocator::GetSmallBuffer(void* alloc) {
  MOZ_ASSERT(BufferAllocator::IsSmallAlloc(alloc));
  auto* buffer =
      reinterpret_cast<SmallBuffer*>(uintptr_t(alloc) - sizeof(SmallBuffer));
  buffer->check();
  return buffer;
}

inline SmallBuffer* LargeBuffer::headerCell() {
  return BufferAllocator::GetSmallBuffer(this);
}

#ifdef DEBUG

inline Zone* LargeBuffer::zone() { return headerCell()->zone(); }

inline Zone* LargeBuffer::zoneFromAnyThread() {
  return headerCell()->zoneFromAnyThread();
}

#endif

#ifdef XP_DARWIN
static inline void VirtualCopyPages(void* dst, const void* src, size_t bytes) {
  MOZ_ASSERT((uintptr_t(dst) & PageMask) == 0);
  MOZ_ASSERT((uintptr_t(src) & PageMask) == 0);
  MOZ_ASSERT(bytes >= ChunkSize);

  kern_return_t r = vm_copy(mach_task_self(), vm_address_t(src),
                            vm_size_t(bytes), vm_address_t(dst));
  if (r != KERN_SUCCESS) {
    MOZ_CRASH("vm_copy() failed");
  }
}
#endif

void* BufferAllocator::realloc(void* alloc, size_t bytes, bool nurseryOwned) {
  // Reallocate a buffer. This has the same semantics as standard libarary
  // realloc: if |ptr| is null it creates a new allocation, and if it fails it
  // returns |nullptr| and the original |ptr| is still valid.

  if (!alloc) {
    return this->alloc(bytes, nurseryOwned);
  }

  MOZ_ASSERT(isNurseryOwned(alloc) == nurseryOwned);
  MOZ_ASSERT_IF(zone->isGCMarkingOrSweeping(), majorState == State::Marking);

  bytes = GetGoodAllocSize(bytes);

  size_t currentBytes;
  if (IsLargeAlloc(alloc)) {
    LargeBuffer* buffer = lookupLargeBuffer(alloc);
    currentBytes = buffer->allocBytes();

    // We can shrink large allocations (on some platforms).
    if (bytes < buffer->allocBytes() && IsLargeAllocSize(bytes)) {
      if (shrinkLarge(buffer, bytes)) {
        return alloc;
      }
    }
  } else if (IsMediumAlloc(alloc)) {
    BufferChunk* chunk = BufferChunk::from(alloc);
    currentBytes = chunk->allocBytes(alloc);

    // We can grow or shrink medium allocations.
    if (bytes < currentBytes && !IsSmallAllocSize(bytes)) {
      if (shrinkMedium(alloc, bytes)) {
        return alloc;
      }
    }

    if (bytes > currentBytes && !IsLargeAllocSize(bytes)) {
      if (growMedium(alloc, bytes)) {
        return alloc;
      }
    }
  } else {
    SmallBuffer* buffer = GetSmallBuffer(alloc);
    currentBytes = buffer->allocBytes();
  }

  if (bytes == currentBytes) {
    return alloc;
  }

  void* newAlloc = this->alloc(bytes, nurseryOwned);
  if (!newAlloc) {
    return nullptr;
  }

  auto freeGuard = mozilla::MakeScopeExit([&]() { free(alloc); });

  size_t bytesToCopy = std::min(bytes, currentBytes);

#ifdef XP_DARWIN
  if (bytesToCopy >= ChunkSize) {
    MOZ_ASSERT(IsLargeAlloc(alloc));
    MOZ_ASSERT(IsLargeAlloc(newAlloc));
    VirtualCopyPages(newAlloc, alloc, bytesToCopy);
    return newAlloc;
  }
#endif

  memcpy(newAlloc, alloc, bytesToCopy);
  return newAlloc;
}

void BufferAllocator::free(void* alloc) {
  MOZ_ASSERT(alloc);

  if (IsLargeAlloc(alloc)) {
    freeLarge(alloc);
    return;
  }

  if (IsMediumAlloc(alloc)) {
    freeMedium(alloc);
    return;
  }

  // Can't free small allocations.
}

/* static */
bool BufferAllocator::IsBufferAlloc(void* alloc) {
  // Precondition: |alloc| is a pointer to a buffer allocation, a GC thing or a
  // direct nursery allocation returned by Nursery::allocateBuffer.

  if (IsLargeAlloc(alloc)) {
    return true;
  }

  ChunkKind chunkKind = detail::GetGCAddressChunkBase(alloc)->getKind();
  if (chunkKind == ChunkKind::MediumBuffers) {
    return true;
  }

  if (chunkKind == ChunkKind::TenuredArenas) {
    auto* arena = reinterpret_cast<Arena*>(uintptr_t(alloc) & ~ArenaMask);
    return IsBufferAllocKind(arena->getAllocKind());
  }

  return false;
}

size_t BufferAllocator::getAllocSize(void* alloc) {
  if (IsLargeAlloc(alloc)) {
    LargeBuffer* buffer = lookupLargeBuffer(alloc);
    return buffer->allocBytes();
  }

  if (IsSmallAlloc(alloc)) {
    SmallBuffer* cell = GetSmallBuffer(alloc);
    return cell->allocBytes();
  }

  MOZ_ASSERT(IsMediumAlloc(alloc));
  BufferChunk* chunk = BufferChunk::from(alloc);
  return chunk->allocBytes(alloc);
}

bool BufferAllocator::isNurseryOwned(void* alloc) {
  if (IsLargeAlloc(alloc)) {
    LargeBuffer* buffer = lookupLargeBuffer(alloc);
    return buffer->isNurseryOwned;
  }

  if (IsSmallAlloc(alloc)) {
    // This is always false because we currently make such allocations directly
    // in the nursery.
    SmallBuffer* cell = GetSmallBuffer(alloc);
    return cell->isNurseryOwned();
  }

  BufferChunk* chunk = BufferChunk::from(alloc);
  return chunk->isNurseryOwned(alloc);
}

void BufferAllocator::markNurseryOwnedAlloc(void* alloc, bool ownerWasTenured) {
  MOZ_ASSERT(alloc);
  MOZ_ASSERT(isNurseryOwned(alloc));
  MOZ_ASSERT(minorState == State::Marking);

  if (IsLargeAlloc(alloc)) {
    LargeBuffer* buffer = lookupLargeBuffer(alloc);
    MOZ_ASSERT(buffer->zone() == zone);
    markLargeNurseryOwnedBuffer(buffer, ownerWasTenured);
    return;
  }

  if (IsSmallAlloc(alloc)) {
    SmallBuffer* buffer = GetSmallBuffer(alloc);
    markSmallNurseryOwnedBuffer(buffer, ownerWasTenured);
    return;
  }

  MOZ_ASSERT(IsMediumAlloc(alloc));
  markMediumNurseryOwnedBuffer(alloc, ownerWasTenured);
}

void BufferAllocator::markSmallNurseryOwnedBuffer(SmallBuffer* buffer,
                                                  bool ownerWasTenured) {
  MOZ_ASSERT(buffer->zone() == zone);

  if (ownerWasTenured) {
    buffer->setNurseryOwned(false);
  }

  // Heap size is tracked as part of GC heap for small allocations.
}

void BufferAllocator::markMediumNurseryOwnedBuffer(void* alloc,
                                                   bool ownerWasTenured) {
  BufferChunk* chunk = BufferChunk::from(alloc);
  MOZ_ASSERT(chunk->zone == zone);
  MOZ_ASSERT(chunk->hasNurseryOwnedAllocs);
  MOZ_ASSERT(chunk->isAllocated(alloc));

  if (ownerWasTenured) {
    // Change the allocation to a tenured owned one. This prevents sweeping in a
    // minor collection.
    chunk->setNurseryOwned(alloc, false);
    size_t size = chunk->allocBytes(alloc);
    updateHeapSize(size, false, false);
    return;
  }

  chunk->markBits.ref().markIfUnmarked(alloc, MarkColor::Black);
}

void BufferAllocator::markLargeNurseryOwnedBuffer(LargeBuffer* buffer,
                                                  bool ownerWasTenured) {
  largeNurseryAllocsToSweep.ref().remove(buffer);

  if (ownerWasTenured) {
    buffer->isNurseryOwned = false;
    buffer->allocatedDuringCollection = majorState != State::NotCollecting;
    largeTenuredAllocs.ref().pushBack(buffer);
    size_t usableSize = buffer->allocBytes();
    updateHeapSize(usableSize, false, false);
    return;
  }

  largeNurseryAllocs.ref().pushBack(buffer);
}

bool BufferAllocator::isMarkedBlack(void* alloc) {
  if (IsLargeAlloc(alloc)) {
    return isLargeAllocMarked(alloc);
  }

  if (IsSmallAlloc(alloc)) {
    SmallBuffer* cell = GetSmallBuffer(alloc);
    MOZ_ASSERT(!cell->isMarkedGray());
    return cell->isMarkedBlack();
  }

  MOZ_ASSERT(IsMediumAlloc(alloc));
  BufferChunk* chunk = BufferChunk::from(alloc);
  MOZ_ASSERT(chunk->isAllocated(alloc));
  return chunk->markBits.ref().isMarkedBlack(alloc);
}

void BufferAllocator::traceEdge(JSTracer* trc, Cell* owner, void** bufferp,
                                const char* name) {
  // Buffers are conceptually part of the owning cell and are not reported to
  // the tracer.

  // TODO: This should be unified with the rest of the tracing system.

  MOZ_ASSERT(owner);
  MOZ_ASSERT(bufferp);

  void* buffer = *bufferp;
  MOZ_ASSERT(buffer);

  if (!IsLargeAlloc(buffer) &&
      js::gc::detail::GetGCAddressChunkBase(buffer)->isNurseryChunk()) {
    // JSObject slots and elements can be allocated in the nursery and this is
    // handled separately.
    return;
  }

  MOZ_ASSERT(IsBufferAlloc(buffer));

  if (IsLargeAlloc(buffer)) {
    traceLargeAlloc(trc, owner, bufferp, name);
    return;
  }

  if (IsSmallAlloc(buffer)) {
    traceSmallAlloc(trc, owner, bufferp, name);
    return;
  }

  traceMediumAlloc(trc, owner, bufferp, name);
}

void BufferAllocator::traceSmallAlloc(JSTracer* trc, Cell* owner, void** allocp,
                                      const char* name) {
  void* alloc = *allocp;
  SmallBuffer* cell = GetSmallBuffer(alloc);
  TraceManuallyBarrieredEdge(trc, &cell, name);
  if (cell != GetSmallBuffer(alloc)) {
    *allocp = cell->data();
  }
}

void BufferAllocator::traceMediumAlloc(JSTracer* trc, Cell* owner,
                                       void** allocp, const char* name) {
  void* alloc = *allocp;
  BufferChunk* chunk = BufferChunk::from(alloc);

  if (trc->isTenuringTracer()) {
    if (chunk->isNurseryOwned(alloc)) {
      markMediumNurseryOwnedBuffer(alloc, owner->isTenured());
    }
    return;
  }

  if (trc->isMarkingTracer()) {
    if (!chunk->isNurseryOwned(alloc)) {
      markMediumTenuredAlloc(alloc);
    }
    return;
  }
}

void BufferAllocator::traceLargeAlloc(JSTracer* trc, Cell* owner, void** allocp,
                                      const char* name) {
  void* alloc = *allocp;
  LargeBuffer* buffer = lookupLargeBuffer(alloc);

  // Trace small buffer that holds large buffer metadata.
  // For moving GC the table is updated in fixupAfterMovingGC().
  traceSmallAlloc(trc, owner, reinterpret_cast<void**>(&buffer), "LargeBuffer");

  if (trc->isTenuringTracer()) {
    if (isNurseryOwned(alloc)) {
      markLargeNurseryOwnedBuffer(buffer, owner->isTenured());
    }
    return;
  }

  if (trc->isMarkingTracer()) {
    if (!isNurseryOwned(alloc)) {
      markLargeTenuredBuffer(buffer);
    }
    return;
  }
}

bool BufferAllocator::markTenuredAlloc(void* alloc) {
  MOZ_ASSERT(alloc);
  MOZ_ASSERT(!isNurseryOwned(alloc));

  if (IsLargeAlloc(alloc)) {
    LargeBuffer* buffer = lookupLargeBuffer(alloc);
    return markLargeTenuredBuffer(buffer);
  }

  if (IsSmallAlloc(alloc)) {
    return markSmallTenuredAlloc(alloc);
  }

  return markMediumTenuredAlloc(alloc);
}

bool BufferAllocator::markSmallTenuredAlloc(void* alloc) {
  SmallBuffer* cell = GetSmallBuffer(alloc);
  return cell->markIfUnmarkedThreadSafe(MarkColor::Black);
}

bool BufferAllocator::markMediumTenuredAlloc(void* alloc) {
  BufferChunk* chunk = BufferChunk::from(alloc);
  MOZ_ASSERT(chunk->isAllocated(alloc));
  if (chunk->allocatedDuringCollection) {
    // Will not be swept, already counted as marked.
    return false;
  }

  return chunk->markBits.ref().markIfUnmarkedThreadSafe(alloc,
                                                        MarkColor::Black);
}

void BufferAllocator::startMinorCollection(MaybeLock& lock) {
  maybeMergeSweptData(lock);

#ifdef DEBUG
  MOZ_ASSERT(minorState == State::NotCollecting);
  if (majorState == State::NotCollecting) {
    checkGCStateNotInUse(lock);
  }
#endif

  // Large allocations that are marked when tracing the nursery will be moved
  // back to the main list.
  MOZ_ASSERT(largeNurseryAllocsToSweep.ref().isEmpty());
  std::swap(largeNurseryAllocs.ref(), largeNurseryAllocsToSweep.ref());

  minorState = State::Marking;
}

bool BufferAllocator::startMinorSweeping() {
  // Called during minor GC. Operates on the active allocs/chunks lists. The 'to
  // sweep' lists do not contain nursery owned allocations.

#ifdef DEBUG
  MOZ_ASSERT(minorState == State::Marking);
  {
    AutoLock lock(this);
    MOZ_ASSERT(!minorSweepingFinished);
    MOZ_ASSERT(sweptMediumMixedChunks.ref().isEmpty());
  }
  for (LargeBuffer* buffer : largeNurseryAllocs.ref()) {
    MOZ_ASSERT(buffer->isNurseryOwned);
  }
  for (LargeBuffer* buffer : largeNurseryAllocsToSweep.ref()) {
    MOZ_ASSERT(buffer->isNurseryOwned);
  }
#endif

  // Check whether there are any medium chunks containing nursery owned
  // allocations that need to be swept.
  if (mediumMixedChunks.ref().isEmpty() &&
      largeNurseryAllocsToSweep.ref().isEmpty()) {
    // Nothing more to do. Don't transition to sweeping state.
    minorState = State::NotCollecting;
    return false;
  }

  // TODO: There are more efficient ways to remove the free regions in nursery
  // chunks from the free lists, but all require some more bookkeeping. I don't
  // know how much difference such a change would make.
  //
  // Some possibilities are:
  //  - maintain a separate list of free regions in each chunk and use that to
  //    remove those regions in nursery chunks
  //  - have separate free lists for nursery/tenured chunks
  //  - keep free regions at different ends of the free list depending on chunk
  //    kind
  mediumFreeLists.ref().eraseIf([](FreeRegion* region) {
    return BufferChunk::from(region)->hasNurseryOwnedAllocs;
  });

  mediumMixedChunksToSweep.ref() = std::move(mediumMixedChunks.ref());

  minorState = State::Sweeping;

  return true;
}

void BufferAllocator::sweepForMinorCollection() {
  // Called on a background thread.

  MOZ_ASSERT(minorState.refNoCheck() == State::Sweeping);
  {
    AutoLock lock(this);
    MOZ_ASSERT(sweptMediumMixedChunks.ref().isEmpty());
  }

  while (!mediumMixedChunksToSweep.ref().isEmpty()) {
    BufferChunk* chunk = mediumMixedChunksToSweep.ref().popFirst();
    FreeLists sweptFreeLists;
    if (sweepChunk(chunk, OwnerKind::Nursery, false, sweptFreeLists)) {
      {
        AutoLock lock(this);
        sweptMediumMixedChunks.ref().pushBack(chunk);
        if (chunk->hasNurseryOwnedAllocsAfterSweep) {
          sweptMediumNurseryFreeLists.ref().append(std::move(sweptFreeLists));
        } else {
          sweptMediumTenuredFreeLists.ref().append(std::move(sweptFreeLists));
        }
      }

      // Signal to the main thread that swept data is available by setting this
      // relaxed atomic flag.
      hasMinorSweepDataToMerge = true;
    }
  }

  // Bug 1961749: Freeing large buffers can be slow so it might be worth
  // splitting sweeping into two phases so that all zones get their medium
  // buffers swept and made available for allocation before any large buffers
  // are freed.
  // Alternatively this could happen in parallel to sweeping medium buffers.

  while (!largeNurseryAllocsToSweep.ref().isEmpty()) {
    LargeBuffer* buffer = largeNurseryAllocsToSweep.ref().popFirst();
    MaybeLock lock(std::in_place, this);
    unmapLarge(buffer, true, lock);
  }

  // Signal to main thread to update minorState.
  {
    AutoLock lock(this);
    MOZ_ASSERT(!minorSweepingFinished);
    minorSweepingFinished = true;
    hasMinorSweepDataToMerge = true;
  }
}

void BufferAllocator::startMajorCollection(MaybeLock& lock) {
  maybeMergeSweptData(lock);

#ifdef DEBUG
  MOZ_ASSERT(majorState == State::NotCollecting);
  checkGCStateNotInUse(lock);

  // Everything is tenured since we just evicted the nursery, or will be by the
  // time minor sweeping finishes.
  MOZ_ASSERT(mediumMixedChunks.ref().isEmpty());
  MOZ_ASSERT(largeNurseryAllocs.ref().isEmpty());
#endif

  mediumTenuredChunksToSweep.ref() = std::move(mediumTenuredChunks.ref());
  largeTenuredAllocsToSweep.ref() = std::move(largeTenuredAllocs.ref());

  // Clear the active free lists to prevent further allocation in chunks that
  // will be swept.
  mediumFreeLists.ref().clear();

  if (minorState == State::Sweeping) {
    // Ensure swept nursery chunks are moved to the mediumTenuredChunks lists in
    // mergeSweptData.
    majorStartedWhileMinorSweeping = true;
  }

#ifdef DEBUG
  MOZ_ASSERT(mediumTenuredChunks.ref().isEmpty());
  mediumFreeLists.ref().assertEmpty();
  MOZ_ASSERT(largeTenuredAllocs.ref().isEmpty());
#endif

  majorState = State::Marking;
}

void BufferAllocator::startMajorSweeping(MaybeLock& lock) {
  // Called when a zone transitions from marking to sweeping.

#ifdef DEBUG
  MOZ_ASSERT(majorState == State::Marking);
  MOZ_ASSERT(zone->isGCFinished());
  MOZ_ASSERT(!majorSweepingFinished.refNoCheck());
#endif

  maybeMergeSweptData(lock);
  MOZ_ASSERT(!majorStartedWhileMinorSweeping);

  majorState = State::Sweeping;
}

void BufferAllocator::sweepForMajorCollection(bool shouldDecommit) {
  // Called on a background thread.

  MOZ_ASSERT(majorState.refNoCheck() == State::Sweeping);

  while (!mediumTenuredChunksToSweep.ref().isEmpty()) {
    BufferChunk* chunk = mediumTenuredChunksToSweep.ref().popFirst();
    FreeLists sweptFreeLists;
    if (sweepChunk(chunk, OwnerKind::Tenured, shouldDecommit, sweptFreeLists)) {
      {
        AutoLock lock(this);
        sweptMediumTenuredChunks.ref().pushBack(chunk);
        sweptMediumTenuredFreeLists.ref().append(std::move(sweptFreeLists));
      }

      // Signal to the main thread that swept data is available by setting this
      // relaxed atomic flag.
      hasMinorSweepDataToMerge = true;
    }
  }

  // It's tempting to try and optimize this by moving the allocations between
  // lists when they are marked, in the same way as for nursery sweeping. This
  // would require synchronizing the list modification when marking in parallel,
  // so is probably not worth it.
  LargeAllocList sweptList;
  while (!largeTenuredAllocsToSweep.ref().isEmpty()) {
    LargeBuffer* buffer = largeTenuredAllocsToSweep.ref().popFirst();
    if (sweepLargeTenured(buffer)) {
      sweptList.pushBack(buffer);
    }
  }

  // It would be possible to add these to the output list as we sweep but
  // there's currently no advantage to that.
  AutoLock lock(this);
  sweptLargeTenuredAllocs.ref() = std::move(sweptList);

  // Signal to main thread to update majorState.
  MOZ_ASSERT(!majorSweepingFinished);
  majorSweepingFinished = true;
}

static void ClearAllocatedDuringCollection(SlimLinkedList<BufferChunk>& list) {
  for (auto* buffer : list) {
    buffer->allocatedDuringCollection = false;
  }
}
static void ClearAllocatedDuringCollection(SlimLinkedList<LargeBuffer>& list) {
  for (auto* element : list) {
    element->allocatedDuringCollection = false;
  }
}

void BufferAllocator::finishMajorCollection(const AutoLock& lock) {
  // This can be called in any state:
  //
  //  - NotCollecting: after major sweeping has finished and the state has been
  //                   reset to NotCollecting in mergeSweptData.
  //
  //  - Marking:       if collection was aborted and startMajorSweeping was not
  //                   called.
  //
  //  - Sweeping:      if sweeping has finished and mergeSweptData has not been
  //                   called yet.

  MOZ_ASSERT_IF(majorState == State::Sweeping, majorSweepingFinished);

  if (minorState == State::Sweeping || majorState == State::Sweeping) {
    mergeSweptData(lock);
  }

  if (majorState == State::Marking) {
    abortMajorSweeping(lock);
  }

#ifdef DEBUG
  checkGCStateNotInUse(lock);
#endif
}

void BufferAllocator::abortMajorSweeping(const AutoLock& lock) {
  // We have aborted collection without sweeping this zone. Restore or rebuild
  // the original state.

  MOZ_ASSERT(majorState == State::Marking);
  MOZ_ASSERT(sweptMediumTenuredChunks.ref().isEmpty());

  clearAllocatedDuringCollectionState(lock);

  for (BufferChunk* chunk : mediumTenuredChunksToSweep.ref()) {
    chunk->markBits.ref().clear();
  }

  // Rebuild free lists for chunks we didn't end up sweeping.
  for (BufferChunk* chunk : mediumTenuredChunksToSweep.ref()) {
    MOZ_ALWAYS_TRUE(
        sweepChunk(chunk, OwnerKind::None, false, mediumFreeLists.ref()));
  }

  mediumTenuredChunks.ref().prepend(
      std::move(mediumTenuredChunksToSweep.ref()));
  largeTenuredAllocs.ref().prepend(std::move(largeTenuredAllocsToSweep.ref()));

  majorState = State::NotCollecting;
}

void BufferAllocator::clearAllocatedDuringCollectionState(
    const AutoLock& lock) {
  ClearAllocatedDuringCollection(mediumMixedChunks.ref());
  ClearAllocatedDuringCollection(mediumTenuredChunks.ref());
  // This flag is not set for large nursery-owned allocations.
  ClearAllocatedDuringCollection(largeTenuredAllocs.ref());
}

void BufferAllocator::maybeMergeSweptData() {
  if (minorState == State::Sweeping || majorState == State::Sweeping) {
    mergeSweptData();
  }
}

void BufferAllocator::mergeSweptData() {
  AutoLock lock(this);
  mergeSweptData(lock);
}

void BufferAllocator::maybeMergeSweptData(MaybeLock& lock) {
  if (minorState == State::Sweeping || majorState == State::Sweeping) {
    if (lock.isNothing()) {
      lock.emplace(this);
    }
    mergeSweptData(lock.ref());
  }
}

void BufferAllocator::mergeSweptData(const AutoLock& lock) {
  MOZ_ASSERT(minorState == State::Sweeping || majorState == State::Sweeping);

  if (majorSweepingFinished) {
    clearAllocatedDuringCollectionState(lock);

    if (minorState == State::Sweeping) {
      majorFinishedWhileMinorSweeping = true;
    }
  }

  // Merge swept chunks that previously contained nursery owned allocations. If
  // semispace nursery collection is in use then these chunks may contain both
  // nursery and tenured-owned allocations, otherwise all allocations will be
  // tenured-owned.
  while (!sweptMediumMixedChunks.ref().isEmpty()) {
    BufferChunk* chunk = sweptMediumMixedChunks.ref().popLast();
    MOZ_ASSERT(chunk->hasNurseryOwnedAllocs);
    chunk->hasNurseryOwnedAllocs = chunk->hasNurseryOwnedAllocsAfterSweep;

    MOZ_ASSERT_IF(
        majorState == State::NotCollecting && !majorFinishedWhileMinorSweeping,
        !chunk->allocatedDuringCollection);
    if (majorFinishedWhileMinorSweeping) {
      chunk->allocatedDuringCollection = false;
    }

    if (chunk->hasNurseryOwnedAllocs) {
      mediumMixedChunks.ref().pushFront(chunk);
    } else if (majorStartedWhileMinorSweeping) {
      mediumTenuredChunksToSweep.ref().pushFront(chunk);
    } else {
      mediumTenuredChunks.ref().pushFront(chunk);
    }
  }

  // Merge swept chunks that did not contain nursery owned allocations.
#ifdef DEBUG
  for (BufferChunk* chunk : sweptMediumTenuredChunks.ref()) {
    MOZ_ASSERT(!chunk->hasNurseryOwnedAllocs);
    MOZ_ASSERT(!chunk->hasNurseryOwnedAllocsAfterSweep);
    MOZ_ASSERT(!chunk->allocatedDuringCollection);
  }
#endif
  mediumTenuredChunks.ref().prepend(std::move(sweptMediumTenuredChunks.ref()));

  mediumFreeLists.ref().prepend(std::move(sweptMediumNurseryFreeLists.ref()));
  if (!majorStartedWhileMinorSweeping) {
    mediumFreeLists.ref().prepend(std::move(sweptMediumTenuredFreeLists.ref()));
  } else {
    sweptMediumTenuredFreeLists.ref().clear();
  }

  largeTenuredAllocs.ref().prepend(std::move(sweptLargeTenuredAllocs.ref()));

  hasMinorSweepDataToMerge = false;

  if (minorSweepingFinished) {
    MOZ_ASSERT(minorState == State::Sweeping);
    minorState = State::NotCollecting;
    minorSweepingFinished = false;
    majorStartedWhileMinorSweeping = false;
    majorFinishedWhileMinorSweeping = false;

#ifdef DEBUG
    for (BufferChunk* chunk : mediumMixedChunks.ref()) {
      verifyChunk(chunk, true);
    }
    for (BufferChunk* chunk : mediumTenuredChunks.ref()) {
      verifyChunk(chunk, false);
    }
#endif
  }

  if (majorSweepingFinished) {
    MOZ_ASSERT(majorState == State::Sweeping);
    majorState = State::NotCollecting;
    majorSweepingFinished = false;

    MOZ_ASSERT(mediumTenuredChunksToSweep.ref().isEmpty());
  }
}

void BufferAllocator::prepareForMovingGC() {
  maybeMergeSweptData();

  MOZ_ASSERT(majorState == State::NotCollecting);
  MOZ_ASSERT(minorState == State::NotCollecting);
  MOZ_ASSERT(largeNurseryAllocsToSweep.ref().isEmpty());
  MOZ_ASSERT(largeTenuredAllocsToSweep.ref().isEmpty());

#ifdef DEBUG
  MOZ_ASSERT(!movingGCInProgress);
  movingGCInProgress = true;
#endif

  // Remove large buffer metadata objects from their linked lists so we can move
  // them without breaking the links.
  for (auto i = largeAllocMap.ref().iter(); !i.done(); i.next()) {
    LargeBuffer* buffer = i.get().value();
    if (buffer->isNurseryOwned) {
      largeNurseryAllocs.ref().remove(buffer);
    } else {
      largeTenuredAllocs.ref().remove(buffer);
    }
  }

  MOZ_ASSERT(largeNurseryAllocs.ref().isEmpty());
  MOZ_ASSERT(largeTenuredAllocs.ref().isEmpty());
}

void BufferAllocator::fixupAfterMovingGC() {
#ifdef DEBUG
  MOZ_ASSERT(movingGCInProgress);
  movingGCInProgress = false;
#endif

  // Update our pointers to large allocation metadata objects and return them to
  // their lists.
  for (auto i = largeAllocMap.ref().iter(); !i.done(); i.next()) {
    LargeBuffer* buffer = i.get().value();
    SmallBuffer* headerCell = buffer->headerCell();
    if (IsForwarded(headerCell)) {
      headerCell = Forwarded(headerCell);
      buffer = static_cast<LargeBuffer*>(headerCell->data());
      i.get().value() = buffer;
    }

    MOZ_ASSERT(!buffer->isInList());
    if (buffer->isNurseryOwned) {
      largeNurseryAllocs.ref().pushBack(buffer);
    } else {
      largeTenuredAllocs.ref().pushBack(buffer);
    }
  }
}

void BufferAllocator::clearMarkStateAfterBarrierVerification() {
  MOZ_ASSERT(!zone->wasGCStarted());

  maybeMergeSweptData();
  MOZ_ASSERT(minorState == State::NotCollecting);
  MOZ_ASSERT(majorState == State::NotCollecting);

  for (auto* chunks : {&mediumMixedChunks.ref(), &mediumTenuredChunks.ref()}) {
    for (auto* chunk : *chunks) {
      chunk->markBits.ref().clear();
    }
  }
}

bool BufferAllocator::isPointerWithinMediumOrLargeBuffer(void* ptr) {
  maybeMergeSweptData();

  for (const auto* chunks :
       {&mediumMixedChunks.ref(), &mediumTenuredChunks.ref()}) {
    for (auto* chunk : *chunks) {
      if (chunk->isPointerWithinAllocation(ptr)) {
        return true;
      }
    }
  }

  if (majorState == State::Marking) {
    for (auto* chunk : mediumTenuredChunksToSweep.ref()) {
      if (chunk->isPointerWithinAllocation(ptr)) {
        return true;
      }
    }
  }

  // Note we cannot safely access data that is being swept on another thread.

  for (const auto* allocs :
       {&largeNurseryAllocs.ref(), &largeTenuredAllocs.ref()}) {
    for (auto* alloc : *allocs) {
      if (alloc->isPointerWithinAllocation(ptr)) {
        return true;
      }
    }
  }

  return false;
}

bool BufferChunk::isPointerWithinAllocation(void* ptr) const {
  uintptr_t offset = uintptr_t(ptr) - uintptr_t(this);
  if (offset >= ChunkSize || offset < FirstMediumAllocOffset) {
    return false;
  }

  uintptr_t allocOffset = findPrevAllocated(offset);
  MOZ_ASSERT(allocOffset <= ChunkSize);
  if (allocOffset == ChunkSize) {
    return false;
  }

  const void* alloc = ptrFromOffset(allocOffset);
  size_t size = allocBytes(alloc);

  return offset < allocOffset + size;
}

bool LargeBuffer::isPointerWithinAllocation(void* ptr) const {
  return uintptr_t(ptr) - uintptr_t(alloc) < bytes;
}

#ifdef DEBUG

void BufferAllocator::checkGCStateNotInUse() {
  maybeMergeSweptData();
  AutoLock lock(this);  // Some fields are protected by this lock.
  checkGCStateNotInUse(lock);
}

void BufferAllocator::checkGCStateNotInUse(MaybeLock& maybeLock) {
  if (maybeLock.isNothing()) {
    // Some fields are protected by this lock.
    maybeLock.emplace(this);
  }

  checkGCStateNotInUse(maybeLock.ref());
}

void BufferAllocator::checkGCStateNotInUse(const AutoLock& lock) {
  MOZ_ASSERT(majorState == State::NotCollecting);
  bool isNurserySweeping = minorState == State::Sweeping;

  checkChunkListGCStateNotInUse(mediumMixedChunks.ref(), true, false);
  checkChunkListGCStateNotInUse(mediumTenuredChunks.ref(), false, false);

  if (isNurserySweeping) {
    checkChunkListGCStateNotInUse(sweptMediumMixedChunks.ref(), true,
                                  majorFinishedWhileMinorSweeping);
    checkChunkListGCStateNotInUse(sweptMediumTenuredChunks.ref(), false, false);
  } else {
    MOZ_ASSERT(mediumMixedChunksToSweep.ref().isEmpty());
    MOZ_ASSERT(largeNurseryAllocsToSweep.ref().isEmpty());

    MOZ_ASSERT(sweptMediumMixedChunks.ref().isEmpty());
    MOZ_ASSERT(sweptMediumTenuredChunks.ref().isEmpty());
    sweptMediumNurseryFreeLists.ref().assertEmpty();
    sweptMediumTenuredFreeLists.ref().assertEmpty();

    MOZ_ASSERT(!majorStartedWhileMinorSweeping);
    MOZ_ASSERT(!majorFinishedWhileMinorSweeping);
    MOZ_ASSERT(!hasMinorSweepDataToMerge);
    MOZ_ASSERT(!minorSweepingFinished);
    MOZ_ASSERT(!majorSweepingFinished);
  }

  MOZ_ASSERT(mediumTenuredChunksToSweep.ref().isEmpty());

  checkAllocListGCStateNotInUse(largeNurseryAllocs.ref(), true);
  checkAllocListGCStateNotInUse(largeTenuredAllocs.ref(), false);

  MOZ_ASSERT(largeTenuredAllocsToSweep.ref().isEmpty());
  MOZ_ASSERT(sweptLargeTenuredAllocs.ref().isEmpty());

  MOZ_ASSERT(!movingGCInProgress);
}

void BufferAllocator::checkChunkListGCStateNotInUse(
    BufferChunkList& chunks, bool hasNurseryOwnedAllocs,
    bool allowAllocatedDuringCollection) {
  for (BufferChunk* chunk : chunks) {
    checkChunkGCStateNotInUse(chunk, allowAllocatedDuringCollection);
    verifyChunk(chunk, hasNurseryOwnedAllocs);
  }
}

void BufferAllocator::checkChunkGCStateNotInUse(
    BufferChunk* chunk, bool allowAllocatedDuringCollection) {
  MOZ_ASSERT_IF(!allowAllocatedDuringCollection,
                !chunk->allocatedDuringCollection);

  static constexpr size_t StepBytes = MinMediumAllocSize;

  // Check nothing's marked.
  uintptr_t chunkAddr = uintptr_t(chunk);
  auto& markBits = chunk->markBits.ref();
  for (size_t offset = 0; offset < ChunkSize; offset += StepBytes) {
    void* alloc = reinterpret_cast<void*>(chunkAddr + offset);
    MOZ_ASSERT(!markBits.isMarkedBlack(alloc));
  }
}

void BufferAllocator::verifyChunk(BufferChunk* chunk,
                                  bool hasNurseryOwnedAllocs) {
  MOZ_ASSERT(chunk->hasNurseryOwnedAllocs == hasNurseryOwnedAllocs);

  static constexpr size_t StepBytes = MinMediumAllocSize;

  size_t freeOffset = FirstMediumAllocOffset;

  for (BufferChunkIter iter(chunk); !iter.done(); iter.next()) {
    // Check any free region preceding this allocation.
    size_t offset = iter.getOffset();
    MOZ_ASSERT(offset >= FirstMediumAllocOffset);
    if (offset > freeOffset) {
      verifyFreeRegion(chunk, offset, offset - freeOffset);
    }

    // Check this allocation.
    void* alloc = iter.get();
    MOZ_ASSERT_IF(chunk->isNurseryOwned(alloc), hasNurseryOwnedAllocs);
    size_t bytes = SizeClassBytes(chunk->sizeClass(alloc));
    uintptr_t endOffset = offset + bytes;
    MOZ_ASSERT(endOffset <= ChunkSize);
    for (size_t i = offset + StepBytes; i < endOffset; i += StepBytes) {
      MOZ_ASSERT(!chunk->isAllocated(i));
    }

    freeOffset = endOffset;
  }

  // Check any free region following the last allocation.
  if (freeOffset < ChunkSize) {
    verifyFreeRegion(chunk, ChunkSize, ChunkSize - freeOffset);
  }
}

void BufferAllocator::verifyFreeRegion(BufferChunk* chunk, uintptr_t endOffset,
                                       size_t expectedSize) {
  auto* freeRegion = FreeRegion::fromEndOffset(chunk, endOffset);
  MOZ_ASSERT(freeRegion->isInList());
  MOZ_ASSERT(freeRegion->size() == expectedSize);
}

void BufferAllocator::checkAllocListGCStateNotInUse(LargeAllocList& list,
                                                    bool isNurseryOwned) {
  for (LargeBuffer* buffer : list) {
    MOZ_ASSERT(buffer->isNurseryOwned == isNurseryOwned);
    MOZ_ASSERT_IF(!isNurseryOwned, !buffer->allocatedDuringCollection);
  }
}

#endif

void* BufferAllocator::allocSmall(size_t bytes, bool nurseryOwned) {
  AllocKind kind = AllocKindForSmallAlloc(bytes);

  void* ptr = CellAllocator::AllocTenuredCellUnchecked<NoGC>(zone, kind);
  if (!ptr) {
    return nullptr;
  }

  //MONGODB MODIFICATION: Moves placement new call within SmallBuffer to resolve compilation error using GCC.
  auto* cell = SmallBuffer::create(ptr);
  cell->setNurseryOwned(nurseryOwned);
  MOZ_ASSERT(cell->isNurseryOwned() == nurseryOwned);
  void* alloc = cell->data();

  MOZ_ASSERT(IsSmallAlloc(alloc));
  MOZ_ASSERT(getAllocSize(alloc) >= bytes);
  MOZ_ASSERT(getAllocSize(alloc) < 2 * bytes);

  return alloc;
}

/* static */
void* BufferAllocator::allocSmallInGC(size_t bytes, bool nurseryOwned) {
  AllocKind kind = AllocKindForSmallAlloc(bytes);

  void* ptr = AllocateTenuredCellInGC(zone, kind);
  if (!ptr) {
    return nullptr;
  }

  //MONGODB MODIFICATION: Moves placement new call within SmallBuffer to resolve compilation error using GCC.
  auto* cell = SmallBuffer::create(ptr);;
  cell->setNurseryOwned(nurseryOwned);
  void* alloc = cell->data();

  MOZ_ASSERT(IsSmallAlloc(alloc));
  MOZ_ASSERT(getAllocSize(alloc) >= bytes);
  MOZ_ASSERT(getAllocSize(alloc) < 2 * bytes);

  return alloc;
}

/* static */
AllocKind BufferAllocator::AllocKindForSmallAlloc(size_t bytes) {
  bytes = std::max(bytes, MinAllocSize);
  MOZ_ASSERT(bytes <= MaxSmallAllocSize);

  size_t logBytes = mozilla::CeilingLog2(bytes);
  MOZ_ASSERT(bytes <= (size_t(1) << logBytes));

  MOZ_ASSERT(logBytes >= mozilla::CeilingLog2(MinAllocSize));
  size_t kindIndex = logBytes - mozilla::CeilingLog2(MinAllocSize);

  AllocKind kind = AllocKind(size_t(AllocKind::BUFFER_FIRST) + kindIndex);
  MOZ_ASSERT(IsBufferAllocKind(kind));

  return kind;
}

/* static */
bool BufferAllocator::IsSmallAlloc(void* alloc) {
  MOZ_ASSERT(IsBufferAlloc(alloc));

  // Test for large buffers before calling this so we can assume |alloc| is
  // inside a chunk.
  MOZ_ASSERT(!IsLargeAlloc(alloc));

  ChunkBase* chunk = detail::GetGCAddressChunkBase(alloc);
  return chunk->getKind() == ChunkKind::TenuredArenas;
}

void* BufferAllocator::allocMedium(size_t bytes, bool nurseryOwned, bool inGC) {
  MOZ_ASSERT(!IsSmallAllocSize(bytes));
  MOZ_ASSERT(!IsLargeAllocSize(bytes));
  bytes = mozilla::RoundUpPow2(std::max(bytes, MinMediumAllocSize));

  // Get size class from |bytes|.
  size_t sizeClass = SizeClassForAlloc(bytes);
  MOZ_ASSERT(SizeClassBytes(sizeClass) == GetGoodAllocSize(bytes));

  void* alloc = bumpAllocOrRetry(sizeClass, inGC);
  if (!alloc) {
    return nullptr;
  }

  BufferChunk* chunk = BufferChunk::from(alloc);
  chunk->setAllocated(alloc, true);

  MOZ_ASSERT(!chunk->isNurseryOwned(alloc));
  chunk->setNurseryOwned(alloc, nurseryOwned);

  MOZ_ASSERT(chunk->sizeClass(alloc) == 0);
  chunk->setSizeClass(alloc, sizeClass);

  if (nurseryOwned && !chunk->hasNurseryOwnedAllocs) {
    mediumTenuredChunks.ref().remove(chunk);
    chunk->hasNurseryOwnedAllocs = true;
    mediumMixedChunks.ref().pushBack(chunk);
  }

  MOZ_ASSERT(!chunk->markBits.ref().isMarkedBlack(alloc));

  if (!nurseryOwned) {
    bool checkThresholds = !inGC;
    updateHeapSize(bytes, checkThresholds, false);
  }

  chunk->checkAlloc(alloc);
  return alloc;
}

void* BufferAllocator::bumpAllocOrRetry(size_t sizeClass, bool inGC) {
  void* ptr = bumpAlloc(sizeClass);
  if (ptr) {
    return ptr;
  }

  if (hasMinorSweepDataToMerge) {
    // Avoid taking the lock unless we know there is data to merge. This reduces
    // context switches.
    mergeSweptData();
    ptr = bumpAlloc(sizeClass);
    if (ptr) {
      return ptr;
    }
  }

  if (!allocNewChunk(inGC)) {
    return nullptr;
  }

  ptr = bumpAlloc(sizeClass);
  MOZ_ASSERT(ptr);
  return ptr;
}

void* BufferAllocator::bumpAlloc(size_t sizeClass) {
  size_t requestedBytes = SizeClassBytes(sizeClass);

  mediumFreeLists.ref().checkAvailable();

  // Find smallest suitable size class that has free regions.
  sizeClass = mediumFreeLists.ref().getFirstAvailableSizeClass(sizeClass);
  if (sizeClass == SIZE_MAX) {
    return nullptr;
  }

  FreeRegion* region = mediumFreeLists.ref().getFirstRegion(sizeClass);
  void* ptr = allocFromRegion(region, requestedBytes, sizeClass);
  updateFreeListsAfterAlloc(&mediumFreeLists.ref(), region, sizeClass);
  return ptr;
}

void* BufferAllocator::allocFromRegion(FreeRegion* region,
                                       size_t requestedBytes,
                                       size_t sizeClass) {
  uintptr_t start = region->startAddr;
  MOZ_ASSERT(region->getEnd() > start);
  MOZ_ASSERT(region->size() >= SizeClassBytes(sizeClass));
  MOZ_ASSERT((region->size() % MinMediumAllocSize) == 0);

  // Ensure whole region is commited.
  if (region->hasDecommittedPages) {
    recommitRegion(region);
  }

  // Allocate from start of region.
  void* ptr = reinterpret_cast<void*>(start);
  start += requestedBytes;
  MOZ_ASSERT(region->getEnd() >= start);

  // Update region start.
  region->startAddr = start;

  return ptr;
}

void BufferAllocator::updateFreeListsAfterAlloc(FreeLists* freeLists,
                                                FreeRegion* region,
                                                size_t sizeClass) {
  // Updates |freeLists| after an allocation of class |sizeClass| from |region|.

  freeLists->assertContains(sizeClass, region);

  // If the region is still valid for further allocations of this size class
  // then there's nothing to do.
  size_t classBytes = SizeClassBytes(sizeClass);
  size_t newSize = region->size();
  MOZ_ASSERT((newSize % MinMediumAllocSize) == 0);
  if (newSize >= classBytes) {
    return;
  }

  // Remove region from this free list.
  freeLists->remove(sizeClass, region);

  // If the region is now empty then we're done.
  if (newSize == 0) {
    return;
  }

  // Otherwise region is now too small. Move it to the appropriate bucket for
  // its reduced size.
  size_t newSizeClass = SizeClassForFreeRegion(newSize);
  MOZ_ASSERT(newSize >= SizeClassBytes(newSizeClass));
  MOZ_ASSERT(newSizeClass < sizeClass);
  freeLists->pushFront(newSizeClass, region);
}

void BufferAllocator::recommitRegion(FreeRegion* region) {
  MOZ_ASSERT(region->hasDecommittedPages);
  MOZ_ASSERT(DecommitEnabled());

  BufferChunk* chunk = BufferChunk::from(region);
  uintptr_t startAddr = RoundUp(region->startAddr, PageSize);
  uintptr_t endAddr = RoundDown(uintptr_t(region), PageSize);

  size_t startPage = (startAddr - uintptr_t(chunk)) / PageSize;
  size_t endPage = (endAddr - uintptr_t(chunk)) / PageSize;

  // If the start of the region does not lie on a page boundary the page it is
  // in should be committed as it must either contain the start of the chunk, a
  // FreeRegion or an allocation.
  MOZ_ASSERT_IF((region->startAddr % PageSize) != 0,
                !chunk->decommittedPages.ref()[startPage - 1]);

  // The end of the region should be committed as it holds FreeRegion |region|.
  MOZ_ASSERT(!chunk->decommittedPages.ref()[endPage]);

  MarkPagesInUseSoft(reinterpret_cast<void*>(startAddr), endAddr - startAddr);
  for (size_t i = startPage; i != endPage; i++) {
    chunk->decommittedPages.ref()[i] = false;
  }

  region->hasDecommittedPages = false;
}

static inline StallAndRetry ShouldStallAndRetry(bool inGC) {
  return inGC ? StallAndRetry::Yes : StallAndRetry::No;
}

bool BufferAllocator::allocNewChunk(bool inGC) {
  GCRuntime* gc = &zone->runtimeFromMainThread()->gc;
  AutoLockGCBgAlloc lock(gc);
  ArenaChunk* baseChunk = gc->takeOrAllocChunk(ShouldStallAndRetry(inGC), lock);
  if (!baseChunk) {
    return false;
  }

  CheckHighBitsOfPointer(baseChunk);

  // Ensure all memory is initially committed.
  if (!baseChunk->decommittedPages.IsEmpty()) {
    MOZ_ASSERT(DecommitEnabled());
    MarkPagesInUseSoft(baseChunk, ChunkSize);
  }

  // Unpoison past the ChunkBase header.
  void* ptr = reinterpret_cast<void*>(uintptr_t(baseChunk) + sizeof(ChunkBase));
  size_t size = ChunkSize - sizeof(ChunkBase);
  SetMemCheckKind(ptr, size, MemCheckKind::MakeUndefined);

  BufferChunk* chunk = new (baseChunk) BufferChunk(zone);
  chunk->allocatedDuringCollection = majorState != State::NotCollecting;

  mediumTenuredChunks.ref().pushBack(chunk);

  uintptr_t freeStart = uintptr_t(chunk) + FirstMediumAllocOffset;
  uintptr_t freeEnd = uintptr_t(chunk) + ChunkSize;

  size_t sizeClass = SizeClassForFreeRegion(freeEnd - freeStart);

  ptr = reinterpret_cast<void*>(freeEnd - sizeof(FreeRegion));
  FreeRegion* region = new (ptr) FreeRegion(freeStart);
  MOZ_ASSERT(region->getEnd() == freeEnd);
  mediumFreeLists.ref().pushFront(sizeClass, region);

  return true;
}

bool BufferAllocator::sweepChunk(BufferChunk* chunk, OwnerKind ownerKindToSweep,
                                 bool shouldDecommit, FreeLists& freeLists) {
  // Find all regions of free space in |chunk| and add them to the swept free
  // lists.

  // TODO: Ideally we'd arrange things so we allocate from most-full chunks
  // first. This could happen by sweeping all chunks and then sorting them by
  // how much free space they had and then adding their free regions to the free
  // lists in that order.

  GCRuntime* gc = &zone->runtimeFromAnyThread()->gc;

  bool hasNurseryOwnedAllocs = false;

  size_t freeStart = FirstMediumAllocOffset;
  bool sweptAny = false;
  size_t mallocHeapBytesFreed = 0;

  for (BufferChunkIter iter(chunk); !iter.done(); iter.next()) {
    void* alloc = iter.get();

    size_t bytes = chunk->allocBytes(alloc);
    uintptr_t allocEnd = iter.getOffset() + bytes;

    bool nurseryOwned = chunk->isNurseryOwned(alloc);
    OwnerKind ownerKind = OwnerKind(uint8_t(nurseryOwned));
    MOZ_ASSERT_IF(nurseryOwned, ownerKind == OwnerKind::Nursery);
    MOZ_ASSERT_IF(!nurseryOwned, ownerKind == OwnerKind::Tenured);
    bool canSweep = ownerKind == ownerKindToSweep;
    bool shouldSweep = canSweep && !chunk->markBits.ref().isMarkedBlack(alloc);

    if (shouldSweep) {
      // Dead. Update allocated bitmap, metadata and heap size accounting.
      if (!nurseryOwned) {
        mallocHeapBytesFreed += bytes;
      }
      chunk->setNurseryOwned(alloc, false);
      chunk->setSizeClass(alloc, 0);
      chunk->setAllocated(alloc, false);
      PoisonAlloc(alloc, JS_SWEPT_TENURED_PATTERN, bytes,
                  MemCheckKind::MakeUndefined);
      sweptAny = true;
    } else {
      // Alive. Add any free space before this allocation.
      uintptr_t allocStart = iter.getOffset();
      if (freeStart != allocStart) {
        addSweptRegion(chunk, freeStart, allocStart, shouldDecommit, !sweptAny,
                       freeLists);
      }
      freeStart = allocEnd;
      if (canSweep) {
        chunk->markBits.ref().unmarkOneBit(alloc, ColorBit::BlackBit);
      }
      if (nurseryOwned) {
        MOZ_ASSERT(ownerKindToSweep == OwnerKind::Nursery);
        hasNurseryOwnedAllocs = true;
      }
      sweptAny = false;
    }
  }

  if (mallocHeapBytesFreed) {
    zone->mallocHeapSize.removeBytes(mallocHeapBytesFreed, true);
  }

  if (freeStart == FirstMediumAllocOffset &&
      ownerKindToSweep != OwnerKind::None) {
    // Chunk is empty. Give it back to the system.
    bool allMemoryCommitted = chunk->decommittedPages.ref().IsEmpty();
    chunk->~BufferChunk();
    ArenaChunk* tenuredChunk =
        ArenaChunk::emplace(chunk, gc, allMemoryCommitted);
    AutoLockGC lock(gc);
    gc->recycleChunk(tenuredChunk, lock);
    return false;
  }

  // Add any free space from the last allocation to the end of the chunk.
  if (freeStart != ChunkSize) {
    addSweptRegion(chunk, freeStart, ChunkSize, shouldDecommit, !sweptAny,
                   freeLists);
  }

  chunk->hasNurseryOwnedAllocsAfterSweep = hasNurseryOwnedAllocs;

  return true;
}

void BufferAllocator::addSweptRegion(BufferChunk* chunk, uintptr_t freeStart,
                                     uintptr_t freeEnd, bool shouldDecommit,
                                     bool expectUnchanged,
                                     FreeLists& freeLists) {
  // Add the region from |freeStart| to |freeEnd| to the appropriate swept free
  // list based on its size.

  MOZ_ASSERT(freeStart >= FirstMediumAllocOffset);
  MOZ_ASSERT(freeStart < freeEnd);
  MOZ_ASSERT(freeEnd <= ChunkSize);
  MOZ_ASSERT((freeStart % MinMediumAllocSize) == 0);
  MOZ_ASSERT((freeEnd % MinMediumAllocSize) == 0);
  MOZ_ASSERT_IF(shouldDecommit, DecommitEnabled());

  // Decommit pages if |shouldDecommit| was specified, but leave space for
  // the FreeRegion structure at the end.
  bool anyDecommitted = false;
  uintptr_t decommitStart = RoundUp(freeStart, PageSize);
  uintptr_t decommitEnd = RoundDown(freeEnd - sizeof(FreeRegion), PageSize);
  size_t endPage = decommitEnd / PageSize;
  if (shouldDecommit && decommitEnd > decommitStart) {
    void* ptr = reinterpret_cast<void*>(decommitStart + uintptr_t(chunk));
    MarkPagesUnusedSoft(ptr, decommitEnd - decommitStart);
    size_t startPage = decommitStart / PageSize;
    for (size_t i = startPage; i != endPage; i++) {
      chunk->decommittedPages.ref()[i] = true;
    }
    anyDecommitted = true;
  } else {
    // Check for any previously decommitted pages.
    uintptr_t startPage = RoundDown(freeStart, PageSize) / PageSize;
    for (size_t i = startPage; i != endPage; i++) {
      if (chunk->decommittedPages.ref()[i]) {
        anyDecommitted = true;
      }
    }
  }

  // The last page must have previously been either a live allocation or a
  // FreeRegion, so it must already be committed.
  MOZ_ASSERT(!chunk->decommittedPages.ref()[endPage]);

  freeStart += uintptr_t(chunk);
  freeEnd += uintptr_t(chunk);

  size_t sizeClass = SizeClassForFreeRegion(freeEnd - freeStart);
  addFreeRegion(&freeLists, sizeClass, freeStart, freeEnd, anyDecommitted,
                ListPosition::Back, expectUnchanged);
}

void BufferAllocator::freeMedium(void* alloc) {
  // Free a medium sized allocation. This coalesces the free space with any
  // neighboring free regions. Coalescing is necessary for resize to work
  // properly.

  BufferChunk* chunk = BufferChunk::from(alloc);
  MOZ_ASSERT(chunk->zone == zone);

  size_t bytes = chunk->allocBytes(alloc);
  PoisonAlloc(alloc, JS_FREED_BUFFER_PATTERN, bytes,
              MemCheckKind::MakeUndefined);

  if (isSweepingChunk(chunk)) {
    return;  // We can't free if the chunk is currently being swept.
  }

  // Update heap size for tenured owned allocations.
  if (!chunk->isNurseryOwned(alloc)) {
    bool updateRetained =
        majorState == State::Marking && !chunk->allocatedDuringCollection;
    zone->mallocHeapSize.removeBytes(bytes, updateRetained);
  }

  // Update metadata.
  chunk->setNurseryOwned(alloc, false);
  chunk->setSizeClass(alloc, 0);

  // Set region as not allocated and then clear mark bit.
  chunk->setAllocated(alloc, false);

  // TODO: Since the mark bits are atomic, it's probably OK to unmark even if
  // the chunk is currently being swept. If we get lucky the memory will be
  // freed sooner.
  chunk->markBits.ref().unmarkOneBit(alloc, ColorBit::BlackBit);

  FreeLists* freeLists = getChunkFreeLists(chunk);

  uintptr_t startAddr = uintptr_t(alloc);
  uintptr_t endAddr = startAddr + bytes;

  // First check whether there is a free region following the allocation.
  FreeRegion* region;
  uintptr_t endOffset = endAddr & ChunkMask;
  if (endOffset == 0 || chunk->isAllocated(endOffset)) {
    // The allocation abuts the end of the chunk or another allocation. Add the
    // allocation as a new free region.
    //
    // The new region is added to the front of relevant list so as to reuse
    // recently freed memory preferentially. This may reduce fragmentation. See
    // "The Memory Fragmentation Problem: Solved?"  by Johnstone et al.
    size_t sizeClass = SizeClassForFreeRegion(bytes);
    region = addFreeRegion(freeLists, sizeClass, startAddr, endAddr, false,
                           ListPosition::Front);
  } else {
    // There is a free region following this allocation. Expand the existing
    // region down to cover the newly freed space.
    region = findFollowingFreeRegion(endAddr);
    MOZ_ASSERT(region->startAddr == endAddr);
    updateFreeRegionStart(freeLists, region, startAddr);
  }

  // Next check for any preceding free region and coalesce.
  FreeRegion* precRegion = findPrecedingFreeRegion(startAddr);
  if (precRegion) {
    if (freeLists) {
      size_t sizeClass = SizeClassForFreeRegion(precRegion->size());
      freeLists->remove(sizeClass, precRegion);
    }

    updateFreeRegionStart(freeLists, region, precRegion->startAddr);
    if (precRegion->hasDecommittedPages) {
      region->hasDecommittedPages = true;
    }
  }
}

bool BufferAllocator::isSweepingChunk(BufferChunk* chunk) {
  if (minorState == State::Sweeping && chunk->hasNurseryOwnedAllocs) {
    // We are currently sweeping nursery owned allocations.

    // TODO: We could set a flag for nursery chunks allocated during minor
    // collection to allow operations on chunks that are not being swept here.

    if (!hasMinorSweepDataToMerge) {
#ifdef DEBUG
      {
        AutoLock lock(this);
        MOZ_ASSERT_IF(!hasMinorSweepDataToMerge, !minorSweepingFinished);
      }
#endif

      // Likely no data to merge so don't bother taking the lock.
      return true;
    }

    // Merge swept data and recheck.
    //
    // TODO: It would be good to know how often this helps and if it is
    // worthwhile.
    mergeSweptData();
    if (minorState == State::Sweeping && chunk->hasNurseryOwnedAllocs) {
      return true;
    }
  }

  if (majorState == State::Sweeping && !chunk->allocatedDuringCollection) {
    // We are currently sweeping tenured owned allocations.
    return true;
  }

  return false;
}

BufferAllocator::FreeRegion* BufferAllocator::addFreeRegion(
    FreeLists* freeLists, size_t sizeClass, uintptr_t start, uintptr_t end,
    bool anyDecommitted, ListPosition position,
    bool expectUnchanged /* = false */) {
#ifdef DEBUG
  MOZ_ASSERT(end - start >= SizeClassBytes(sizeClass));
  if (expectUnchanged) {
    // We didn't free any allocations so there should already be a FreeRegion
    // from |start| to |end|.
    auto* region = FreeRegion::fromEndAddr(end);
    region->check();
    MOZ_ASSERT(region->startAddr == start);
  }
#endif

  void* ptr = reinterpret_cast<void*>(end - sizeof(FreeRegion));
  FreeRegion* region = new (ptr) FreeRegion(start, anyDecommitted);
  MOZ_ASSERT(region->getEnd() == end);

  if (freeLists) {
    if (position == ListPosition::Front) {
      freeLists->pushFront(sizeClass, region);
    } else {
      freeLists->pushBack(sizeClass, region);
    }
  }

  return region;
}

void BufferAllocator::updateFreeRegionStart(FreeLists* freeLists,
                                            FreeRegion* region,
                                            uintptr_t newStart) {
  MOZ_ASSERT((newStart & ~ChunkMask) == (uintptr_t(region) & ~ChunkMask));
  MOZ_ASSERT(region->startAddr != newStart);

  size_t oldSize = region->size();
  region->startAddr = newStart;

  if (!freeLists) {
    return;
  }

  size_t currentSizeClass = SizeClassForFreeRegion(oldSize);
  size_t newSizeClass = SizeClassForFreeRegion(region->size());
  if (currentSizeClass != newSizeClass) {
    freeLists->remove(currentSizeClass, region);
    freeLists->pushFront(newSizeClass, region);
  }
}

bool BufferAllocator::growMedium(void* alloc, size_t newBytes) {
  MOZ_ASSERT(!IsSmallAllocSize(newBytes));
  MOZ_ASSERT(!IsLargeAllocSize(newBytes));
  newBytes = std::max(newBytes, MinMediumAllocSize);
  MOZ_ASSERT(newBytes == GetGoodAllocSize(newBytes));

  BufferChunk* chunk = BufferChunk::from(alloc);
  MOZ_ASSERT(chunk->zone == zone);

  if (isSweepingChunk(chunk)) {
    return false;  // We can't grow if the chunk is currently being swept.
  }

  size_t currentBytes = SizeClassBytes(chunk->sizeClass(alloc));
  MOZ_ASSERT(newBytes > currentBytes);

  uintptr_t endOffset = (uintptr_t(alloc) & ChunkMask) + currentBytes;
  MOZ_ASSERT(endOffset <= ChunkSize);
  if (endOffset == ChunkSize) {
    return false;  // Can't extend because we're at the end of the chunk.
  }

  size_t endAddr = uintptr_t(chunk) + endOffset;
  if (chunk->isAllocated(endOffset)) {
    return false;  // Can't extend because we abut another allocation.
  }

  FreeRegion* region = findFollowingFreeRegion(endAddr);
  MOZ_ASSERT(region->startAddr == endAddr);

  size_t extraBytes = newBytes - currentBytes;
  if (region->size() < extraBytes) {
    return false;  // Can't extend because following free region is too small.
  }

  size_t sizeClass = SizeClassForFreeRegion(region->size());

  allocFromRegion(region, extraBytes, sizeClass);

  // If the allocation is in a chunk where we've cleared the free lists before
  // sweeping we don't need to update the free lists.
  if (FreeLists* freeLists = getChunkFreeLists(chunk)) {
    updateFreeListsAfterAlloc(freeLists, region, sizeClass);
  }

  chunk->setSizeClass(alloc, SizeClassForAlloc(newBytes));
  if (!chunk->isNurseryOwned(alloc)) {
    bool updateRetained =
        majorState == State::Marking && !chunk->allocatedDuringCollection;
    updateHeapSize(extraBytes, true, updateRetained);
  }

  return true;
}

bool BufferAllocator::shrinkMedium(void* alloc, size_t newBytes) {
  MOZ_ASSERT(!IsSmallAllocSize(newBytes));
  MOZ_ASSERT(!IsLargeAllocSize(newBytes));
  newBytes = std::max(newBytes, MinMediumAllocSize);
  MOZ_ASSERT(newBytes == GetGoodAllocSize(newBytes));

  BufferChunk* chunk = BufferChunk::from(alloc);
  MOZ_ASSERT(chunk->zone == zone);

  if (isSweepingChunk(chunk)) {
    return false;  // We can't shrink if the chunk is currently being swept.
  }

  size_t currentBytes = chunk->allocBytes(alloc);
  if (newBytes == currentBytes) {
    // Requested size is the same after adjusting to a valid medium alloc size.
    return false;
  }

  MOZ_ASSERT(newBytes < currentBytes);
  size_t sizeChange = currentBytes - newBytes;

  // Update allocation size.
  chunk->setSizeClass(alloc, SizeClassForAlloc(newBytes));
  if (!chunk->isNurseryOwned(alloc)) {
    bool updateRetained =
        majorState == State::Marking && !chunk->allocatedDuringCollection;
    zone->mallocHeapSize.removeBytes(sizeChange, updateRetained);
  }

  uintptr_t startOffset = uintptr_t(alloc) & ChunkMask;
  uintptr_t oldEndOffset = startOffset + currentBytes;
  uintptr_t newEndOffset = startOffset + newBytes;
  MOZ_ASSERT(oldEndOffset <= ChunkSize);

  // Poison freed memory.
  uintptr_t chunkAddr = uintptr_t(chunk);
  PoisonAlloc(reinterpret_cast<void*>(chunkAddr + newEndOffset),
              JS_SWEPT_TENURED_PATTERN, sizeChange,
              MemCheckKind::MakeUndefined);

  FreeLists* freeLists = getChunkFreeLists(chunk);

  // If we abut another allocation then add a new free region.
  if (oldEndOffset == ChunkSize || chunk->isAllocated(oldEndOffset)) {
    size_t sizeClass = SizeClassForFreeRegion(sizeChange);
    uintptr_t freeStart = chunkAddr + newEndOffset;
    uintptr_t freeEnd = chunkAddr + oldEndOffset;
    addFreeRegion(freeLists, sizeClass, freeStart, freeEnd, false,
                  ListPosition::Front);
    return true;
  }

  // Otherwise find the following free region and extend it down.
  FreeRegion* region = findFollowingFreeRegion(chunkAddr + oldEndOffset);
  MOZ_ASSERT(region->startAddr == chunkAddr + oldEndOffset);
  updateFreeRegionStart(freeLists, region, chunkAddr + newEndOffset);

  return true;
}

BufferAllocator::FreeLists* BufferAllocator::getChunkFreeLists(
    BufferChunk* chunk) {
  MOZ_ASSERT_IF(majorState == State::Sweeping,
                chunk->allocatedDuringCollection);

  if (majorState == State::Marking && !chunk->allocatedDuringCollection) {
    // The chunk has been queued for sweeping and the free lists cleared.
    return nullptr;
  }

  return &mediumFreeLists.ref();
}

BufferAllocator::FreeRegion* BufferAllocator::findFollowingFreeRegion(
    uintptr_t startAddr) {
  // Find the free region that starts at |startAddr|, which is not allocated and
  // not at the end of the chunk. Always returns a region.

  uintptr_t offset = uintptr_t(startAddr) & ChunkMask;
  MOZ_ASSERT(offset >= FirstMediumAllocOffset);
  MOZ_ASSERT(offset < ChunkSize);
  MOZ_ASSERT((offset % MinMediumAllocSize) == 0);

  BufferChunk* chunk = BufferChunk::from(reinterpret_cast<void*>(startAddr));
  MOZ_ASSERT(!chunk->isAllocated(offset));  // Already marked as not allocated.
  offset = chunk->findNextAllocated(offset);
  MOZ_ASSERT(offset <= ChunkSize);

  auto* region = FreeRegion::fromEndOffset(chunk, offset);
  MOZ_ASSERT(region->startAddr == startAddr);

  return region;
}

BufferAllocator::FreeRegion* BufferAllocator::findPrecedingFreeRegion(
    uintptr_t endAddr) {
  // Find the free region, if any, that ends at |endAddr|, which may be
  // allocated or at the start of the chunk.

  uintptr_t offset = uintptr_t(endAddr) & ChunkMask;
  MOZ_ASSERT(offset >= FirstMediumAllocOffset);
  MOZ_ASSERT(offset < ChunkSize);
  MOZ_ASSERT((offset % MinMediumAllocSize) == 0);

  if (offset == FirstMediumAllocOffset) {
    return nullptr;  // Already at start of chunk.
  }

  BufferChunk* chunk = BufferChunk::from(reinterpret_cast<void*>(endAddr));
  MOZ_ASSERT(!chunk->isAllocated(offset));
  offset = chunk->findPrevAllocated(offset);

  if (offset != ChunkSize) {
    // Found a preceding allocation.
    const void* alloc = chunk->ptrFromOffset(offset);
    size_t bytes = SizeClassBytes(chunk->sizeClass(alloc));
    MOZ_ASSERT(uintptr_t(alloc) + bytes <= endAddr);
    if (uintptr_t(alloc) + bytes == endAddr) {
      // No free space between preceding allocation and |endAddr|.
      return nullptr;
    }
  }

  auto* region = FreeRegion::fromEndAddr(endAddr);

#ifdef DEBUG
  region->check();
  if (offset != ChunkSize) {
    const void* alloc = chunk->ptrFromOffset(offset);
    size_t bytes = chunk->allocBytes(alloc);
    MOZ_ASSERT(region->startAddr == uintptr_t(alloc) + bytes);
  } else {
    MOZ_ASSERT(region->startAddr == uintptr_t(chunk) + FirstMediumAllocOffset);
  }
#endif

  return region;
}

/* static */
size_t BufferAllocator::SizeClassForAlloc(size_t bytes) {
  MOZ_ASSERT(bytes >= MinMediumAllocSize);
  MOZ_ASSERT(bytes <= MaxMediumAllocSize);

  size_t log2Size = mozilla::CeilingLog2(bytes);
  MOZ_ASSERT((size_t(1) << log2Size) >= bytes);
  MOZ_ASSERT(MinMediumAllocShift == mozilla::CeilingLog2(MinMediumAllocSize));
  MOZ_ASSERT(log2Size >= MinMediumAllocShift);
  size_t sizeClass = log2Size - MinMediumAllocShift;
  MOZ_ASSERT(sizeClass < MediumAllocClasses);
  return sizeClass;
}

/* static */
size_t BufferAllocator::SizeClassForFreeRegion(size_t bytes) {
  MOZ_ASSERT(bytes >= MinMediumAllocSize);
  MOZ_ASSERT(bytes < ChunkSize);

  size_t log2Size = mozilla::FloorLog2(bytes);
  MOZ_ASSERT((size_t(1) << log2Size) <= bytes);
  MOZ_ASSERT(log2Size >= MinMediumAllocShift);
  size_t sizeClass =
      std::min(log2Size - MinMediumAllocShift, MediumAllocClasses - 1);
  MOZ_ASSERT(sizeClass < MediumAllocClasses);

  return sizeClass;
}

/* static */
inline size_t BufferAllocator::SizeClassBytes(size_t sizeClass) {
  MOZ_ASSERT(sizeClass < MediumAllocClasses);
  // MONGODB MODIFICATION: Make 64-bit shift explicit for MSVC
  return (size_t)1 << (sizeClass + MinMediumAllocShift);
}

/* static */
bool BufferAllocator::IsMediumAlloc(void* alloc) {
  ChunkBase* chunk = js::gc::detail::GetGCAddressChunkBase(alloc);
  return chunk->getKind() == ChunkKind::MediumBuffers;
}

bool BufferAllocator::needLockToAccessBufferMap() const {
  MOZ_ASSERT(CurrentThreadCanAccessZone(zone) || CurrentThreadIsPerformingGC());
  return minorState.refNoCheck() == State::Sweeping ||
         majorState.refNoCheck() == State::Sweeping;
}

LargeBuffer* BufferAllocator::lookupLargeBuffer(void* alloc) {
  MaybeLock lock;
  return lookupLargeBuffer(alloc, lock);
}

LargeBuffer* BufferAllocator::lookupLargeBuffer(void* alloc, MaybeLock& lock) {
  MOZ_ASSERT(lock.isNothing());
  if (needLockToAccessBufferMap()) {
    lock.emplace(this);
  }

  auto ptr = largeAllocMap.ref().lookup(alloc);
  MOZ_ASSERT(ptr);
  LargeBuffer* buffer = ptr->value();
  MOZ_ASSERT(buffer->data() == alloc);
  MOZ_ASSERT(buffer->zoneFromAnyThread() == zone);
  return buffer;
}

void* BufferAllocator::allocLarge(size_t bytes, bool nurseryOwned, bool inGC) {
  bytes = RoundUp(bytes, ChunkSize);
  MOZ_ASSERT(bytes > MaxMediumAllocSize);
  MOZ_ASSERT(bytes >= bytes);

  // Allocate a small buffer the size of a LargeBuffer to hold the metadata.
  static_assert(sizeof(LargeBuffer) <= MaxSmallAllocSize);
  void* bufferPtr = allocSmall(sizeof(LargeBuffer), nurseryOwned);
  if (!bufferPtr) {
    return nullptr;
  }

  // Large allocations are aligned to the chunk size, even if they are smaller
  // than a chunk. This allows us to tell large buffer allocations apart by
  // looking at the pointer alignment.
  void* alloc = MapAlignedPages(bytes, ChunkSize, ShouldStallAndRetry(inGC));
  if (!alloc) {
    return nullptr;
  }
  auto freeGuard = mozilla::MakeScopeExit([&]() { UnmapPages(alloc, bytes); });

  CheckHighBitsOfPointer(alloc);

  auto* buffer = new (bufferPtr) LargeBuffer(alloc, bytes, nurseryOwned);

  {
    MaybeLock lock;
    if (needLockToAccessBufferMap()) {
      lock.emplace(this);
    }
    if (!largeAllocMap.ref().putNew(alloc, buffer)) {
      return nullptr;
    }
  }

  freeGuard.release();

  if (nurseryOwned) {
    largeNurseryAllocs.ref().pushBack(buffer);
  } else {
    buffer->allocatedDuringCollection = majorState != State::NotCollecting;
    largeTenuredAllocs.ref().pushBack(buffer);
  }

  // Update memory accounting and trigger an incremental slice if needed.
  if (!nurseryOwned) {
    bool checkThresholds = !inGC;
    updateHeapSize(bytes, checkThresholds, false);
  }

  MOZ_ASSERT(IsLargeAlloc(alloc));
  return alloc;
}

void BufferAllocator::updateHeapSize(size_t bytes, bool checkThresholds,
                                     bool updateRetainedSize) {
  // Update memory accounting and trigger an incremental slice if needed.
  // TODO: This will eventually be attributed to gcHeapSize.
  zone->mallocHeapSize.addBytes(bytes, updateRetainedSize);
  if (checkThresholds) {
    GCRuntime* gc = &zone->runtimeFromAnyThread()->gc;
    gc->maybeTriggerGCAfterMalloc(zone);
  }
}

/* static */
bool BufferAllocator::IsLargeAlloc(void* alloc) {
  return (uintptr_t(alloc) & ChunkMask) == 0;
}

bool BufferAllocator::isLargeAllocMarked(void* alloc) {
  LargeBuffer* buffer = lookupLargeBuffer(alloc);
  return buffer->headerCell()->isMarkedAny();
}

bool BufferAllocator::markLargeTenuredBuffer(LargeBuffer* buffer) {
  MOZ_ASSERT(!buffer->isNurseryOwned);

  if (buffer->allocatedDuringCollection) {
    return false;
  }

  // Bug 1961755: This method can return false positives. A fully atomic version
  // would be preferable in this case.
  return buffer->headerCell()->markIfUnmarkedThreadSafe(MarkColor::Black);
}

bool BufferAllocator::sweepLargeTenured(LargeBuffer* buffer) {
  MOZ_ASSERT(!buffer->isNurseryOwned);
  MOZ_ASSERT(buffer->zoneFromAnyThread() == zone);
  MOZ_ASSERT(!buffer->isInList());

  if (buffer->headerCell()->isMarkedAny()) {
    return true;
  }

  MaybeLock lock(std::in_place, this);
  unmapLarge(buffer, true, lock);
  return false;
}

void BufferAllocator::freeLarge(void* alloc) {
  MaybeLock lock;
  LargeBuffer* buffer = lookupLargeBuffer(alloc, lock);
  MOZ_ASSERT(buffer->zone() == zone);

  DebugOnlyPoison(alloc, JS_FREED_BUFFER_PATTERN, buffer->allocBytes(),
                  MemCheckKind::MakeUndefined);

  if (!buffer->isNurseryOwned && majorState == State::Sweeping &&
      !buffer->allocatedDuringCollection) {
    return;  // Large allocations are currently being swept.
  }

  MOZ_ASSERT(buffer->isInList());

  if (buffer->isNurseryOwned) {
    largeNurseryAllocs.ref().remove(buffer);
  } else if (majorState == State::Marking &&
             !buffer->allocatedDuringCollection) {
    largeTenuredAllocsToSweep.ref().remove(buffer);
  } else {
    largeTenuredAllocs.ref().remove(buffer);
  }

  unmapLarge(buffer, false, lock);
}

bool BufferAllocator::shrinkLarge(LargeBuffer* buffer, size_t newBytes) {
  MOZ_ASSERT(IsLargeAllocSize(newBytes));
#ifdef XP_WIN
  // Can't unmap part of a region mapped with VirtualAlloc on Windows.
  //
  // It is possible to decommit the physical pages so we could do that and
  // track virtual size as well as committed size. This would also allow us to
  // grow the allocation again if necessary.
  return false;
#else
  MOZ_ASSERT(buffer->zone() == zone);

  if (!buffer->isNurseryOwned && majorState == State::Sweeping &&
      !buffer->allocatedDuringCollection) {
    return false;  // Large allocations are currently being swept.
  }

  MOZ_ASSERT(buffer->isInList());

  newBytes = RoundUp(newBytes, ChunkSize);
  size_t oldBytes = buffer->bytes;
  MOZ_ASSERT(oldBytes > newBytes);
  size_t shrinkBytes = oldBytes - newBytes;

  if (!buffer->isNurseryOwned) {
    zone->mallocHeapSize.removeBytes(shrinkBytes, false);
  }

  buffer->bytes = newBytes;

  void* endPtr = reinterpret_cast<void*>(uintptr_t(buffer->data()) + newBytes);
  UnmapPages(endPtr, shrinkBytes);

  return true;
#endif
}

void BufferAllocator::unmapLarge(LargeBuffer* buffer, bool isSweeping,
                                 MaybeLock& lock) {
  MOZ_ASSERT(buffer->zoneFromAnyThread() == zone);
  MOZ_ASSERT(!buffer->isInList());
  MOZ_ASSERT_IF(isSweeping || needLockToAccessBufferMap(), lock.isSome());

#ifdef DEBUG
  auto ptr = largeAllocMap.ref().lookup(buffer->data());
  MOZ_ASSERT(ptr && ptr->value() == buffer);
#endif
  largeAllocMap.ref().remove(buffer->data());

  // Drop the lock now we've updated the map.
  lock.reset();

  size_t bytes = buffer->bytes;

  if (!buffer->isNurseryOwned) {
    zone->mallocHeapSize.removeBytes(bytes, isSweeping);
  }

  UnmapPages(buffer->data(), bytes);
}

#include "js/Printer.h"
#include "util/GetPidProvider.h"

static const char* const BufferAllocatorStatsPrefix = "BufAllc:";

#define FOR_EACH_BUFFER_STATS_FIELD(_)                \
  _("PID", 7, "%7zu", pid)                            \
  _("Runtime", 14, "0x%12p", runtime)                 \
  _("Timestamp", 10, "%10.6f", timestamp.ToSeconds()) \
  _("Reason", 20, "%-20.20s", reason)                 \
  _("", 2, "%2s", "")                                 \
  _("TotalKB", 8, "%8zu", totalBytes / 1024)          \
  _("UsedKB", 8, "%8zu", usedBytes / 1024)            \
  _("FreeKB", 8, "%8zu", freeBytes / 1024)            \
  _("Zs", 3, "%3zu", zoneCount)                       \
  _("", 7, "%7s", "")                                 \
  _("MNCs", 6, "%6zu", mediumMixedChunks)             \
  _("MTCs", 6, "%6zu", mediumTenuredChunks)           \
  _("FRs", 6, "%6zu", freeRegions)                    \
  _("LNAs", 6, "%6zu", largeNurseryAllocs)            \
  _("LTAs", 6, "%6zu", largeTenuredAllocs)

/* static */
void BufferAllocator::printStatsHeader(FILE* file) {
  Sprinter sprinter;
  if (!sprinter.init()) {
    return;
  }
  sprinter.put(BufferAllocatorStatsPrefix);

#define PRINT_METADATA_NAME(name, width, _1, _2) \
  sprinter.printf(" %-*s", width, name);

  FOR_EACH_BUFFER_STATS_FIELD(PRINT_METADATA_NAME)
#undef PRINT_METADATA_NAME

  sprinter.put("\n");

  JS::UniqueChars str = sprinter.release();
  if (!str) {
    return;
  }
  fputs(str.get(), file);
}

/* static */
void BufferAllocator::printStats(GCRuntime* gc, mozilla::TimeStamp creationTime,
                                 bool isMajorGC, FILE* file) {
  Sprinter sprinter;
  if (!sprinter.init()) {
    return;
  }
  sprinter.put(BufferAllocatorStatsPrefix);

  size_t pid = getpid();
  JSRuntime* runtime = gc->rt;
  mozilla::TimeDuration timestamp = mozilla::TimeStamp::Now() - creationTime;
  const char* reason = isMajorGC ? "post major slice" : "pre minor GC";

  size_t zoneCount = 0;
  size_t usedBytes = 0;
  size_t freeBytes = 0;
  size_t adminBytes = 0;
  size_t mediumMixedChunks = 0;
  size_t mediumTenuredChunks = 0;
  size_t freeRegions = 0;
  size_t largeNurseryAllocs = 0;
  size_t largeTenuredAllocs = 0;
  for (AllZonesIter zone(gc); !zone.done(); zone.next()) {
    zoneCount++;
    zone->bufferAllocator.getStats(usedBytes, freeBytes, adminBytes,
                                   mediumMixedChunks, mediumTenuredChunks,
                                   freeRegions, largeNurseryAllocs,
                                   largeTenuredAllocs);
  }

  size_t totalBytes = usedBytes + freeBytes + adminBytes;

#define PRINT_FIELD_VALUE(_1, _2, format, value) \
  sprinter.printf(" " format, value);

  FOR_EACH_BUFFER_STATS_FIELD(PRINT_FIELD_VALUE)
#undef PRINT_FIELD_VALUE

  sprinter.put("\n");

  JS::UniqueChars str = sprinter.release();
  if (!str) {
    return;
  }

  fputs(str.get(), file);
}

size_t BufferAllocator::getSizeOfNurseryBuffers() {
  maybeMergeSweptData();

  MOZ_ASSERT(minorState == State::NotCollecting);
  MOZ_ASSERT(majorState == State::NotCollecting);

  size_t bytes = 0;

  for (BufferChunk* chunk : mediumMixedChunks.ref()) {
    for (BufferChunkIter alloc(chunk); !alloc.done(); alloc.next()) {
      if (chunk->isNurseryOwned(alloc)) {
        bytes += chunk->allocBytes(alloc);
      }
    }
  }

  for (const LargeBuffer* buffer : largeNurseryAllocs.ref()) {
    bytes += buffer->allocBytes();
  }

  return bytes;
}

void BufferAllocator::addSizeOfExcludingThis(size_t* usedBytesOut,
                                             size_t* freeBytesOut,
                                             size_t* adminBytesOut) {
  maybeMergeSweptData();

  MOZ_ASSERT(minorState == State::NotCollecting);
  MOZ_ASSERT(majorState == State::NotCollecting);

  size_t usedBytes = 0;
  size_t freeBytes = 0;
  size_t adminBytes = 0;
  size_t mediumMixedChunks = 0;
  size_t mediumTenuredChunks = 0;
  size_t freeRegions = 0;
  size_t largeNurseryAllocs = 0;
  size_t largeTenuredAllocs = 0;
  getStats(usedBytes, freeBytes, adminBytes, mediumMixedChunks,
           mediumTenuredChunks, freeRegions, largeNurseryAllocs,
           largeTenuredAllocs);

  *usedBytesOut += usedBytes;
  *freeBytesOut += freeBytes;
  *adminBytesOut += adminBytes;
}

void BufferAllocator::getStats(size_t& usedBytes, size_t& freeBytes,
                               size_t& adminBytes,
                               size_t& mediumNurseryChunkCount,
                               size_t& mediumTenuredChunkCount,
                               size_t& freeRegions,
                               size_t& largeNurseryAllocCount,
                               size_t& largeTenuredAllocCount) {
  maybeMergeSweptData();

  MOZ_ASSERT(minorState == State::NotCollecting);

  for (const BufferChunk* chunk : mediumMixedChunks.ref()) {
    (void)chunk;
    mediumNurseryChunkCount++;
    usedBytes += ChunkSize - FirstMediumAllocOffset;
    adminBytes += FirstMediumAllocOffset;
  }
  for (const BufferChunk* chunk : mediumTenuredChunks.ref()) {
    (void)chunk;
    mediumTenuredChunkCount++;
    usedBytes += ChunkSize - FirstMediumAllocOffset;
    adminBytes += FirstMediumAllocOffset;
  }
  for (const LargeBuffer* buffer : largeNurseryAllocs.ref()) {
    largeNurseryAllocCount++;
    usedBytes += buffer->allocBytes();
    adminBytes += sizeof(LargeBuffer);
  }
  for (const LargeBuffer* buffer : largeTenuredAllocs.ref()) {
    largeTenuredAllocCount++;
    usedBytes += buffer->allocBytes();
    adminBytes += sizeof(LargeBuffer);
  }
  for (const FreeList& freeList : mediumFreeLists.ref()) {
    for (const FreeRegion* region : freeList) {
      freeRegions++;
      size_t size = region->size();
      MOZ_ASSERT(usedBytes >= size);
      usedBytes -= size;
      freeBytes += size;
    }
  }
}

JS::ubi::Node::Size JS::ubi::Concrete<SmallBuffer>::size(
    mozilla::MallocSizeOf mallocSizeOf) const {
  return get().allocBytes();
}

const char16_t JS::ubi::Concrete<SmallBuffer>::concreteTypeName[] =
    u"SmallBuffer";
