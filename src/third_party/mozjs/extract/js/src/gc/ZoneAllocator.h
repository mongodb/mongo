/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * Public header for allocating memory associated with GC things.
 */

#ifndef gc_ZoneAllocator_h
#define gc_ZoneAllocator_h

#include "jstypes.h"
#include "gc/Cell.h"
#include "gc/Scheduling.h"
#include "js/GCAPI.h"
#include "js/HeapAPI.h"
#include "js/shadow/Zone.h"  // JS::shadow::Zone
#include "vm/MallocProvider.h"

namespace JS {
class JS_PUBLIC_API Zone;
}  // namespace JS

namespace js {

class ZoneAllocator;

#ifdef DEBUG
bool CurrentThreadIsGCFinalizing();
#endif

namespace gc {
void MaybeMallocTriggerZoneGC(JSRuntime* rt, ZoneAllocator* zoneAlloc,
                              const HeapSize& heap,
                              const HeapThreshold& threshold,
                              JS::GCReason reason);
}

// Base class of JS::Zone that provides malloc memory allocation and accounting.
class ZoneAllocator : public JS::shadow::Zone,
                      public js::MallocProvider<JS::Zone> {
 protected:
  explicit ZoneAllocator(JSRuntime* rt, Kind kind);
  ~ZoneAllocator();
  void fixupAfterMovingGC();

 public:
  static ZoneAllocator* from(JS::Zone* zone) {
    // This is a safe upcast, but the compiler hasn't seen the definition yet.
    return reinterpret_cast<ZoneAllocator*>(zone);
  }

  [[nodiscard]] void* onOutOfMemory(js::AllocFunction allocFunc,
                                    arena_id_t arena, size_t nbytes,
                                    void* reallocPtr = nullptr);
  void reportAllocationOverflow() const;

  void adoptMallocBytes(ZoneAllocator* other) {
    mallocHeapSize.adopt(other->mallocHeapSize);
    jitHeapSize.adopt(other->jitHeapSize);
#ifdef DEBUG
    mallocTracker.adopt(other->mallocTracker);
#endif
  }

  void updateMemoryCountersOnGCStart();
  void updateGCStartThresholds(gc::GCRuntime& gc, JS::GCOptions options,
                               const js::AutoLockGC& lock);
  void setGCSliceThresholds(gc::GCRuntime& gc);
  void clearGCSliceThresholds();

  // Memory accounting APIs for malloc memory owned by GC cells.

  void addCellMemory(js::gc::Cell* cell, size_t nbytes, js::MemoryUse use) {
    MOZ_ASSERT(cell);
    MOZ_ASSERT(nbytes);

    mallocHeapSize.addBytes(nbytes);

#ifdef DEBUG
    mallocTracker.trackGCMemory(cell, nbytes, use);
#endif

    maybeTriggerGCOnMalloc();
  }

  void removeCellMemory(js::gc::Cell* cell, size_t nbytes, js::MemoryUse use,
                        bool wasSwept = false) {
    MOZ_ASSERT(cell);
    MOZ_ASSERT(nbytes);
    MOZ_ASSERT_IF(CurrentThreadIsGCFinalizing(), wasSwept);

    mallocHeapSize.removeBytes(nbytes, wasSwept);

#ifdef DEBUG
    mallocTracker.untrackGCMemory(cell, nbytes, use);
#endif
  }

  void swapCellMemory(js::gc::Cell* a, js::gc::Cell* b, js::MemoryUse use) {
#ifdef DEBUG
    mallocTracker.swapGCMemory(a, b, use);
#endif
  }

  void registerNonGCMemory(void* mem, MemoryUse use) {
#ifdef DEBUG
    return mallocTracker.registerNonGCMemory(mem, use);
#endif
  }
  void unregisterNonGCMemory(void* mem, MemoryUse use) {
#ifdef DEBUG
    return mallocTracker.unregisterNonGCMemory(mem, use);
#endif
  }
  void moveOtherMemory(void* dst, void* src, MemoryUse use) {
#ifdef DEBUG
    return mallocTracker.moveNonGCMemory(dst, src, use);
#endif
  }

  void incNonGCMemory(void* mem, size_t nbytes, MemoryUse use) {
    MOZ_ASSERT(nbytes);
    mallocHeapSize.addBytes(nbytes);

#ifdef DEBUG
    mallocTracker.incNonGCMemory(mem, nbytes, use);
#endif

    maybeTriggerGCOnMalloc();
  }
  void decNonGCMemory(void* mem, size_t nbytes, MemoryUse use, bool wasSwept) {
    MOZ_ASSERT(nbytes);
    MOZ_ASSERT_IF(CurrentThreadIsGCFinalizing(), wasSwept);

    mallocHeapSize.removeBytes(nbytes, wasSwept);

#ifdef DEBUG
    mallocTracker.decNonGCMemory(mem, nbytes, use);
#endif
  }

  // Account for allocations that may be referenced by more than one GC thing.
  bool addSharedMemory(void* mem, size_t nbytes, MemoryUse use);
  void removeSharedMemory(void* mem, size_t nbytes, MemoryUse use);

  void incJitMemory(size_t nbytes) {
    MOZ_ASSERT(nbytes);
    jitHeapSize.addBytes(nbytes);
    maybeTriggerZoneGC(jitHeapSize, jitHeapThreshold,
                       JS::GCReason::TOO_MUCH_JIT_CODE);
  }
  void decJitMemory(size_t nbytes) {
    MOZ_ASSERT(nbytes);
    jitHeapSize.removeBytes(nbytes, true);
  }

  // Check malloc allocation threshold and trigger a zone GC if necessary.
  void maybeTriggerGCOnMalloc() {
    maybeTriggerZoneGC(mallocHeapSize, mallocHeapThreshold,
                       JS::GCReason::TOO_MUCH_MALLOC);
  }

 private:
  void maybeTriggerZoneGC(const js::gc::HeapSize& heap,
                          const js::gc::HeapThreshold& threshold,
                          JS::GCReason reason) {
    if (heap.bytes() >= threshold.startBytes()) {
      gc::MaybeMallocTriggerZoneGC(runtimeFromAnyThread(), this, heap,
                                   threshold, reason);
    }
  }

 public:
  // The size of allocated GC arenas in this zone.
  gc::HeapSize gcHeapSize;

  // Threshold used to trigger GC based on GC heap size.
  gc::GCHeapThreshold gcHeapThreshold;

  // Amount of malloc data owned by tenured GC things in this zone, including
  // external allocations supplied by JS::AddAssociatedMemory.
  gc::HeapSize mallocHeapSize;

  // Threshold used to trigger GC based on malloc allocations.
  gc::MallocHeapThreshold mallocHeapThreshold;

  // Amount of exectuable JIT code owned by GC things in this zone.
  gc::HeapSize jitHeapSize;

  // Threshold used to trigger GC based on JIT allocations.
  gc::JitHeapThreshold jitHeapThreshold;

  // Use counts for memory that can be referenced by more than one GC thing.
  gc::SharedMemoryMap sharedMemoryUseCounts;

 private:
#ifdef DEBUG
  // In debug builds, malloc allocations can be tracked to make debugging easier
  // (possible?) if allocation and free sizes don't balance.
  gc::MemoryTracker mallocTracker;
#endif

  friend class gc::GCRuntime;
};

/*
 * Allocation policy that performs precise memory tracking on the zone. This
 * should be used for all containers associated with a GC thing or a zone.
 *
 * Since it doesn't hold a JSContext (those may not live long enough), it can't
 * report out-of-memory conditions itself; the caller must check for OOM and
 * take the appropriate action.
 *
 * FIXME bug 647103 - replace these *AllocPolicy names.
 */
class ZoneAllocPolicy : public MallocProvider<ZoneAllocPolicy> {
  ZoneAllocator* zone_;

#ifdef DEBUG
  friend class js::gc::MemoryTracker;  // Can clear |zone_| on merge.
#endif

 public:
  MOZ_IMPLICIT ZoneAllocPolicy(ZoneAllocator* z) : zone_(z) {
    zone()->registerNonGCMemory(this, MemoryUse::ZoneAllocPolicy);
  }
  MOZ_IMPLICIT ZoneAllocPolicy(JS::Zone* z)
      : ZoneAllocPolicy(ZoneAllocator::from(z)) {}
  ZoneAllocPolicy(ZoneAllocPolicy& other) : ZoneAllocPolicy(other.zone_) {}
  ZoneAllocPolicy(ZoneAllocPolicy&& other) : zone_(other.zone_) {
    zone()->moveOtherMemory(this, &other, MemoryUse::ZoneAllocPolicy);
    other.zone_ = nullptr;
  }
  ~ZoneAllocPolicy() {
    if (zone_) {
      zone_->unregisterNonGCMemory(this, MemoryUse::ZoneAllocPolicy);
    }
  }

  ZoneAllocPolicy& operator=(const ZoneAllocPolicy& other) {
    zone()->unregisterNonGCMemory(this, MemoryUse::ZoneAllocPolicy);
    zone_ = other.zone();
    zone()->registerNonGCMemory(this, MemoryUse::ZoneAllocPolicy);
    return *this;
  }
  ZoneAllocPolicy& operator=(ZoneAllocPolicy&& other) {
    MOZ_ASSERT(this != &other);
    zone()->unregisterNonGCMemory(this, MemoryUse::ZoneAllocPolicy);
    zone_ = other.zone();
    zone()->moveOtherMemory(this, &other, MemoryUse::ZoneAllocPolicy);
    other.zone_ = nullptr;
    return *this;
  }

  // Public methods required to fulfill the AllocPolicy interface.

  template <typename T>
  void free_(T* p, size_t numElems) {
    if (p) {
      decMemory(numElems * sizeof(T));
      js_free(p);
    }
  }

  [[nodiscard]] bool checkSimulatedOOM() const {
    return !js::oom::ShouldFailWithOOM();
  }

  void reportAllocOverflow() const { reportAllocationOverflow(); }

  // Internal methods called by the MallocProvider implementation.

  [[nodiscard]] void* onOutOfMemory(js::AllocFunction allocFunc,
                                    arena_id_t arena, size_t nbytes,
                                    void* reallocPtr = nullptr) {
    return zone()->onOutOfMemory(allocFunc, arena, nbytes, reallocPtr);
  }
  void reportAllocationOverflow() const { zone()->reportAllocationOverflow(); }
  void updateMallocCounter(size_t nbytes) {
    zone()->incNonGCMemory(this, nbytes, MemoryUse::ZoneAllocPolicy);
  }

 private:
  ZoneAllocator* zone() const {
    MOZ_ASSERT(zone_);
    return zone_;
  }
  void decMemory(size_t nbytes);
};

// Functions for memory accounting on the zone.

// Associate malloc memory with a GC thing. This call should be matched by a
// following call to RemoveCellMemory with the same size and use. The total
// amount of malloc memory associated with a zone is used to trigger GC.
//
// You should use InitReservedSlot / InitObjectPrivate in preference to this
// where possible.

inline void AddCellMemory(gc::TenuredCell* cell, size_t nbytes, MemoryUse use) {
  if (nbytes) {
    ZoneAllocator::from(cell->zone())->addCellMemory(cell, nbytes, use);
  }
}
inline void AddCellMemory(gc::Cell* cell, size_t nbytes, MemoryUse use) {
  if (cell->isTenured()) {
    AddCellMemory(&cell->asTenured(), nbytes, use);
  }
}

// Remove association between malloc memory and a GC thing. This call should
// follow a call to AddCellMemory with the same size and use.

inline void RemoveCellMemory(gc::TenuredCell* cell, size_t nbytes,
                             MemoryUse use, bool wasSwept = false) {
  if (nbytes) {
    auto zoneBase = ZoneAllocator::from(cell->zoneFromAnyThread());
    zoneBase->removeCellMemory(cell, nbytes, use, wasSwept);
  }
}
inline void RemoveCellMemory(gc::Cell* cell, size_t nbytes, MemoryUse use,
                             bool wasSwept = false) {
  if (cell->isTenured()) {
    RemoveCellMemory(&cell->asTenured(), nbytes, use, wasSwept);
  }
}

}  // namespace js

#endif  // gc_ZoneAllocator_h
