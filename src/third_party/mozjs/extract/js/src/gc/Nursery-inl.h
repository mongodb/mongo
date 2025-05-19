/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=4 sw=2 et tw=80 ft=cpp:
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef gc_Nursery_inl_h
#define gc_Nursery_inl_h

#include "gc/Nursery.h"

#include "gc/GCRuntime.h"
#include "gc/RelocationOverlay.h"
#include "js/TracingAPI.h"
#include "vm/JSContext.h"
#include "vm/NativeObject.h"

namespace js {
namespace gc {
struct Cell;
}  // namespace gc
}  // namespace js

inline JSRuntime* js::Nursery::runtime() const { return gc->rt; }

template <typename T>
bool js::Nursery::isInside(const SharedMem<T>& p) const {
  return isInside(p.unwrap(/*safe - used for value in comparison above*/));
}

inline bool js::Nursery::shouldTenure(gc::Cell* cell) {
  MOZ_ASSERT(semispaceEnabled());
  MOZ_ASSERT(inCollectedRegion(cell));

  size_t offset = fromSpace.offsetFromAddress(uintptr_t(cell));
  MOZ_ASSERT(offset >=
             fromSpace.offsetFromExclusiveAddress(fromSpace.startPosition_));
  return offset <= tenureThreshold_;
}

inline bool js::Nursery::inCollectedRegion(gc::Cell* cell) const {
  return gc::InCollectedNurseryRegion(cell);
}

inline bool js::Nursery::inCollectedRegion(void* ptr) const {
  if (!semispaceEnabled()) {
    return toSpace.isInside(ptr);
  }

  return fromSpace.isInside(ptr);
}

inline size_t js::Nursery::Space::offsetFromExclusiveAddress(
    uintptr_t addr) const {
  if ((addr & gc::ChunkMask) == 0) {
    // |addr| points one past the end of the previous chunk.
    return offsetFromAddress(addr - 1) + 1;
  }

  return offsetFromAddress(addr);
}

inline size_t js::Nursery::Space::offsetFromAddress(uintptr_t addr) const {
  gc::ChunkBase* chunk =
      gc::detail::GetCellChunkBase(reinterpret_cast<gc::Cell*>(addr));
  MOZ_ASSERT(chunk->getKind() == kind);
  MOZ_ASSERT(findChunkIndex(addr & ~gc::ChunkMask) == chunk->nurseryChunkIndex);

  uint32_t offset = addr & gc::ChunkMask;
  MOZ_ASSERT(offset >= sizeof(gc::ChunkBase));
  return (chunk->nurseryChunkIndex << gc::ChunkShift) | offset;
}

MOZ_ALWAYS_INLINE /* static */ bool js::Nursery::getForwardedPointer(
    js::gc::Cell** ref) {
  js::gc::Cell* cell = (*ref);
  MOZ_ASSERT(IsInsideNursery(cell));
  if (!cell->isForwarded()) {
    return false;
  }
  const gc::RelocationOverlay* overlay = gc::RelocationOverlay::fromCell(cell);
  *ref = overlay->forwardingAddress();
  return true;
}

inline void js::Nursery::maybeSetForwardingPointer(JSTracer* trc, void* oldData,
                                                   void* newData, bool direct) {
  if (trc->isTenuringTracer()) {
    setForwardingPointerWhileTenuring(oldData, newData, direct);
  }
}

inline void js::Nursery::setForwardingPointerWhileTenuring(void* oldData,
                                                           void* newData,
                                                           bool direct) {
  if (isInside(oldData)) {
    setForwardingPointer(oldData, newData, direct);
  }
}

inline void js::Nursery::setSlotsForwardingPointer(HeapSlot* oldSlots,
                                                   HeapSlot* newSlots,
                                                   uint32_t nslots) {
  // Slot arrays always have enough space for a forwarding pointer, since the
  // number of slots is never zero.
  MOZ_ASSERT(nslots > 0);
  setDirectForwardingPointer(oldSlots, newSlots);
}

inline void js::Nursery::setElementsForwardingPointer(ObjectElements* oldHeader,
                                                      ObjectElements* newHeader,
                                                      uint32_t capacity) {
  // Only use a direct forwarding pointer if there is enough space for one.
  setForwardingPointer(oldHeader->elements(), newHeader->elements(),
                       capacity > 0);
}

inline void js::Nursery::setForwardingPointer(void* oldData, void* newData,
                                              bool direct) {
  if (direct) {
    setDirectForwardingPointer(oldData, newData);
    return;
  }

  setIndirectForwardingPointer(oldData, newData);
}

inline void js::Nursery::setDirectForwardingPointer(void* oldData,
                                                    void* newData) {
  MOZ_ASSERT(isInside(oldData));
  MOZ_ASSERT_IF(isInside(newData), !inCollectedRegion(newData));

  new (oldData) BufferRelocationOverlay{newData};
}

inline void* js::Nursery::tryAllocateCell(gc::AllocSite* site, size_t size,
                                          JS::TraceKind kind) {
  // Ensure there's enough space to replace the contents with a
  // RelocationOverlay.
  // MOZ_ASSERT(size >= sizeof(RelocationOverlay));
  MOZ_ASSERT(size % gc::CellAlignBytes == 0);
  MOZ_ASSERT(size_t(kind) < gc::NurseryTraceKinds);
  MOZ_ASSERT_IF(kind == JS::TraceKind::String, canAllocateStrings());
  MOZ_ASSERT_IF(kind == JS::TraceKind::BigInt, canAllocateBigInts());

  void* ptr = tryAllocate(sizeof(gc::NurseryCellHeader) + size);
  if (MOZ_UNLIKELY(!ptr)) {
    return nullptr;
  }

  new (ptr) gc::NurseryCellHeader(site, kind);

  void* cell =
      reinterpret_cast<void*>(uintptr_t(ptr) + sizeof(gc::NurseryCellHeader));
  if (!cell) {
    MOZ_MAKE_COMPILER_ASSUME_IS_UNREACHABLE(
        "Successful allocation cannot result in nullptr");
  }

  // Update the allocation site. This code is also inlined in
  // MacroAssembler::updateAllocSite.
  uint32_t allocCount = site->incAllocCount();
  if (allocCount == gc::NormalSiteAttentionThreshold) {
    pretenuringNursery.insertIntoAllocatedList(site);
  }
  MOZ_ASSERT_IF(
      site->isNormal() && allocCount >= gc::NormalSiteAttentionThreshold,
      site->isInAllocatedList());

  gc::gcprobes::NurseryAlloc(cell, kind);
  return cell;
}

inline void* js::Nursery::tryAllocate(size_t size) {
  MOZ_ASSERT(isEnabled());
  MOZ_ASSERT_IF(JS::RuntimeHeapIsBusy(), JS::RuntimeHeapIsMinorCollecting());
  MOZ_ASSERT_IF(currentChunk() == startChunk(), position() >= startPosition());
  MOZ_ASSERT(size % gc::CellAlignBytes == 0);
  MOZ_ASSERT(position() % gc::CellAlignBytes == 0);

  if (MOZ_UNLIKELY(currentEnd() < position() + size)) {
    return nullptr;
  }

  void* ptr = reinterpret_cast<void*>(position());
  if (!ptr) {
    MOZ_MAKE_COMPILER_ASSUME_IS_UNREACHABLE(
        "Successful allocation cannot result in nullptr");
  }

  toSpace.position_ = position() + size;

  DebugOnlyPoison(ptr, JS_ALLOCATED_NURSERY_PATTERN, size,
                  MemCheckKind::MakeUndefined);

  return ptr;
}

inline bool js::Nursery::registerTrailer(PointerAndUint7 blockAndListID,
                                         size_t nBytes) {
  MOZ_ASSERT(toSpace.trailersAdded_.length() ==
             toSpace.trailersRemoved_.length());
  MOZ_ASSERT(nBytes > 0);
  if (MOZ_UNLIKELY(!toSpace.trailersAdded_.append(blockAndListID))) {
    return false;
  }
  if (MOZ_UNLIKELY(!toSpace.trailersRemoved_.append(nullptr))) {
    toSpace.trailersAdded_.popBack();
    return false;
  }

  // This is a clone of the logic in ::registerMallocedBuffer.  It may be
  // that some other heuristic is better, once we know more about the
  // typical behaviour of wasm-GC applications.
  toSpace.trailerBytes_ += nBytes;
  if (MOZ_UNLIKELY(toSpace.trailerBytes_ > capacity() * 8)) {
    requestMinorGC(JS::GCReason::NURSERY_TRAILERS);
  }
  return true;
}

inline void js::Nursery::unregisterTrailer(void* block) {
  // Unlike removeMallocedBuffer this is only called during minor GC.
  MOZ_ASSERT(fromSpace.trailersRemovedUsed_ <
             fromSpace.trailersRemoved_.length());
  fromSpace.trailersRemoved_[fromSpace.trailersRemovedUsed_] = block;
  fromSpace.trailersRemovedUsed_++;
}

namespace js {

// The allocation methods below will not run the garbage collector. If the
// nursery cannot accomodate the allocation, the malloc heap will be used
// instead.

template <typename T>
static inline T* AllocateCellBuffer(Nursery& nursery, gc::Cell* cell,
                                    uint32_t count) {
  size_t nbytes = RoundUp(count * sizeof(T), sizeof(Value));
  return static_cast<T*>(
      nursery.allocateBuffer(cell->zone(), cell, nbytes, js::MallocArena));
}

template <typename T>
static inline T* AllocateCellBuffer(JSContext* cx, gc::Cell* cell,
                                    uint32_t count) {
  T* buffer = AllocateCellBuffer<T>(cx->nursery(), cell, count);
  if (!buffer) {
    ReportOutOfMemory(cx);
    return nullptr;
  }

  return buffer;
}

// If this returns null then the old buffer will be left alone.
template <typename T>
static inline T* ReallocateCellBuffer(JSContext* cx, gc::Cell* cell,
                                      T* oldBuffer, uint32_t oldCount,
                                      uint32_t newCount, arena_id_t arenaId) {
  size_t oldBytes = RoundUp(oldCount * sizeof(T), sizeof(Value));
  size_t newBytes = RoundUp(newCount * sizeof(T), sizeof(Value));

  T* buffer = static_cast<T*>(cx->nursery().reallocateBuffer(
      cell->zone(), cell, oldBuffer, oldBytes, newBytes, arenaId));
  if (!buffer) {
    ReportOutOfMemory(cx);
  }

  return buffer;
}

}  // namespace js

#endif /* gc_Nursery_inl_h */
