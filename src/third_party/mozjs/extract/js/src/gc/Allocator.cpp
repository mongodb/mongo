/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "gc/Allocator.h"

#include "mozilla/DebugOnly.h"
#include "mozilla/TimeStamp.h"

#include <type_traits>

#include "gc/GCInternals.h"
#include "gc/GCLock.h"
#include "gc/GCProbes.h"
#include "gc/Nursery.h"
#include "threading/CpuCount.h"
#include "util/Poison.h"
#include "vm/GetterSetter.h"
#include "vm/JSContext.h"
#include "vm/PropMap.h"
#include "vm/Runtime.h"
#include "vm/StringType.h"
#include "vm/TraceLogging.h"

#include "gc/ArenaList-inl.h"
#include "gc/Heap-inl.h"
#include "gc/PrivateIterators-inl.h"
#include "vm/JSObject-inl.h"

using mozilla::TimeDuration;
using mozilla::TimeStamp;

using namespace js;
using namespace gc;

template <AllowGC allowGC /* = CanGC */>
JSObject* js::AllocateObject(JSContext* cx, AllocKind kind,
                             size_t nDynamicSlots, gc::InitialHeap heap,
                             const JSClass* clasp,
                             AllocSite* site /* = nullptr */) {
  MOZ_ASSERT(IsObjectAllocKind(kind));
  size_t thingSize = Arena::thingSize(kind);

  MOZ_ASSERT(thingSize == Arena::thingSize(kind));
  MOZ_ASSERT(thingSize >= sizeof(JSObject_Slots0));
  static_assert(
      sizeof(JSObject_Slots0) >= MinCellSize,
      "All allocations must be at least the allocator-imposed minimum size.");

  MOZ_ASSERT_IF(nDynamicSlots != 0, clasp->isNativeObject());

  MOZ_ASSERT_IF(site && site->initialHeap() == TenuredHeap,
                heap == TenuredHeap);

  // We cannot trigger GC or make runtime assertions when nursery allocation
  // is suppressed, either explicitly or because we are off-thread.
  if (cx->isNurseryAllocSuppressed()) {
    JSObject* obj = GCRuntime::tryNewTenuredObject<NoGC>(cx, kind, thingSize,
                                                         nDynamicSlots);
    if (MOZ_UNLIKELY(allowGC && !obj)) {
      ReportOutOfMemory(cx);
    }
    return obj;
  }

  JSRuntime* rt = cx->runtime();
  if (!rt->gc.checkAllocatorState<allowGC>(cx, kind)) {
    return nullptr;
  }

  if (cx->nursery().isEnabled() && heap != TenuredHeap) {
    if (!site) {
      site = cx->zone()->unknownAllocSite();
    }

    JSObject* obj = rt->gc.tryNewNurseryObject<allowGC>(
        cx, thingSize, nDynamicSlots, clasp, site);
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

  return GCRuntime::tryNewTenuredObject<allowGC>(cx, kind, thingSize,
                                                 nDynamicSlots);
}
template JSObject* js::AllocateObject<NoGC>(JSContext* cx, gc::AllocKind kind,
                                            size_t nDynamicSlots,
                                            gc::InitialHeap heap,
                                            const JSClass* clasp,
                                            gc::AllocSite* site);
template JSObject* js::AllocateObject<CanGC>(JSContext* cx, gc::AllocKind kind,
                                             size_t nDynamicSlots,
                                             gc::InitialHeap heap,
                                             const JSClass* clasp,
                                             gc::AllocSite* site);

// Attempt to allocate a new JSObject out of the nursery. If there is not
// enough room in the nursery or there is an OOM, this method will return
// nullptr.
template <AllowGC allowGC>
JSObject* GCRuntime::tryNewNurseryObject(JSContext* cx, size_t thingSize,
                                         size_t nDynamicSlots,
                                         const JSClass* clasp,
                                         AllocSite* site) {
  MOZ_RELEASE_ASSERT(!cx->isHelperThreadContext());

  MOZ_ASSERT(cx->isNurseryAllocAllowed());
  MOZ_ASSERT(!cx->isNurseryAllocSuppressed());
  MOZ_ASSERT(!cx->zone()->isAtomsZone());

  JSObject* obj =
      cx->nursery().allocateObject(site, thingSize, nDynamicSlots, clasp);
  if (obj) {
    return obj;
  }

  if (allowGC && !cx->suppressGC) {
    cx->runtime()->gc.minorGC(JS::GCReason::OUT_OF_NURSERY);

    // Exceeding gcMaxBytes while tenuring can disable the Nursery.
    if (cx->nursery().isEnabled()) {
      return cx->nursery().allocateObject(site, thingSize, nDynamicSlots,
                                          clasp);
    }
  }
  return nullptr;
}

template <AllowGC allowGC>
JSObject* GCRuntime::tryNewTenuredObject(JSContext* cx, AllocKind kind,
                                         size_t thingSize,
                                         size_t nDynamicSlots) {
  ObjectSlots* slotsHeader = nullptr;
  if (nDynamicSlots) {
    HeapSlot* allocation =
        cx->maybe_pod_malloc<HeapSlot>(ObjectSlots::allocCount(nDynamicSlots));
    if (MOZ_UNLIKELY(!allocation)) {
      if (allowGC) {
        ReportOutOfMemory(cx);
      }
      return nullptr;
    }

    slotsHeader = new (allocation) ObjectSlots(nDynamicSlots, 0);
    Debug_SetSlotRangeToCrashOnTouch(slotsHeader->slots(), nDynamicSlots);
  }

  JSObject* obj = tryNewTenuredThing<JSObject, allowGC>(cx, kind, thingSize);

  if (obj) {
    if (nDynamicSlots) {
      static_cast<NativeObject*>(obj)->initSlots(slotsHeader->slots());
      AddCellMemory(obj, ObjectSlots::allocSize(nDynamicSlots),
                    MemoryUse::ObjectSlots);
    }
  } else {
    js_free(slotsHeader);
  }

  return obj;
}

// Attempt to allocate a new string out of the nursery. If there is not enough
// room in the nursery or there is an OOM, this method will return nullptr.
template <AllowGC allowGC>
JSString* GCRuntime::tryNewNurseryString(JSContext* cx, size_t thingSize,
                                         AllocKind kind) {
  MOZ_ASSERT(IsNurseryAllocable(kind));
  MOZ_ASSERT(cx->isNurseryAllocAllowed());
  MOZ_ASSERT(!cx->isHelperThreadContext());
  MOZ_ASSERT(!cx->isNurseryAllocSuppressed());
  MOZ_ASSERT(!cx->zone()->isAtomsZone());

  AllocSite* site = cx->zone()->unknownAllocSite();
  Cell* cell = cx->nursery().allocateString(site, thingSize);
  if (cell) {
    return static_cast<JSString*>(cell);
  }

  if (allowGC && !cx->suppressGC) {
    cx->runtime()->gc.minorGC(JS::GCReason::OUT_OF_NURSERY);

    // Exceeding gcMaxBytes while tenuring can disable the Nursery, and
    // other heuristics can disable nursery strings for this zone.
    if (cx->nursery().isEnabled() && cx->zone()->allocNurseryStrings) {
      return static_cast<JSString*>(
          cx->nursery().allocateString(site, thingSize));
    }
  }
  return nullptr;
}

template <typename StringAllocT, AllowGC allowGC /* = CanGC */>
StringAllocT* js::AllocateStringImpl(JSContext* cx, InitialHeap heap) {
  static_assert(std::is_convertible_v<StringAllocT*, JSString*>,
                "must be JSString derived");

  AllocKind kind = MapTypeToFinalizeKind<StringAllocT>::kind;
  size_t size = sizeof(StringAllocT);
  MOZ_ASSERT(size == Arena::thingSize(kind));
  MOZ_ASSERT(size == sizeof(JSString) || size == sizeof(JSFatInlineString));

  // Off-thread alloc cannot trigger GC or make runtime assertions.
  if (cx->isNurseryAllocSuppressed()) {
    StringAllocT* str =
        GCRuntime::tryNewTenuredThing<StringAllocT, NoGC>(cx, kind, size);
    if (MOZ_UNLIKELY(allowGC && !str)) {
      ReportOutOfMemory(cx);
    }
    return str;
  }

  JSRuntime* rt = cx->runtime();
  if (!rt->gc.checkAllocatorState<allowGC>(cx, kind)) {
    return nullptr;
  }

  if (cx->nursery().isEnabled() && heap != TenuredHeap &&
      cx->nursery().canAllocateStrings() && cx->zone()->allocNurseryStrings) {
    auto* str = static_cast<StringAllocT*>(
        rt->gc.tryNewNurseryString<allowGC>(cx, size, kind));
    if (str) {
      return str;
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

  return GCRuntime::tryNewTenuredThing<StringAllocT, allowGC>(cx, kind, size);
}

// Attempt to allocate a new BigInt out of the nursery. If there is not enough
// room in the nursery or there is an OOM, this method will return nullptr.
template <AllowGC allowGC>
JS::BigInt* GCRuntime::tryNewNurseryBigInt(JSContext* cx, size_t thingSize,
                                           AllocKind kind) {
  MOZ_ASSERT(IsNurseryAllocable(kind));
  MOZ_ASSERT(cx->isNurseryAllocAllowed());
  MOZ_ASSERT(!cx->isHelperThreadContext());
  MOZ_ASSERT(!cx->isNurseryAllocSuppressed());
  MOZ_ASSERT(!cx->zone()->isAtomsZone());

  AllocSite* site = cx->zone()->unknownAllocSite();
  Cell* cell = cx->nursery().allocateBigInt(site, thingSize);
  if (cell) {
    return static_cast<JS::BigInt*>(cell);
  }

  if (allowGC && !cx->suppressGC) {
    cx->runtime()->gc.minorGC(JS::GCReason::OUT_OF_NURSERY);

    // Exceeding gcMaxBytes while tenuring can disable the Nursery, and
    // other heuristics can disable nursery BigInts for this zone.
    if (cx->nursery().isEnabled() && cx->zone()->allocNurseryBigInts) {
      return static_cast<JS::BigInt*>(
          cx->nursery().allocateBigInt(site, thingSize));
    }
  }
  return nullptr;
}

template <AllowGC allowGC /* = CanGC */>
JS::BigInt* js::AllocateBigInt(JSContext* cx, InitialHeap heap) {
  AllocKind kind = MapTypeToFinalizeKind<JS::BigInt>::kind;
  size_t size = sizeof(JS::BigInt);
  MOZ_ASSERT(size == Arena::thingSize(kind));

  // Off-thread alloc cannot trigger GC or make runtime assertions.
  if (cx->isNurseryAllocSuppressed()) {
    JS::BigInt* bi =
        GCRuntime::tryNewTenuredThing<JS::BigInt, NoGC>(cx, kind, size);
    if (MOZ_UNLIKELY(allowGC && !bi)) {
      ReportOutOfMemory(cx);
    }
    return bi;
  }

  JSRuntime* rt = cx->runtime();
  if (!rt->gc.checkAllocatorState<allowGC>(cx, kind)) {
    return nullptr;
  }

  if (cx->nursery().isEnabled() && heap != TenuredHeap &&
      cx->nursery().canAllocateBigInts() && cx->zone()->allocNurseryBigInts) {
    auto* bi = static_cast<JS::BigInt*>(
        rt->gc.tryNewNurseryBigInt<allowGC>(cx, size, kind));
    if (bi) {
      return bi;
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

  return GCRuntime::tryNewTenuredThing<JS::BigInt, allowGC>(cx, kind, size);
}
template JS::BigInt* js::AllocateBigInt<NoGC>(JSContext* cx,
                                              gc::InitialHeap heap);
template JS::BigInt* js::AllocateBigInt<CanGC>(JSContext* cx,
                                               gc::InitialHeap heap);

#define DECL_ALLOCATOR_INSTANCES(allocKind, traceKind, type, sizedType, \
                                 bgfinal, nursery, compact)             \
  template type* js::AllocateStringImpl<type, NoGC>(JSContext * cx,     \
                                                    InitialHeap heap);  \
  template type* js::AllocateStringImpl<type, CanGC>(JSContext * cx,    \
                                                     InitialHeap heap);
FOR_EACH_NURSERY_STRING_ALLOCKIND(DECL_ALLOCATOR_INSTANCES)
#undef DECL_ALLOCATOR_INSTANCES

template <typename T, AllowGC allowGC /* = CanGC */>
T* js::Allocate(JSContext* cx) {
  static_assert(!std::is_convertible_v<T*, JSObject*>,
                "must not be JSObject derived");
  static_assert(
      sizeof(T) >= MinCellSize,
      "All allocations must be at least the allocator-imposed minimum size.");

  AllocKind kind = MapTypeToFinalizeKind<T>::kind;
  size_t thingSize = sizeof(T);
  MOZ_ASSERT(thingSize == Arena::thingSize(kind));

  if (!cx->isHelperThreadContext()) {
    if (!cx->runtime()->gc.checkAllocatorState<allowGC>(cx, kind)) {
      return nullptr;
    }
  }

  return GCRuntime::tryNewTenuredThing<T, allowGC>(cx, kind, thingSize);
}

#define DECL_ALLOCATOR_INSTANCES(allocKind, traceKind, type, sizedType, \
                                 bgFinal, nursery, compact)             \
  template type* js::Allocate<type, NoGC>(JSContext * cx);              \
  template type* js::Allocate<type, CanGC>(JSContext * cx);
FOR_EACH_NONOBJECT_NONNURSERY_ALLOCKIND(DECL_ALLOCATOR_INSTANCES)
#undef DECL_ALLOCATOR_INSTANCES

template <typename T, AllowGC allowGC>
/* static */
T* GCRuntime::tryNewTenuredThing(JSContext* cx, AllocKind kind,
                                 size_t thingSize) {
  // Bump allocate in the arena's current free-list span.
  auto* t = reinterpret_cast<T*>(cx->freeLists().allocate(kind));
  if (MOZ_UNLIKELY(!t)) {
    // Get the next available free list and allocate out of it. This may
    // acquire a new arena, which will lock the chunk list. If there are no
    // chunks available it may also allocate new memory directly.
    t = reinterpret_cast<T*>(refillFreeListFromAnyThread(cx, kind));

    if (MOZ_UNLIKELY(!t)) {
      if (allowGC) {
        cx->runtime()->gc.attemptLastDitchGC(cx);
        t = tryNewTenuredThing<T, NoGC>(cx, kind, thingSize);
      }
      if (!t) {
        if (allowGC) {
          ReportOutOfMemory(cx);
        }
        return nullptr;
      }
    }
  }

  checkIncrementalZoneState(cx, t);
  gcprobes::TenuredAlloc(t, kind);
  // We count this regardless of the profiler's state, assuming that it costs
  // just as much to count it, as to check the profiler's state and decide not
  // to count it.
  cx->noteTenuredAlloc();
  return t;
}

void GCRuntime::attemptLastDitchGC(JSContext* cx) {
  // Either there was no memory available for a new chunk or the heap hit its
  // size limit. Try to perform an all-compartments, non-incremental, shrinking
  // GC and wait for it to finish.

  if (cx->isHelperThreadContext()) {
    return;
  }

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
  if (allowGC) {
    if (!gcIfNeededAtAllocation(cx)) {
      return false;
    }
  }

#if defined(JS_GC_ZEAL) || defined(DEBUG)
  MOZ_ASSERT_IF(cx->zone()->isAtomsZone(),
                kind == AllocKind::ATOM || kind == AllocKind::FAT_INLINE_ATOM ||
                    kind == AllocKind::SYMBOL || kind == AllocKind::JITCODE ||
                    kind == AllocKind::SCOPE);
  MOZ_ASSERT_IF(!cx->zone()->isAtomsZone(),
                kind != AllocKind::ATOM && kind != AllocKind::FAT_INLINE_ATOM);
  MOZ_ASSERT_IF(cx->zone()->isSelfHostingZone(),
                !rt->parentRuntime && !selfHostingZoneFrozen);
  MOZ_ASSERT(!JS::RuntimeHeapIsBusy());
#endif

  // Crash if we perform a GC action when it is not safe.
  if (allowGC && !cx->suppressGC) {
    cx->verifyIsSafeToGC();
  }

  // For testing out of memory conditions
  if (js::oom::ShouldFailWithOOM()) {
    // If we are doing a fallible allocation, percolate up the OOM
    // instead of reporting it.
    if (allowGC) {
      ReportOutOfMemory(cx);
    }
    return false;
  }

  return true;
}

inline bool GCRuntime::gcIfNeededAtAllocation(JSContext* cx) {
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

  return true;
}

template <typename T>
/* static */
void GCRuntime::checkIncrementalZoneState(JSContext* cx, T* t) {
#ifdef DEBUG
  if (cx->isHelperThreadContext() || !t) {
    return;
  }

  TenuredCell* cell = &t->asTenured();
  Zone* zone = cell->zone();
  if (zone->isGCMarkingOrSweeping()) {
    MOZ_ASSERT(cell->isMarkedBlack());
  } else {
    MOZ_ASSERT(!cell->isMarkedAny());
  }
#endif
}

TenuredCell* js::gc::AllocateCellInGC(Zone* zone, AllocKind thingKind) {
  TenuredCell* cell = zone->arenas.allocateFromFreeList(thingKind);
  if (!cell) {
    AutoEnterOOMUnsafeRegion oomUnsafe;
    cell = GCRuntime::refillFreeListInGC(zone, thingKind);
    if (!cell) {
      oomUnsafe.crash(ChunkSize, "Failed to allocate new chunk during GC");
    }
  }
  return cell;
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
TenuredCell* GCRuntime::refillFreeListFromAnyThread(JSContext* cx,
                                                    AllocKind thingKind) {
  MOZ_ASSERT(cx->freeLists().isEmpty(thingKind));

  if (!cx->isHelperThreadContext()) {
    return refillFreeListFromMainThread(cx, thingKind);
  }

  return refillFreeListFromHelperThread(cx, thingKind);
}

/* static */
TenuredCell* GCRuntime::refillFreeListFromMainThread(JSContext* cx,
                                                     AllocKind thingKind) {
  // It should not be possible to allocate on the main thread while we are
  // inside a GC.
  MOZ_ASSERT(!JS::RuntimeHeapIsBusy(), "allocating while under GC");

  return cx->zone()->arenas.refillFreeListAndAllocate(
      cx->freeLists(), thingKind, ShouldCheckThresholds::CheckThresholds);
}

/* static */
TenuredCell* GCRuntime::refillFreeListFromHelperThread(JSContext* cx,
                                                       AllocKind thingKind) {
  // A GC may be happening on the main thread, but zones used by off thread
  // tasks are never collected.
  Zone* zone = cx->zone();
  MOZ_ASSERT(!zone->wasGCStarted());

  return zone->arenas.refillFreeListAndAllocate(
      cx->freeLists(), thingKind, ShouldCheckThresholds::CheckThresholds);
}

/* static */
TenuredCell* GCRuntime::refillFreeListInGC(Zone* zone, AllocKind thingKind) {
  // Called by compacting GC to refill a free list while we are in a GC.
  MOZ_ASSERT(JS::RuntimeHeapIsCollecting());
  MOZ_ASSERT_IF(!JS::RuntimeHeapIsMinorCollecting(),
                !zone->runtimeFromMainThread()->gc.isBackgroundSweeping());

  return zone->arenas.refillFreeListAndAllocate(
      zone->arenas.freeLists(), thingKind,
      ShouldCheckThresholds::DontCheckThresholds);
}

TenuredCell* ArenaLists::refillFreeListAndAllocate(
    FreeLists& freeLists, AllocKind thingKind,
    ShouldCheckThresholds checkThresholds) {
  MOZ_ASSERT(freeLists.isEmpty(thingKind));

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

    return freeLists.setArenaAndAllocate(arena, thingKind);
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

  addNewArena(arena, thingKind);

  return freeLists.setArenaAndAllocate(arena, thingKind);
}

inline void ArenaLists::addNewArena(Arena* arena, AllocKind thingKind) {
  ArenaList& al = zone_->isGCMarking() ? newArenasInMarkPhase(thingKind)
                                       : arenaList(thingKind);

  MOZ_ASSERT(al.isCursorAtEnd());
  al.insertBeforeCursor(arena);
}

inline TenuredCell* FreeLists::setArenaAndAllocate(Arena* arena,
                                                   AllocKind kind) {
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

void GCRuntime::setParallelAtomsAllocEnabled(bool enabled) {
  // This can only be changed on the main thread otherwise we could race.
  MOZ_ASSERT(CurrentThreadCanAccessRuntime(rt));
  MOZ_ASSERT(enabled == rt->hasHelperThreadZones());

  atomsZone->arenas.setParallelAllocEnabled(enabled);
}

void ArenaLists::setParallelAllocEnabled(bool enabled) {
  MOZ_ASSERT(zone_->isAtomsZone());

  static const ConcurrentUse states[2] = {ConcurrentUse::None,
                                          ConcurrentUse::ParallelAlloc};

  for (auto kind : AllAllocKinds()) {
    MOZ_ASSERT(concurrentUse(kind) == states[!enabled]);
    concurrentUse(kind) = states[enabled];
  }
}

void GCRuntime::setParallelUnmarkEnabled(bool enabled) {
  // This can only be changed on the main thread otherwise we could race.
  MOZ_ASSERT(CurrentThreadCanAccessRuntime(rt));
  MOZ_ASSERT(JS::RuntimeHeapIsMajorCollecting());
  for (GCZonesIter zone(this); !zone.done(); zone.next()) {
    zone->arenas.setParallelUnmarkEnabled(enabled);
  }
}

void ArenaLists::setParallelUnmarkEnabled(bool enabled) {
  static const ConcurrentUse states[2] = {ConcurrentUse::None,
                                          ConcurrentUse::ParallelUnmark};

  for (auto kind : AllAllocKinds()) {
    MOZ_ASSERT(concurrentUse(kind) == states[!enabled]);
    concurrentUse(kind) = states[enabled];
  }
}

// ///////////  TenuredChunk -> Arena Allocator  ///////////////////////////////

bool GCRuntime::wantBackgroundAllocation(const AutoLockGC& lock) const {
  // To minimize memory waste, we do not want to run the background chunk
  // allocation if we already have some empty chunks or when the runtime has
  // a small heap size (and therefore likely has a small growth rate).
  return allocTask.enabled() &&
         emptyChunks(lock).count() < tunables.minEmptyChunkCount(lock) &&
         (fullChunks(lock).count() + availableChunks(lock).count()) >= 4;
}

Arena* GCRuntime::allocateArena(TenuredChunk* chunk, Zone* zone,
                                AllocKind thingKind,
                                ShouldCheckThresholds checkThresholds,
                                const AutoLockGC& lock) {
  MOZ_ASSERT(chunk->hasAvailableArenas());

  // Fail the allocation if we are over our heap size limits.
  if ((checkThresholds != ShouldCheckThresholds::DontCheckThresholds) &&
      (heapSize.bytes() >= tunables.gcMaxBytes()))
    return nullptr;

  Arena* arena = chunk->allocateArena(this, zone, thingKind, lock);
  zone->gcHeapSize.addGCArena();

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
  return arena;
}

void TenuredChunk::commitOnePage(GCRuntime* gc) {
  MOZ_ASSERT(info.numArenasFreeCommitted == 0);
  MOZ_ASSERT(!info.freeArenasHead);
  MOZ_ASSERT(info.numArenasFree > 0);

  unsigned offset = findDecommittedPageOffset();
  info.lastDecommittedPageOffset = offset + 1;

  if (DecommitEnabled()) {
    MarkPagesInUseSoft(pageAddress(offset), PageSize);
  }

  size_t arenaIndex = offset * ArenasPerPage;
  decommittedPages[offset] = false;
  for (size_t i = 0; i < ArenasPerPage; i++) {
    arenas[arenaIndex + i].setAsNotAllocated();
  }

  // numArenasFreeCommitted will be updated in addArenasInPageToFreeList.
  // No need to update numArenasFree, as the arena is still free, it just
  // changes from decommitted to committed free. Later fetchNextFreeArena should
  // update the numArenasFree.
  addArenasInPageToFreeList(gc, offset);
}

inline void GCRuntime::updateOnFreeArenaAlloc(const TenuredChunkInfo& info) {
  MOZ_ASSERT(info.numArenasFreeCommitted <= numArenasFreeCommitted);
  --numArenasFreeCommitted;
}

Arena* TenuredChunk::fetchNextFreeArena(GCRuntime* gc) {
  MOZ_ASSERT(info.numArenasFreeCommitted > 0);
  MOZ_ASSERT(info.numArenasFreeCommitted <= info.numArenasFree);
  MOZ_ASSERT(info.freeArenasHead);

  Arena* arena = info.freeArenasHead;
  info.freeArenasHead = arena->next;
  --info.numArenasFreeCommitted;
  --info.numArenasFree;
  gc->updateOnFreeArenaAlloc(info);

  return arena;
}

/*
 * Search for and return the next decommitted page. Our goal is to keep
 * lastDecommittedPageOffset "close" to a free page. We do this by setting
 * it to the most recently freed page when we free, and forcing it to
 * the last alloc + 1 when we allocate.
 */
uint32_t TenuredChunk::findDecommittedPageOffset() {
  /* Note: lastFreeArenaOffset can be past the end of the list. */
  for (unsigned i = info.lastDecommittedPageOffset; i < PagesPerChunk; i++) {
    if (decommittedPages[i]) {
      return i;
    }
  }
  for (unsigned i = 0; i < info.lastDecommittedPageOffset; i++) {
    if (decommittedPages[i]) {
      return i;
    }
  }
  MOZ_CRASH("No decommitted arenas found.");
}

// ///////////  System -> TenuredChunk Allocator  //////////////////////////////

TenuredChunk* GCRuntime::getOrAllocChunk(AutoLockGCBgAlloc& lock) {
  TenuredChunk* chunk = emptyChunks(lock).pop();
  if (!chunk) {
    chunk = TenuredChunk::allocate(this);
    if (!chunk) {
      return nullptr;
    }
    MOZ_ASSERT(chunk->info.numArenasFreeCommitted == 0);
  }

  if (wantBackgroundAllocation(lock)) {
    lock.tryToStartBackgroundAllocation();
  }

  return chunk;
}

void GCRuntime::recycleChunk(TenuredChunk* chunk, const AutoLockGC& lock) {
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

  chunk->init(this);
  MOZ_ASSERT(chunk->info.numArenasFreeCommitted == 0);
  MOZ_ASSERT(chunk->unused());
  MOZ_ASSERT(!fullChunks(lock).contains(chunk));
  MOZ_ASSERT(!availableChunks(lock).contains(chunk));

  availableChunks(lock).push(chunk);

  return chunk;
}

BackgroundAllocTask::BackgroundAllocTask(GCRuntime* gc, ChunkPool& pool)
    : GCParallelTask(gc),
      chunkPool_(pool),
      enabled_(CanUseExtraThreads() && GetCPUCount() >= 2) {}

void BackgroundAllocTask::run(AutoLockHelperThreadState& lock) {
  AutoUnlockHelperThreadState unlock(lock);

  TraceLoggerThread* logger = TraceLoggerForCurrentThread();
  AutoTraceLog logAllocation(logger, TraceLogger_GCAllocation);

  AutoLockGC gcLock(gc);
  while (!isCancelled() && gc->wantBackgroundAllocation(gcLock)) {
    TenuredChunk* chunk;
    {
      AutoUnlockGC unlock(gcLock);
      chunk = TenuredChunk::allocate(gc);
      if (!chunk) {
        break;
      }
      chunk->init(gc);
    }
    chunkPool_.ref().push(chunk);
  }
}

/* static */
TenuredChunk* TenuredChunk::allocate(GCRuntime* gc) {
  void* chunk = MapAlignedPages(ChunkSize, ChunkSize);
  if (!chunk) {
    return nullptr;
  }

  gc->stats().count(gcstats::COUNT_NEW_CHUNK);
  return static_cast<TenuredChunk*>(chunk);
}

void TenuredChunk::init(GCRuntime* gc) {
  /* The chunk may still have some regions marked as no-access. */
  MOZ_MAKE_MEM_UNDEFINED(this, ChunkSize);

  /*
   * Poison the chunk. Note that decommitAllArenas() below will mark the
   * arenas as inaccessible (for memory sanitizers).
   */
  Poison(this, JS_FRESH_TENURED_PATTERN, ChunkSize,
         MemCheckKind::MakeUndefined);

  new (this) TenuredChunk(gc->rt);

  /*
   * Decommit the arenas. We do this after poisoning so that if the OS does
   * not have to recycle the pages, we still get the benefit of poisoning.
   */
  decommitAllArenas();

#ifdef DEBUG
  verify();
#endif

  /* The rest of info fields are initialized in pickChunk. */
}

void TenuredChunk::decommitAllArenas() {
  decommittedPages.SetAll();
  if (DecommitEnabled()) {
    MarkPagesUnusedSoft(&arenas[0], ArenasPerChunk * ArenaSize);
  }

  info.freeArenasHead = nullptr;
  info.lastDecommittedPageOffset = 0;
  info.numArenasFree = ArenasPerChunk;
  info.numArenasFreeCommitted = 0;
}
