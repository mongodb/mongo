/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * Implementation of GC sweeping.
 *
 * In the SpiderMonkey GC, 'sweeping' is used to mean two things:
 *  - updating data structures to remove pointers to dead GC things and updating
 *    pointers to moved GC things
 *  - finalizing dead GC things
 *
 * Furthermore, the GC carries out gray and weak marking after the start of the
 * sweep phase. This is also implemented in this file.
 */

#include "mozilla/Maybe.h"
#include "mozilla/ScopeExit.h"
#include "mozilla/TimeStamp.h"

#include "builtin/FinalizationRegistryObject.h"
#include "builtin/WeakRefObject.h"
#include "debugger/DebugAPI.h"
#include "gc/AllocKind.h"
#include "gc/FinalizationObservers.h"
#include "gc/GCInternals.h"
#include "gc/GCLock.h"
#include "gc/GCProbes.h"
#include "gc/GCRuntime.h"
#include "gc/ParallelWork.h"
#include "gc/Statistics.h"
#include "gc/TraceKind.h"
#include "gc/WeakMap.h"
#include "gc/Zone.h"
#include "jit/JitFrames.h"
#include "jit/JitRuntime.h"
#include "jit/JitZone.h"
#include "proxy/DeadObjectProxy.h"
#include "vm/BigIntType.h"
#include "vm/HelperThreads.h"
#include "vm/JSContext.h"
#include "vm/Time.h"
#include "vm/WrapperObject.h"

#include "gc/PrivateIterators-inl.h"
#include "vm/GeckoProfiler-inl.h"
#include "vm/JSObject-inl.h"
#include "vm/PropMap-inl.h"
#include "vm/Shape-inl.h"
#include "vm/StringType-inl.h"

using namespace js;
using namespace js::gc;

using mozilla::TimeStamp;

struct js::gc::FinalizePhase {
  gcstats::PhaseKind statsPhase;
  AllocKinds kinds;
};

/*
 * Finalization order for objects swept incrementally on the main thread.
 */
static constexpr FinalizePhase ForegroundObjectFinalizePhase = {
    gcstats::PhaseKind::FINALIZE_OBJECT,
    {AllocKind::OBJECT0, AllocKind::OBJECT2, AllocKind::OBJECT4,
     AllocKind::OBJECT8, AllocKind::OBJECT12, AllocKind::OBJECT16}};

/*
 * Finalization order for GC things swept incrementally on the main thread.
 */
static constexpr FinalizePhase ForegroundNonObjectFinalizePhase = {
    gcstats::PhaseKind::FINALIZE_NON_OBJECT,
    {AllocKind::SCRIPT, AllocKind::JITCODE}};

/*
 * Finalization order for GC things swept on the background thread.
 */
static constexpr FinalizePhase BackgroundFinalizePhases[] = {
    {gcstats::PhaseKind::FINALIZE_OBJECT,
     {AllocKind::FUNCTION, AllocKind::FUNCTION_EXTENDED,
      AllocKind::OBJECT0_BACKGROUND, AllocKind::OBJECT2_BACKGROUND,
      AllocKind::ARRAYBUFFER4, AllocKind::OBJECT4_BACKGROUND,
      AllocKind::ARRAYBUFFER8, AllocKind::OBJECT8_BACKGROUND,
      AllocKind::ARRAYBUFFER12, AllocKind::OBJECT12_BACKGROUND,
      AllocKind::ARRAYBUFFER16, AllocKind::OBJECT16_BACKGROUND}},
    {gcstats::PhaseKind::FINALIZE_NON_OBJECT,
     {AllocKind::SCOPE, AllocKind::REGEXP_SHARED, AllocKind::FAT_INLINE_STRING,
      AllocKind::STRING, AllocKind::EXTERNAL_STRING, AllocKind::FAT_INLINE_ATOM,
      AllocKind::ATOM, AllocKind::SYMBOL, AllocKind::BIGINT, AllocKind::SHAPE,
      AllocKind::BASE_SHAPE, AllocKind::GETTER_SETTER,
      AllocKind::COMPACT_PROP_MAP, AllocKind::NORMAL_PROP_MAP,
      AllocKind::DICT_PROP_MAP}}};

template <typename T>
inline size_t Arena::finalize(JS::GCContext* gcx, AllocKind thingKind,
                              size_t thingSize) {
  /* Enforce requirements on size of T. */
  MOZ_ASSERT(thingSize % CellAlignBytes == 0);
  MOZ_ASSERT(thingSize >= MinCellSize);
  MOZ_ASSERT(thingSize <= 255);

  MOZ_ASSERT(allocated());
  MOZ_ASSERT(thingKind == getAllocKind());
  MOZ_ASSERT(thingSize == getThingSize());
  MOZ_ASSERT(!onDelayedMarkingList_);

  uint_fast16_t firstThing = firstThingOffset(thingKind);
  uint_fast16_t firstThingOrSuccessorOfLastMarkedThing = firstThing;
  uint_fast16_t lastThing = ArenaSize - thingSize;

  FreeSpan newListHead;
  FreeSpan* newListTail = &newListHead;
  size_t nmarked = 0, nfinalized = 0;

  for (ArenaCellIterUnderFinalize cell(this); !cell.done(); cell.next()) {
    T* t = cell.as<T>();
    if (TenuredThingIsMarkedAny(t)) {
      uint_fast16_t thing = uintptr_t(t) & ArenaMask;
      if (thing != firstThingOrSuccessorOfLastMarkedThing) {
        // We just finished passing over one or more free things,
        // so record a new FreeSpan.
        newListTail->initBounds(firstThingOrSuccessorOfLastMarkedThing,
                                thing - thingSize, this);
        newListTail = newListTail->nextSpanUnchecked(this);
      }
      firstThingOrSuccessorOfLastMarkedThing = thing + thingSize;
      nmarked++;
    } else {
      t->finalize(gcx);
      AlwaysPoison(t, JS_SWEPT_TENURED_PATTERN, thingSize,
                   MemCheckKind::MakeUndefined);
      gcprobes::TenuredFinalize(t);
      nfinalized++;
    }
  }

  if constexpr (std::is_same_v<T, JSObject> || std::is_same_v<T, JSString> ||
                std::is_same_v<T, JS::BigInt>) {
    if (isNewlyCreated_) {
      zone->pretenuring.updateCellCountsInNewlyCreatedArenas(
          nmarked + nfinalized, nmarked);
    }
  }
  isNewlyCreated_ = 0;

  if (nmarked == 0) {
    // Do nothing. The caller will update the arena appropriately.
    MOZ_ASSERT(newListTail == &newListHead);
    DebugOnlyPoison(data, JS_SWEPT_TENURED_PATTERN, sizeof(data),
                    MemCheckKind::MakeUndefined);
    return nmarked;
  }

  MOZ_ASSERT(firstThingOrSuccessorOfLastMarkedThing != firstThing);
  uint_fast16_t lastMarkedThing =
      firstThingOrSuccessorOfLastMarkedThing - thingSize;
  if (lastThing == lastMarkedThing) {
    // If the last thing was marked, we will have already set the bounds of
    // the final span, and we just need to terminate the list.
    newListTail->initAsEmpty();
  } else {
    // Otherwise, end the list with a span that covers the final stretch of free
    // things.
    newListTail->initFinal(firstThingOrSuccessorOfLastMarkedThing, lastThing,
                           this);
  }

  firstFreeSpan = newListHead;
#ifdef DEBUG
  size_t nfree = numFreeThings(thingSize);
  MOZ_ASSERT(nfree + nmarked == thingsPerArena(thingKind));
#endif
  return nmarked;
}

// Finalize arenas from src list, releasing empty arenas if keepArenas wasn't
// specified and inserting the others into the appropriate destination size
// bins.
template <typename T>
static inline bool FinalizeTypedArenas(JS::GCContext* gcx, ArenaList& src,
                                       SortedArenaList& dest,
                                       AllocKind thingKind,
                                       SliceBudget& budget) {
  MOZ_ASSERT(gcx->isFinalizing());

  size_t thingSize = Arena::thingSize(thingKind);
  size_t thingsPerArena = Arena::thingsPerArena(thingKind);
  size_t markCount = 0;

  auto updateMarkCount = mozilla::MakeScopeExit([&] {
    GCRuntime* gc = &gcx->runtimeFromAnyThread()->gc;
    gc->stats().addCount(gcstats::COUNT_CELLS_MARKED, markCount);
  });

  while (Arena* arena = src.takeFirstArena()) {
    size_t nmarked = arena->finalize<T>(gcx, thingKind, thingSize);
    size_t nfree = thingsPerArena - nmarked;

    markCount += nmarked;

    if (nmarked) {
      dest.insertAt(arena, nfree);
    } else {
      arena->chunk()->recycleArena(arena, dest, thingsPerArena);
    }

    budget.step(thingsPerArena);
    if (budget.isOverBudget()) {
      return false;
    }
  }

  return true;
}

/*
 * Finalize the list of areans.
 */
static bool FinalizeArenas(JS::GCContext* gcx, ArenaList& src,
                           SortedArenaList& dest, AllocKind thingKind,
                           SliceBudget& budget) {
  switch (thingKind) {
#define EXPAND_CASE(allocKind, traceKind, type, sizedType, bgFinal, nursery, \
                    compact)                                                 \
  case AllocKind::allocKind:                                                 \
    return FinalizeTypedArenas<type>(gcx, src, dest, thingKind, budget);
    FOR_EACH_ALLOCKIND(EXPAND_CASE)
#undef EXPAND_CASE

    default:
      MOZ_CRASH("Invalid alloc kind");
  }
}

void GCRuntime::initBackgroundSweep(Zone* zone, JS::GCContext* gcx,
                                    const FinalizePhase& phase) {
  gcstats::AutoPhase ap(stats(), phase.statsPhase);
  for (auto kind : phase.kinds) {
    zone->arenas.initBackgroundSweep(kind);
  }
}

void ArenaLists::initBackgroundSweep(AllocKind thingKind) {
  MOZ_ASSERT(IsBackgroundFinalized(thingKind));
  MOZ_ASSERT(concurrentUse(thingKind) == ConcurrentUse::None);

  if (!collectingArenaList(thingKind).isEmpty()) {
    concurrentUse(thingKind) = ConcurrentUse::BackgroundFinalize;
  }
}

void GCRuntime::backgroundFinalize(JS::GCContext* gcx, Zone* zone,
                                   AllocKind kind, Arena** empty) {
  MOZ_ASSERT(empty);

  ArenaLists* lists = &zone->arenas;
  ArenaList& arenas = lists->collectingArenaList(kind);
  if (arenas.isEmpty()) {
    MOZ_ASSERT(lists->concurrentUse(kind) == ArenaLists::ConcurrentUse::None);
    return;
  }

  SortedArenaList finalizedSorted(kind);

  auto unlimited = SliceBudget::unlimited();
  FinalizeArenas(gcx, arenas, finalizedSorted, kind, unlimited);
  MOZ_ASSERT(arenas.isEmpty());

  finalizedSorted.extractEmptyTo(empty);

  // When marking begins, all arenas are moved from arenaLists to
  // collectingArenaLists. When the mutator runs, new arenas are allocated in
  // arenaLists. Now that finalization is complete, we want to merge these lists
  // back together.

  // We must take the GC lock to be able to safely modify the ArenaList;
  // however, this does not by itself make the changes visible to all threads,
  // as not all threads take the GC lock to read the ArenaLists.
  // That safety is provided by the ReleaseAcquire memory ordering of the
  // background finalize state, which we explicitly set as the final step.
  {
    AutoLockGC lock(rt);
    MOZ_ASSERT(lists->concurrentUse(kind) ==
               ArenaLists::ConcurrentUse::BackgroundFinalize);
    lists->mergeFinalizedArenas(kind, finalizedSorted);
  }

  lists->concurrentUse(kind) = ArenaLists::ConcurrentUse::None;
}

// After finalizing arenas, merge the following to get the final state of an
// arena list:
//  - arenas allocated during marking
//  - arenas allocated during sweeping
//  - finalized arenas
void ArenaLists::mergeFinalizedArenas(AllocKind kind,
                                      SortedArenaList& finalizedArenas) {
#ifdef DEBUG
  // Updating arena lists off-thread requires taking the GC lock because the
  // main thread uses these when allocating.
  if (IsBackgroundFinalized(kind)) {
    runtimeFromAnyThread()->gc.assertCurrentThreadHasLockedGC();
  }
#endif

  ArenaList& arenas = arenaList(kind);

  ArenaList allocatedDuringCollection = std::move(arenas);
  arenas = finalizedArenas.convertToArenaList();
  arenas.insertListWithCursorAtEnd(allocatedDuringCollection);

  collectingArenaList(kind).clear();
}

void ArenaLists::queueForegroundThingsForSweep() {
  gcCompactPropMapArenasToUpdate =
      collectingArenaList(AllocKind::COMPACT_PROP_MAP).head();
  gcNormalPropMapArenasToUpdate =
      collectingArenaList(AllocKind::NORMAL_PROP_MAP).head();
}

void GCRuntime::sweepBackgroundThings(ZoneList& zones) {
  if (zones.isEmpty()) {
    return;
  }

  JS::GCContext* gcx = TlsGCContext.get();
  MOZ_ASSERT(gcx->isFinalizing());

  // Sweep zones in order. The atoms zone must be finalized last as other
  // zones may have direct pointers into it.
  while (!zones.isEmpty()) {
    Zone* zone = zones.removeFront();
    MOZ_ASSERT(zone->isGCFinished());

    TimeStamp startTime = TimeStamp::Now();

    Arena* emptyArenas = zone->arenas.takeSweptEmptyArenas();

    // We must finalize thing kinds in the order specified by
    // BackgroundFinalizePhases.
    for (const auto& phase : BackgroundFinalizePhases) {
      for (auto kind : phase.kinds) {
        backgroundFinalize(gcx, zone, kind, &emptyArenas);
      }
    }

    // Release any arenas that are now empty.
    //
    // Empty arenas are only released after everything has been finalized so
    // that it's still possible to get a thing's zone after the thing has been
    // finalized. The HeapPtr destructor depends on this, and this allows
    // HeapPtrs between things of different alloc kind regardless of
    // finalization order.
    //
    // Periodically drop and reaquire the GC lock every so often to avoid
    // blocking the main thread from allocating chunks.
    static const size_t LockReleasePeriod = 32;

    while (emptyArenas) {
      AutoLockGC lock(this);
      for (size_t i = 0; i < LockReleasePeriod && emptyArenas; i++) {
        Arena* arena = emptyArenas;
        emptyArenas = emptyArenas->next;
        releaseArena(arena, lock);
      }
    }

    // Record time spent sweeping this zone.
    TimeStamp endTime = TimeStamp::Now();
    zone->perZoneGCTime += endTime - startTime;
  }
}

void GCRuntime::assertBackgroundSweepingFinished() {
#ifdef DEBUG
  {
    AutoLockHelperThreadState lock;
    MOZ_ASSERT(backgroundSweepZones.ref().isEmpty());
  }

  for (ZonesIter zone(this, WithAtoms); !zone.done(); zone.next()) {
    for (auto kind : AllAllocKinds()) {
      MOZ_ASSERT_IF(state() != State::Prepare && state() != State::Mark &&
                        state() != State::Sweep,
                    zone->arenas.collectingArenaList(kind).isEmpty());
      MOZ_ASSERT(zone->arenas.doneBackgroundFinalize(kind));
    }
  }
#endif
}

void GCRuntime::queueZonesAndStartBackgroundSweep(ZoneList&& zones) {
  {
    AutoLockHelperThreadState lock;
    MOZ_ASSERT(!requestSliceAfterBackgroundTask);
    backgroundSweepZones.ref().appendList(std::move(zones));
    if (useBackgroundThreads) {
      sweepTask.startOrRunIfIdle(lock);
    }
  }
  if (!useBackgroundThreads) {
    sweepTask.join();
    sweepTask.runFromMainThread();
  }
}

BackgroundSweepTask::BackgroundSweepTask(GCRuntime* gc)
    : GCParallelTask(gc, gcstats::PhaseKind::SWEEP, GCUse::Finalizing) {}

void BackgroundSweepTask::run(AutoLockHelperThreadState& lock) {
  gc->sweepFromBackgroundThread(lock);
}

void GCRuntime::sweepFromBackgroundThread(AutoLockHelperThreadState& lock) {
  do {
    ZoneList zones;
    zones.appendList(std::move(backgroundSweepZones.ref()));

    AutoUnlockHelperThreadState unlock(lock);
    sweepBackgroundThings(zones);

    // The main thread may call queueZonesAndStartBackgroundSweep() while this
    // is running so we must check there is no more work after releasing the
    // lock.
  } while (!backgroundSweepZones.ref().isEmpty());

  maybeRequestGCAfterBackgroundTask(lock);
}

void GCRuntime::waitBackgroundSweepEnd() {
  sweepTask.join();
  if (state() != State::Sweep) {
    assertBackgroundSweepingFinished();
  }
}

void GCRuntime::startBackgroundFree() {
  AutoLockHelperThreadState lock;

  if (lifoBlocksToFree.ref().isEmpty() &&
      buffersToFreeAfterMinorGC.ref().empty()) {
    return;
  }

  freeTask.startOrRunIfIdle(lock);
}

BackgroundFreeTask::BackgroundFreeTask(GCRuntime* gc)
    : GCParallelTask(gc, gcstats::PhaseKind::NONE) {
  // This can occur outside GCs so doesn't have a stats phase.
}

void BackgroundFreeTask::run(AutoLockHelperThreadState& lock) {
  gc->freeFromBackgroundThread(lock);
}

void GCRuntime::freeFromBackgroundThread(AutoLockHelperThreadState& lock) {
  do {
    LifoAlloc lifoBlocks(JSContext::TEMP_LIFO_ALLOC_PRIMARY_CHUNK_SIZE);
    lifoBlocks.transferFrom(&lifoBlocksToFree.ref());

    Nursery::BufferSet buffers;
    std::swap(buffers, buffersToFreeAfterMinorGC.ref());

    AutoUnlockHelperThreadState unlock(lock);

    lifoBlocks.freeAll();

    JS::GCContext* gcx = TlsGCContext.get();
    for (Nursery::BufferSet::Range r = buffers.all(); !r.empty();
         r.popFront()) {
      // Malloc memory associated with nursery objects is not tracked as these
      // are assumed to be short lived.
      gcx->freeUntracked(r.front());
    }
  } while (!lifoBlocksToFree.ref().isEmpty() ||
           !buffersToFreeAfterMinorGC.ref().empty());
}

void GCRuntime::waitBackgroundFreeEnd() { freeTask.join(); }

template <class ZoneIterT>
IncrementalProgress GCRuntime::markWeakReferences(
    SliceBudget& incrementalBudget) {
  MOZ_ASSERT(!marker().isWeakMarking());

  gcstats::AutoPhase ap1(stats(), gcstats::PhaseKind::MARK_WEAK);

  auto unlimited = SliceBudget::unlimited();
  SliceBudget& budget =
      marker().incrementalWeakMapMarkingEnabled ? incrementalBudget : unlimited;

  // Ensure we don't return to the mutator while we're still in weak marking
  // mode.
  auto leaveOnExit =
      mozilla::MakeScopeExit([&] { marker().leaveWeakMarkingMode(); });

  if (marker().enterWeakMarkingMode()) {
    // If there was an 'enter-weak-marking-mode' token in the queue, then it
    // and everything after it will still be in the queue so we can process
    // them now.
    while (processTestMarkQueue() == QueueYielded) {
    };

    // Do not rely on the information about not-yet-marked weak keys that have
    // been collected by barriers. Clear out the gcEphemeronEdges entries and
    // rebuild the full table. Note that this a cross-zone operation; delegate
    // zone entries will be populated by map zone traversals, so everything
    // needs to be cleared first, then populated.
    if (!marker().incrementalWeakMapMarkingEnabled) {
      for (ZoneIterT zone(this); !zone.done(); zone.next()) {
        AutoEnterOOMUnsafeRegion oomUnsafe;
        if (!zone->gcEphemeronEdges().clear()) {
          oomUnsafe.crash("clearing weak keys when entering weak marking mode");
        }
      }
    }

    for (ZoneIterT zone(this); !zone.done(); zone.next()) {
      if (zone->enterWeakMarkingMode(&marker(), budget) == NotFinished) {
        return NotFinished;
      }
    }
  }

  bool markedAny = true;
  while (markedAny) {
    if (!marker().markUntilBudgetExhausted(budget)) {
      MOZ_ASSERT(marker().incrementalWeakMapMarkingEnabled);
      return NotFinished;
    }

    markedAny = false;

    if (!marker().isWeakMarking()) {
      for (ZoneIterT zone(this); !zone.done(); zone.next()) {
        markedAny |= WeakMapBase::markZoneIteratively(zone, &marker());
      }
    }

    markedAny |= jit::JitRuntime::MarkJitcodeGlobalTableIteratively(&marker());
  }

  assertNoMarkingWork();

  return Finished;
}

IncrementalProgress GCRuntime::markWeakReferencesInCurrentGroup(
    SliceBudget& budget) {
  return markWeakReferences<SweepGroupZonesIter>(budget);
}

template <class ZoneIterT>
IncrementalProgress GCRuntime::markGrayRoots(SliceBudget& budget,
                                             gcstats::PhaseKind phase) {
  MOZ_ASSERT(marker().markColor() == MarkColor::Black);

  gcstats::AutoPhase ap(stats(), phase);

  {
    AutoSetMarkColor setColorGray(marker(), MarkColor::Gray);

    AutoUpdateLiveCompartments updateLive(this);
    marker().setRootMarkingMode(true);
    auto guard = mozilla::MakeScopeExit(
        [this]() { marker().setRootMarkingMode(false); });

    IncrementalProgress result =
        traceEmbeddingGrayRoots(marker().tracer(), budget);
    if (result == NotFinished) {
      return NotFinished;
    }

    Compartment::traceIncomingCrossCompartmentEdgesForZoneGC(
        marker().tracer(), Compartment::GrayEdges);
  }

  // Also mark any incoming cross compartment edges that were originally gray
  // but have been marked black by a barrier.
  Compartment::traceIncomingCrossCompartmentEdgesForZoneGC(
      marker().tracer(), Compartment::BlackEdges);

  return Finished;
}

IncrementalProgress GCRuntime::markAllWeakReferences() {
  SliceBudget budget = SliceBudget::unlimited();
  return markWeakReferences<GCZonesIter>(budget);
}

void GCRuntime::markAllGrayReferences(gcstats::PhaseKind phase) {
  SliceBudget budget = SliceBudget::unlimited();
  markGrayRoots<GCZonesIter>(budget, phase);
  drainMarkStack();
}

void GCRuntime::dropStringWrappers() {
  /*
   * String "wrappers" are dropped on GC because their presence would require
   * us to sweep the wrappers in all compartments every time we sweep a
   * compartment group.
   */
  for (ZonesIter zone(this, WithAtoms); !zone.done(); zone.next()) {
    zone->dropStringWrappersOnGC();
  }
}

/*
 * Group zones that must be swept at the same time.
 *
 * From the point of view of the mutator, groups of zones transition atomically
 * from marking to sweeping. If compartment A has an edge to an unmarked object
 * in compartment B, then we must not start sweeping A in a later slice than we
 * start sweeping B. That's because a write barrier in A could lead to the
 * unmarked object in B becoming marked. However, if we had already swept that
 * object, we would be in trouble.
 *
 * If we consider these dependencies as a graph, then all the compartments in
 * any strongly-connected component of this graph must start sweeping in the
 * same slice.
 *
 * Tarjan's algorithm is used to calculate the components.
 */

bool Compartment::findSweepGroupEdges() {
  Zone* source = zone();
  for (WrappedObjectCompartmentEnum e(this); !e.empty(); e.popFront()) {
    Compartment* targetComp = e.front();
    Zone* target = targetComp->zone();

    if (!target->isGCMarking() || source->hasSweepGroupEdgeTo(target)) {
      continue;
    }

    for (ObjectWrapperEnum e(this, targetComp); !e.empty(); e.popFront()) {
      JSObject* key = e.front().mutableKey();
      MOZ_ASSERT(key->zone() == target);

      // Add an edge to the wrapped object's zone to ensure that the wrapper
      // zone is not still being marked when we start sweeping the wrapped zone.
      // As an optimization, if the wrapped object is already marked black there
      // is no danger of later marking and we can skip this.
      if (key->isMarkedBlack()) {
        continue;
      }

      if (!source->addSweepGroupEdgeTo(target)) {
        return false;
      }

      // We don't need to consider any more wrappers for this target
      // compartment since we already added an edge.
      break;
    }
  }

  return true;
}

bool Zone::findSweepGroupEdges(Zone* atomsZone) {
  MOZ_ASSERT_IF(this != atomsZone, !isAtomsZone());

#ifdef DEBUG
  if (FinalizationObservers* observers = finalizationObservers()) {
    observers->checkTables();
  }
#endif

  // Any zone may have a pointer to an atom in the atoms zone, and these aren't
  // in the cross compartment map.
  if (atomsZone->wasGCStarted() && !addSweepGroupEdgeTo(atomsZone)) {
    return false;
  }

  for (CompartmentsInZoneIter comp(this); !comp.done(); comp.next()) {
    if (!comp->findSweepGroupEdges()) {
      return false;
    }
  }

  return WeakMapBase::findSweepGroupEdgesForZone(this);
}

bool GCRuntime::addEdgesForMarkQueue() {
#ifdef DEBUG
  // For testing only.
  //
  // Add edges between all objects mentioned in the test mark queue, since
  // otherwise they will get marked in a different order than their sweep
  // groups. Note that this is only done at the beginning of an incremental
  // collection, so it is possible for objects to be added later that do not
  // follow the sweep group ordering. These objects will wait until their sweep
  // group comes up, or will be skipped if their sweep group is already past.
  JS::Zone* prevZone = nullptr;
  for (Value val : testMarkQueue) {
    if (!val.isObject()) {
      continue;
    }
    JSObject* obj = &val.toObject();
    JS::Zone* zone = obj->zone();
    if (!zone->isGCMarking()) {
      continue;
    }
    if (prevZone && prevZone != zone) {
      if (!prevZone->addSweepGroupEdgeTo(zone)) {
        return false;
      }
    }
    prevZone = zone;
  }
#endif
  return true;
}

bool GCRuntime::findSweepGroupEdges() {
  for (GCZonesIter zone(this); !zone.done(); zone.next()) {
    if (!zone->findSweepGroupEdges(atomsZone())) {
      return false;
    }
  }

  if (!addEdgesForMarkQueue()) {
    return false;
  }

  return DebugAPI::findSweepGroupEdges(rt);
}

void GCRuntime::groupZonesForSweeping(JS::GCReason reason) {
#ifdef DEBUG
  for (ZonesIter zone(this, WithAtoms); !zone.done(); zone.next()) {
    MOZ_ASSERT(zone->gcSweepGroupEdges().empty());
  }
#endif

  JSContext* cx = rt->mainContextFromOwnThread();
  ZoneComponentFinder finder(cx);
  if (!isIncremental || !findSweepGroupEdges()) {
    finder.useOneComponent();
  }

  // Use one component for zeal modes that yield at specific points.
  if (useZeal && zealModeControlsYieldPoint()) {
    finder.useOneComponent();
  }

  for (GCZonesIter zone(this); !zone.done(); zone.next()) {
    MOZ_ASSERT(zone->isGCMarking());
    finder.addNode(zone);
  }
  sweepGroups = finder.getResultsList();
  currentSweepGroup = sweepGroups;
  sweepGroupIndex = 1;

  for (GCZonesIter zone(this); !zone.done(); zone.next()) {
    zone->clearSweepGroupEdges();
  }

#ifdef DEBUG
  unsigned idx = sweepGroupIndex;
  for (Zone* head = currentSweepGroup; head; head = head->nextGroup()) {
    for (Zone* zone = head; zone; zone = zone->nextNodeInGroup()) {
      MOZ_ASSERT(zone->isGCMarking());
      zone->gcSweepGroupIndex = idx;
    }
    idx++;
  }

  MOZ_ASSERT_IF(!isIncremental, !currentSweepGroup->nextGroup());
  for (ZonesIter zone(this, WithAtoms); !zone.done(); zone.next()) {
    MOZ_ASSERT(zone->gcSweepGroupEdges().empty());
  }
#endif
}

void GCRuntime::getNextSweepGroup() {
  currentSweepGroup = currentSweepGroup->nextGroup();
  ++sweepGroupIndex;
  if (!currentSweepGroup) {
    abortSweepAfterCurrentGroup = false;
    return;
  }

  MOZ_ASSERT_IF(abortSweepAfterCurrentGroup, !isIncremental);
  if (!isIncremental) {
    ZoneComponentFinder::mergeGroups(currentSweepGroup);
  }

  for (Zone* zone = currentSweepGroup; zone; zone = zone->nextNodeInGroup()) {
    MOZ_ASSERT(zone->gcState() == zone->initialMarkingState());
    MOZ_ASSERT(!zone->isQueuedForBackgroundSweep());
  }

  if (abortSweepAfterCurrentGroup) {
    markTask.join();

    // Abort collection of subsequent sweep groups.
    for (SweepGroupZonesIter zone(this); !zone.done(); zone.next()) {
      MOZ_ASSERT(!zone->gcNextGraphComponent);
      zone->changeGCState(zone->initialMarkingState(), Zone::NoGC);
      zone->arenas.unmarkPreMarkedFreeCells();
      zone->arenas.mergeArenasFromCollectingLists();
      zone->clearGCSliceThresholds();
    }

    for (SweepGroupCompartmentsIter comp(rt); !comp.done(); comp.next()) {
      resetGrayList(comp);
    }

    abortSweepAfterCurrentGroup = false;
    currentSweepGroup = nullptr;
  }
}

/*
 * Gray marking:
 *
 * At the end of collection, anything reachable from a gray root that has not
 * otherwise been marked black must be marked gray.
 *
 * This means that when marking things gray we must not allow marking to leave
 * the current compartment group, as that could result in things being marked
 * gray when they might subsequently be marked black.  To achieve this, when we
 * find a cross compartment pointer we don't mark the referent but add it to a
 * singly-linked list of incoming gray pointers that is stored with each
 * compartment.
 *
 * The list head is stored in Compartment::gcIncomingGrayPointers and contains
 * cross compartment wrapper objects. The next pointer is stored in the second
 * extra slot of the cross compartment wrapper.
 *
 * The list is created during gray marking when one of the
 * MarkCrossCompartmentXXX functions is called for a pointer that leaves the
 * current compartent group.  This calls DelayCrossCompartmentGrayMarking to
 * push the referring object onto the list.
 *
 * The list is traversed and then unlinked in
 * GCRuntime::markIncomingGrayCrossCompartmentPointers.
 */

static bool IsGrayListObject(JSObject* obj) {
  MOZ_ASSERT(obj);
  return obj->is<CrossCompartmentWrapperObject>() && !IsDeadProxyObject(obj);
}

/* static */
unsigned ProxyObject::grayLinkReservedSlot(JSObject* obj) {
  MOZ_ASSERT(IsGrayListObject(obj));
  return CrossCompartmentWrapperObject::GrayLinkReservedSlot;
}

#ifdef DEBUG
static void AssertNotOnGrayList(JSObject* obj) {
  MOZ_ASSERT_IF(
      IsGrayListObject(obj),
      GetProxyReservedSlot(obj, ProxyObject::grayLinkReservedSlot(obj))
          .isUndefined());
}
#endif

static void AssertNoWrappersInGrayList(JSRuntime* rt) {
#ifdef DEBUG
  for (CompartmentsIter c(rt); !c.done(); c.next()) {
    MOZ_ASSERT(!c->gcIncomingGrayPointers);
    for (Compartment::ObjectWrapperEnum e(c); !e.empty(); e.popFront()) {
      AssertNotOnGrayList(e.front().value().unbarrieredGet());
    }
  }
#endif
}

static JSObject* CrossCompartmentPointerReferent(JSObject* obj) {
  MOZ_ASSERT(IsGrayListObject(obj));
  return &obj->as<ProxyObject>().private_().toObject();
}

static JSObject* NextIncomingCrossCompartmentPointer(JSObject* prev,
                                                     bool unlink) {
  unsigned slot = ProxyObject::grayLinkReservedSlot(prev);
  JSObject* next = GetProxyReservedSlot(prev, slot).toObjectOrNull();
  MOZ_ASSERT_IF(next, IsGrayListObject(next));

  if (unlink) {
    SetProxyReservedSlot(prev, slot, UndefinedValue());
  }

  return next;
}

void js::gc::DelayCrossCompartmentGrayMarking(GCMarker* maybeMarker,
                                              JSObject* src) {
  MOZ_ASSERT_IF(!maybeMarker, !JS::RuntimeHeapIsBusy());
  MOZ_ASSERT(IsGrayListObject(src));
  MOZ_ASSERT(src->isMarkedGray());

  AutoTouchingGrayThings tgt;

  mozilla::Maybe<AutoLockGC> lock;
  if (maybeMarker && maybeMarker->isParallelMarking()) {
    // Synchronize access to JSCompartment::gcIncomingGrayPointers.
    //
    // TODO: Instead of building this list we could scan all incoming CCWs and
    // mark through gray ones when marking gray roots for a sweep group.
    lock.emplace(maybeMarker->runtime());
  }

  /* Called from MarkCrossCompartmentXXX functions. */
  unsigned slot = ProxyObject::grayLinkReservedSlot(src);
  JSObject* dest = CrossCompartmentPointerReferent(src);
  Compartment* comp = dest->compartment();

  if (GetProxyReservedSlot(src, slot).isUndefined()) {
    SetProxyReservedSlot(src, slot,
                         ObjectOrNullValue(comp->gcIncomingGrayPointers));
    comp->gcIncomingGrayPointers = src;
  } else {
    MOZ_ASSERT(GetProxyReservedSlot(src, slot).isObjectOrNull());
  }

#ifdef DEBUG
  /*
   * Assert that the object is in our list, also walking the list to check its
   * integrity.
   */
  JSObject* obj = comp->gcIncomingGrayPointers;
  bool found = false;
  while (obj) {
    if (obj == src) {
      found = true;
    }
    obj = NextIncomingCrossCompartmentPointer(obj, false);
  }
  MOZ_ASSERT(found);
#endif
}

void GCRuntime::markIncomingGrayCrossCompartmentPointers() {
  gcstats::AutoPhase ap(stats(), gcstats::PhaseKind::MARK_INCOMING_GRAY);

  for (SweepGroupCompartmentsIter c(rt); !c.done(); c.next()) {
    MOZ_ASSERT(c->zone()->isGCMarkingBlackAndGray());
    MOZ_ASSERT_IF(c->gcIncomingGrayPointers,
                  IsGrayListObject(c->gcIncomingGrayPointers));

    for (JSObject* src = c->gcIncomingGrayPointers; src;
         src = NextIncomingCrossCompartmentPointer(src, true)) {
      JSObject* dst = CrossCompartmentPointerReferent(src);
      MOZ_ASSERT(dst->compartment() == c);
      MOZ_ASSERT_IF(src->asTenured().isMarkedBlack(),
                    dst->asTenured().isMarkedBlack());

      if (src->asTenured().isMarkedGray()) {
        TraceManuallyBarrieredEdge(marker().tracer(), &dst,
                                   "cross-compartment gray pointer");
      }
    }

    c->gcIncomingGrayPointers = nullptr;
  }
}

static bool RemoveFromGrayList(JSObject* wrapper) {
  AutoTouchingGrayThings tgt;

  if (!IsGrayListObject(wrapper)) {
    return false;
  }

  unsigned slot = ProxyObject::grayLinkReservedSlot(wrapper);
  if (GetProxyReservedSlot(wrapper, slot).isUndefined()) {
    return false; /* Not on our list. */
  }

  JSObject* tail = GetProxyReservedSlot(wrapper, slot).toObjectOrNull();
  SetProxyReservedSlot(wrapper, slot, UndefinedValue());

  Compartment* comp = CrossCompartmentPointerReferent(wrapper)->compartment();
  JSObject* obj = comp->gcIncomingGrayPointers;
  if (obj == wrapper) {
    comp->gcIncomingGrayPointers = tail;
    return true;
  }

  while (obj) {
    unsigned slot = ProxyObject::grayLinkReservedSlot(obj);
    JSObject* next = GetProxyReservedSlot(obj, slot).toObjectOrNull();
    if (next == wrapper) {
      js::detail::SetProxyReservedSlotUnchecked(obj, slot,
                                                ObjectOrNullValue(tail));
      return true;
    }
    obj = next;
  }

  MOZ_CRASH("object not found in gray link list");
}

void GCRuntime::resetGrayList(Compartment* comp) {
  JSObject* src = comp->gcIncomingGrayPointers;
  while (src) {
    src = NextIncomingCrossCompartmentPointer(src, true);
  }
  comp->gcIncomingGrayPointers = nullptr;
}

#ifdef DEBUG
static bool HasIncomingCrossCompartmentPointers(JSRuntime* rt) {
  for (SweepGroupCompartmentsIter c(rt); !c.done(); c.next()) {
    if (c->gcIncomingGrayPointers) {
      return true;
    }
  }

  return false;
}
#endif

void js::NotifyGCNukeWrapper(JSContext* cx, JSObject* wrapper) {
  MOZ_ASSERT(IsCrossCompartmentWrapper(wrapper));

  /*
   * References to target of wrapper are being removed, we no longer have to
   * remember to mark it.
   */
  RemoveFromGrayList(wrapper);

  /*
   * Clean up WeakRef maps which might include this wrapper.
   */
  JSObject* target = UncheckedUnwrapWithoutExpose(wrapper);
  if (target->is<WeakRefObject>()) {
    WeakRefObject* weakRef = &target->as<WeakRefObject>();
    if (weakRef->target()) {
      cx->runtime()->gc.nukeWeakRefWrapper(wrapper, weakRef);
    }
  }

  /*
   * Clean up FinalizationRecord record objects which might be the target of
   * this wrapper.
   */
  if (target->is<FinalizationRecordObject>()) {
    auto* record = &target->as<FinalizationRecordObject>();
    cx->runtime()->gc.nukeFinalizationRecordWrapper(wrapper, record);
  }
}

enum {
  JS_GC_SWAP_OBJECT_A_REMOVED = 1 << 0,
  JS_GC_SWAP_OBJECT_B_REMOVED = 1 << 1
};

unsigned js::NotifyGCPreSwap(JSObject* a, JSObject* b) {
  /*
   * Two objects in the same compartment are about to have had their contents
   * swapped.  If either of them are in our gray pointer list, then we remove
   * them from the lists, returning a bitset indicating what happened.
   */
  return (RemoveFromGrayList(a) ? JS_GC_SWAP_OBJECT_A_REMOVED : 0) |
         (RemoveFromGrayList(b) ? JS_GC_SWAP_OBJECT_B_REMOVED : 0);
}

void js::NotifyGCPostSwap(JSObject* a, JSObject* b, unsigned removedFlags) {
  /*
   * Two objects in the same compartment have had their contents swapped.  If
   * either of them were in our gray pointer list, we re-add them again.
   */
  if (removedFlags & JS_GC_SWAP_OBJECT_A_REMOVED) {
    DelayCrossCompartmentGrayMarking(nullptr, b);
  }
  if (removedFlags & JS_GC_SWAP_OBJECT_B_REMOVED) {
    DelayCrossCompartmentGrayMarking(nullptr, a);
  }
}

static inline void MaybeCheckWeakMapMarking(GCRuntime* gc) {
#if defined(JS_GC_ZEAL) || defined(DEBUG)

  bool shouldCheck;
#  if defined(DEBUG)
  shouldCheck = true;
#  else
  shouldCheck = gc->hasZealMode(ZealMode::CheckWeakMapMarking);
#  endif

  if (shouldCheck) {
    for (SweepGroupZonesIter zone(gc); !zone.done(); zone.next()) {
      MOZ_RELEASE_ASSERT(WeakMapBase::checkMarkingForZone(zone));
    }
  }

#endif
}

IncrementalProgress GCRuntime::beginMarkingSweepGroup(JS::GCContext* gcx,
                                                      SliceBudget& budget) {
#ifdef DEBUG
  MOZ_ASSERT(!markOnBackgroundThreadDuringSweeping);
  assertNoMarkingWork();
  for (auto& marker : markers) {
    MOZ_ASSERT(marker->markColor() == MarkColor::Black);
  }
  MOZ_ASSERT(cellsToAssertNotGray.ref().empty());
#endif

  gcstats::AutoPhase ap(stats(), gcstats::PhaseKind::MARK);

  // Change state of current group to MarkBlackAndGray to restrict gray marking
  // to this group. Note that there may be pointers to the atoms zone, and these
  // will be marked through, as they are not marked with
  // TraceCrossCompartmentEdge.
  for (SweepGroupZonesIter zone(this); !zone.done(); zone.next()) {
    zone->changeGCState(zone->initialMarkingState(), Zone::MarkBlackAndGray);
  }

  AutoSetMarkColor setColorGray(marker(), MarkColor::Gray);

  // Mark incoming gray pointers from previously swept compartments.
  markIncomingGrayCrossCompartmentPointers();

  return Finished;
}

IncrementalProgress GCRuntime::markGrayRootsInCurrentGroup(
    JS::GCContext* gcx, SliceBudget& budget) {
  gcstats::AutoPhase ap(stats(), gcstats::PhaseKind::MARK);

  return markGrayRoots<SweepGroupZonesIter>(budget,
                                            gcstats::PhaseKind::MARK_GRAY);
}

IncrementalProgress GCRuntime::markGray(JS::GCContext* gcx,
                                        SliceBudget& budget) {
  gcstats::AutoPhase ap(stats(), gcstats::PhaseKind::MARK);

  if (markUntilBudgetExhausted(budget, useParallelMarking) == NotFinished) {
    return NotFinished;
  }

  return Finished;
}

IncrementalProgress GCRuntime::endMarkingSweepGroup(JS::GCContext* gcx,
                                                    SliceBudget& budget) {
#ifdef DEBUG
  MOZ_ASSERT(!markOnBackgroundThreadDuringSweeping);
  assertNoMarkingWork();
  for (auto& marker : markers) {
    MOZ_ASSERT(marker->markColor() == MarkColor::Black);
  }
  MOZ_ASSERT(!HasIncomingCrossCompartmentPointers(rt));
#endif

  gcstats::AutoPhase ap(stats(), gcstats::PhaseKind::MARK);

  if (markWeakReferencesInCurrentGroup(budget) == NotFinished) {
    return NotFinished;
  }

  AutoSetMarkColor setColorGray(marker(), MarkColor::Gray);

  // Mark transitively inside the current compartment group.
  if (markWeakReferencesInCurrentGroup(budget) == NotFinished) {
    return NotFinished;
  }

  MOZ_ASSERT(marker().isDrained());

  // We must not yield after this point before we start sweeping the group.
  safeToYield = false;

  MaybeCheckWeakMapMarking(this);

  return Finished;
}

// Causes the given WeakCache to be swept when run.
class ImmediateSweepWeakCacheTask : public GCParallelTask {
  Zone* zone;
  JS::detail::WeakCacheBase& cache;

 public:
  ImmediateSweepWeakCacheTask(GCRuntime* gc, Zone* zone,
                              JS::detail::WeakCacheBase& wc)
      : GCParallelTask(gc, gcstats::PhaseKind::SWEEP_WEAK_CACHES),
        zone(zone),
        cache(wc) {}

  ImmediateSweepWeakCacheTask(ImmediateSweepWeakCacheTask&& other) noexcept
      : GCParallelTask(std::move(other)),
        zone(other.zone),
        cache(other.cache) {}

  ImmediateSweepWeakCacheTask(const ImmediateSweepWeakCacheTask&) = delete;

  void run(AutoLockHelperThreadState& lock) override {
    AutoUnlockHelperThreadState unlock(lock);
    AutoSetThreadIsSweeping threadIsSweeping(zone);
    SweepingTracer trc(gc->rt);
    cache.traceWeak(&trc, JS::detail::WeakCacheBase::LockStoreBuffer);
  }
};

void GCRuntime::updateAtomsBitmap() {
  size_t collectedZones = 0;
  size_t uncollectedZones = 0;
  for (ZonesIter zone(this, SkipAtoms); !zone.done(); zone.next()) {
    if (zone->isCollecting()) {
      collectedZones++;
    } else {
      uncollectedZones++;
    }
  }

  atomMarking.refineZoneBitmapsForCollectedZones(this, collectedZones);

  atomMarking.markAtomsUsedByUncollectedZones(this, uncollectedZones);

  // For convenience sweep these tables non-incrementally as part of bitmap
  // sweeping; they are likely to be much smaller than the main atoms table.
  SweepingTracer trc(rt);
  rt->symbolRegistry().traceWeak(&trc);
}

void GCRuntime::sweepCCWrappers() {
  SweepingTracer trc(rt);
  for (SweepGroupZonesIter zone(this); !zone.done(); zone.next()) {
    zone->traceWeakCCWEdges(&trc);
  }
}

void GCRuntime::sweepRealmGlobals() {
  SweepingTracer trc(rt);
  for (SweepGroupRealmsIter r(this); !r.done(); r.next()) {
    AutoSetThreadIsSweeping threadIsSweeping(r->zone());
    r->traceWeakGlobalEdge(&trc);
  }
}

void GCRuntime::sweepMisc() {
  SweepingTracer trc(rt);
  for (SweepGroupRealmsIter r(this); !r.done(); r.next()) {
    AutoSetThreadIsSweeping threadIsSweeping(r->zone());
    r->traceWeakSavedStacks(&trc);
  }
  for (SweepGroupCompartmentsIter c(this); !c.done(); c.next()) {
    AutoSetThreadIsSweeping threadIsSweeping(c->zone());
    c->traceWeakNativeIterators(&trc);
  }
}

void GCRuntime::sweepCompressionTasks() {
  JSRuntime* runtime = rt;

  // Attach finished compression tasks.
  AutoLockHelperThreadState lock;
  AttachFinishedCompressions(runtime, lock);
  SweepPendingCompressions(lock);
}

void GCRuntime::sweepWeakMaps() {
  SweepingTracer trc(rt);
  for (SweepGroupZonesIter zone(this); !zone.done(); zone.next()) {
    /* No need to look up any more weakmap keys from this sweep group. */
    AutoEnterOOMUnsafeRegion oomUnsafe;
    if (!zone->gcEphemeronEdges().clear()) {
      oomUnsafe.crash("clearing weak keys in beginSweepingSweepGroup()");
    }

    // Lock the storebuffer since this may access it when rehashing or resizing
    // the tables.
    AutoLockStoreBuffer lock(rt);
    zone->sweepWeakMaps(&trc);
  }
}

void GCRuntime::sweepUniqueIds() {
  for (SweepGroupZonesIter zone(this); !zone.done(); zone.next()) {
    AutoSetThreadIsSweeping threadIsSweeping(zone);
    zone->sweepUniqueIds();
  }
}

void JS::Zone::sweepUniqueIds() {
  SweepingTracer trc(runtimeFromAnyThread());
  uniqueIds().traceWeak(&trc);
}

/* static */
bool UniqueIdGCPolicy::traceWeak(JSTracer* trc, Cell** keyp, uint64_t* valuep) {
  // Since this is only ever used for sweeping, we can optimize it for that
  // case. (Compacting GC updates this table manually when it moves a cell.)
  MOZ_ASSERT(trc->kind() == JS::TracerKind::Sweeping);
  return (*keyp)->isMarkedAny();
}

void GCRuntime::sweepFinalizationObserversOnMainThread() {
  // This calls back into the browser which expects to be called from the main
  // thread.
  gcstats::AutoPhase ap1(stats(), gcstats::PhaseKind::SWEEP_COMPARTMENTS);
  gcstats::AutoPhase ap2(stats(),
                         gcstats::PhaseKind::SWEEP_FINALIZATION_OBSERVERS);
  SweepingTracer trc(rt);
  AutoLockStoreBuffer lock(rt);
  for (SweepGroupZonesIter zone(this); !zone.done(); zone.next()) {
    traceWeakFinalizationObserverEdges(&trc, zone);
  }
}

void GCRuntime::startTask(GCParallelTask& task,
                          AutoLockHelperThreadState& lock) {
  if (!CanUseExtraThreads()) {
    AutoUnlockHelperThreadState unlock(lock);
    task.runFromMainThread();
    stats().recordParallelPhase(task.phaseKind, task.duration());
    return;
  }

  task.startWithLockHeld(lock);
}

void GCRuntime::joinTask(GCParallelTask& task,
                         AutoLockHelperThreadState& lock) {
  gcstats::AutoPhase ap(stats(), gcstats::PhaseKind::JOIN_PARALLEL_TASKS);
  task.joinWithLockHeld(lock);
}

void GCRuntime::sweepDebuggerOnMainThread(JS::GCContext* gcx) {
  SweepingTracer trc(rt);
  AutoLockStoreBuffer lock(rt);

  // Detach unreachable debuggers and global objects from each other.
  // This can modify weakmaps and so must happen before weakmap sweeping.
  DebugAPI::sweepAll(gcx);

  gcstats::AutoPhase ap(stats(), gcstats::PhaseKind::SWEEP_COMPARTMENTS);

  // Sweep debug environment information. This performs lookups in the Zone's
  // unique IDs table and so must not happen in parallel with sweeping that
  // table.
  {
    gcstats::AutoPhase ap2(stats(), gcstats::PhaseKind::SWEEP_MISC);
    for (SweepGroupRealmsIter r(rt); !r.done(); r.next()) {
      r->traceWeakDebugEnvironmentEdges(&trc);
    }
  }
}

void GCRuntime::sweepJitDataOnMainThread(JS::GCContext* gcx) {
  SweepingTracer trc(rt);
  {
    gcstats::AutoPhase ap(stats(), gcstats::PhaseKind::SWEEP_JIT_DATA);

    // Bug 1071218: the following method has not yet been refactored to
    // work on a single zone-group at once.

    // Sweep entries containing about-to-be-finalized JitCode in the
    // JitcodeGlobalTable.
    jit::JitRuntime::TraceWeakJitcodeGlobalTable(rt, &trc);
  }

  // Discard JIT code and trace weak edges in JitScripts to remove edges to
  // dying GC things. The latter is carried out as part of discardJitCode if
  // possible to avoid iterating all scripts in the zone twice.
  {
    gcstats::AutoPhase apdc(stats(), gcstats::PhaseKind::SWEEP_DISCARD_CODE);
    Zone::DiscardOptions options;
    options.traceWeakJitScripts = &trc;
    for (SweepGroupZonesIter zone(this); !zone.done(); zone.next()) {
      if (!haveDiscardedJITCodeThisSlice && !zone->isPreservingCode()) {
        zone->forceDiscardJitCode(gcx, options);
      } else {
        zone->traceWeakJitScripts(&trc);
      }
    }
  }

  // JitZone must be swept *after* discarding JIT code, because
  // Zone::discardJitCode might access CacheIRStubInfos deleted here.
  {
    gcstats::AutoPhase ap(stats(), gcstats::PhaseKind::SWEEP_JIT_DATA);

    for (SweepGroupZonesIter zone(this); !zone.done(); zone.next()) {
      if (jit::JitZone* jitZone = zone->jitZone()) {
        jitZone->traceWeak(&trc, zone);
      }
    }

    JSContext* cx = rt->mainContextFromOwnThread();
    jit::TraceWeakJitActivationsInSweepingZones(cx, &trc);
  }
}

void GCRuntime::sweepObjectsWithWeakPointers() {
  SweepingTracer trc(rt);
  for (SweepGroupZonesIter zone(this); !zone.done(); zone.next()) {
    AutoSetThreadIsSweeping threadIsSweeping(zone);
    zone->sweepObjectsWithWeakPointers(&trc);
  }
}

void JS::Zone::sweepObjectsWithWeakPointers(JSTracer* trc) {
  MOZ_ASSERT(trc->traceWeakEdges());

  objectsWithWeakPointers.ref().mutableEraseIf([&](JSObject*& obj) {
    if (!TraceManuallyBarrieredWeakEdge(trc, &obj, "objectsWithWeakPointers")) {
      // Object itself is dead.
      return true;
    }

    // Call trace hook to sweep weak pointers.
    obj->getClass()->doTrace(trc, obj);
    return false;
  });
}

using WeakCacheTaskVector =
    mozilla::Vector<ImmediateSweepWeakCacheTask, 0, SystemAllocPolicy>;

// Call a functor for all weak caches that need to be swept in the current
// sweep group.
template <typename Functor>
static inline bool IterateWeakCaches(JSRuntime* rt, Functor f) {
  for (SweepGroupZonesIter zone(rt); !zone.done(); zone.next()) {
    for (JS::detail::WeakCacheBase* cache : zone->weakCaches()) {
      if (!f(cache, zone.get())) {
        return false;
      }
    }
  }

  for (JS::detail::WeakCacheBase* cache : rt->weakCaches()) {
    if (!f(cache, nullptr)) {
      return false;
    }
  }

  return true;
}

static bool PrepareWeakCacheTasks(JSRuntime* rt,
                                  WeakCacheTaskVector* immediateTasks) {
  // Start incremental sweeping for caches that support it or add to a vector
  // of sweep tasks to run on a helper thread.

  MOZ_ASSERT(immediateTasks->empty());

  GCRuntime* gc = &rt->gc;
  bool ok =
      IterateWeakCaches(rt, [&](JS::detail::WeakCacheBase* cache, Zone* zone) {
        if (cache->empty()) {
          return true;
        }

        // Caches that support incremental sweeping will be swept later.
        if (zone && cache->setIncrementalBarrierTracer(&gc->sweepingTracer)) {
          return true;
        }

        return immediateTasks->emplaceBack(gc, zone, *cache);
      });

  if (!ok) {
    immediateTasks->clearAndFree();
  }

  return ok;
}

static void SweepAllWeakCachesOnMainThread(JSRuntime* rt) {
  // If we ran out of memory, do all the work on the main thread.
  gcstats::AutoPhase ap(rt->gc.stats(), gcstats::PhaseKind::SWEEP_WEAK_CACHES);
  SweepingTracer trc(rt);
  IterateWeakCaches(rt, [&](JS::detail::WeakCacheBase* cache, Zone* zone) {
    if (cache->needsIncrementalBarrier()) {
      cache->setIncrementalBarrierTracer(nullptr);
    }
    cache->traceWeak(&trc, JS::detail::WeakCacheBase::LockStoreBuffer);
    return true;
  });
}

void GCRuntime::sweepEmbeddingWeakPointers(JS::GCContext* gcx) {
  using namespace gcstats;

  AutoLockStoreBuffer lock(rt);

  AutoPhase ap(stats(), PhaseKind::FINALIZE_START);
  callFinalizeCallbacks(gcx, JSFINALIZE_GROUP_PREPARE);
  {
    AutoPhase ap2(stats(), PhaseKind::WEAK_ZONES_CALLBACK);
    callWeakPointerZonesCallbacks(&sweepingTracer);
  }
  {
    AutoPhase ap2(stats(), PhaseKind::WEAK_COMPARTMENT_CALLBACK);
    for (SweepGroupZonesIter zone(this); !zone.done(); zone.next()) {
      for (CompartmentsInZoneIter comp(zone); !comp.done(); comp.next()) {
        callWeakPointerCompartmentCallbacks(&sweepingTracer, comp);
      }
    }
  }
  callFinalizeCallbacks(gcx, JSFINALIZE_GROUP_START);
}

IncrementalProgress GCRuntime::beginSweepingSweepGroup(JS::GCContext* gcx,
                                                       SliceBudget& budget) {
  /*
   * Begin sweeping the group of zones in currentSweepGroup, performing
   * actions that must be done before yielding to caller.
   */

  using namespace gcstats;

  AutoSCC scc(stats(), sweepGroupIndex);

  bool sweepingAtoms = false;
  for (SweepGroupZonesIter zone(this); !zone.done(); zone.next()) {
    /* Set the GC state to sweeping. */
    zone->changeGCState(Zone::MarkBlackAndGray, Zone::Sweep);

    /* Purge the ArenaLists before sweeping. */
    zone->arenas.checkSweepStateNotInUse();
    zone->arenas.unmarkPreMarkedFreeCells();
    zone->arenas.clearFreeLists();

    if (zone->isAtomsZone()) {
      sweepingAtoms = true;
    }
  }

#ifdef DEBUG
  for (const auto* cell : cellsToAssertNotGray.ref()) {
    JS::AssertCellIsNotGray(cell);
  }
  cellsToAssertNotGray.ref().clearAndFree();
#endif

  // Cancel off thread compilation as soon as possible, unless this already
  // happened in GCRuntime::discardJITCodeForGC.
  if (!haveDiscardedJITCodeThisSlice) {
    js::CancelOffThreadIonCompile(rt, JS::Zone::Sweep);
  }

  // Updating the atom marking bitmaps. This marks atoms referenced by
  // uncollected zones so cannot be done in parallel with the other sweeping
  // work below.
  if (sweepingAtoms) {
    AutoPhase ap(stats(), PhaseKind::UPDATE_ATOMS_BITMAP);
    updateAtomsBitmap();
  }

#ifdef JS_GC_ZEAL
  validateIncrementalMarking();
#endif

  AutoSetThreadIsSweeping threadIsSweeping;

  // This must happen before sweeping realm globals.
  sweepDebuggerOnMainThread(gcx);

  // FinalizationRegistry sweeping touches weak maps and so must not run in
  // parallel with that. This triggers a read barrier and can add marking work
  // for zones that are still marking. Must happen before sweeping realm
  // globals.
  sweepFinalizationObserversOnMainThread();

  // This must happen before updating embedding weak pointers.
  sweepRealmGlobals();

  sweepEmbeddingWeakPointers(gcx);

  {
    AutoLockHelperThreadState lock;

    AutoPhase ap(stats(), PhaseKind::SWEEP_COMPARTMENTS);

    AutoRunParallelTask sweepCCWrappers(this, &GCRuntime::sweepCCWrappers,
                                        PhaseKind::SWEEP_CC_WRAPPER,
                                        GCUse::Sweeping, lock);
    AutoRunParallelTask sweepMisc(this, &GCRuntime::sweepMisc,
                                  PhaseKind::SWEEP_MISC, GCUse::Sweeping, lock);
    AutoRunParallelTask sweepCompTasks(this, &GCRuntime::sweepCompressionTasks,
                                       PhaseKind::SWEEP_COMPRESSION,
                                       GCUse::Sweeping, lock);
    AutoRunParallelTask sweepWeakMaps(this, &GCRuntime::sweepWeakMaps,
                                      PhaseKind::SWEEP_WEAKMAPS,
                                      GCUse::Sweeping, lock);
    AutoRunParallelTask sweepUniqueIds(this, &GCRuntime::sweepUniqueIds,
                                       PhaseKind::SWEEP_UNIQUEIDS,
                                       GCUse::Sweeping, lock);
    AutoRunParallelTask sweepWeakPointers(
        this, &GCRuntime::sweepObjectsWithWeakPointers,
        PhaseKind::SWEEP_WEAK_POINTERS, GCUse::Sweeping, lock);

    WeakCacheTaskVector sweepCacheTasks;
    bool canSweepWeakCachesOffThread =
        PrepareWeakCacheTasks(rt, &sweepCacheTasks);
    if (canSweepWeakCachesOffThread) {
      weakCachesToSweep.ref().emplace(currentSweepGroup);
      for (auto& task : sweepCacheTasks) {
        startTask(task, lock);
      }
    }

    {
      AutoUnlockHelperThreadState unlock(lock);
      sweepJitDataOnMainThread(gcx);

      if (!canSweepWeakCachesOffThread) {
        MOZ_ASSERT(sweepCacheTasks.empty());
        SweepAllWeakCachesOnMainThread(rt);
      }
    }

    for (auto& task : sweepCacheTasks) {
      joinTask(task, lock);
    }
  }

  if (sweepingAtoms) {
    startSweepingAtomsTable();
  }

  // Queue all GC things in all zones for sweeping, either on the foreground
  // or on the background thread.

  for (SweepGroupZonesIter zone(this); !zone.done(); zone.next()) {
    for (const auto& phase : BackgroundFinalizePhases) {
      initBackgroundSweep(zone, gcx, phase);
    }

    zone->arenas.queueForegroundThingsForSweep();
  }

  MOZ_ASSERT(!sweepZone);

  safeToYield = true;
  markOnBackgroundThreadDuringSweeping = CanUseExtraThreads();

  return Finished;
}

#ifdef JS_GC_ZEAL
bool GCRuntime::shouldYieldForZeal(ZealMode mode) {
  bool yield = useZeal && hasZealMode(mode);

  // Only yield on the first sweep slice for this mode.
  bool firstSweepSlice = initialState != State::Sweep;
  if (mode == ZealMode::IncrementalMultipleSlices && !firstSweepSlice) {
    yield = false;
  }

  return yield;
}
#endif

IncrementalProgress GCRuntime::endSweepingSweepGroup(JS::GCContext* gcx,
                                                     SliceBudget& budget) {
  // This is to prevent a race between markTask checking the zone state and
  // us changing it below.
  if (joinBackgroundMarkTask() == NotFinished) {
    return NotFinished;
  }

  assertNoMarkingWork();

  // Disable background marking during sweeping until we start sweeping the next
  // zone group.
  markOnBackgroundThreadDuringSweeping = false;

  {
    gcstats::AutoPhase ap(stats(), gcstats::PhaseKind::FINALIZE_END);
    AutoLockStoreBuffer lock(rt);
    callFinalizeCallbacks(gcx, JSFINALIZE_GROUP_END);
  }

  /* Free LIFO blocks on a background thread if possible. */
  startBackgroundFree();

  /* Update the GC state for zones we have swept. */
  for (SweepGroupZonesIter zone(this); !zone.done(); zone.next()) {
    if (jit::JitZone* jitZone = zone->jitZone()) {
      // Clear out any small pools that we're hanging on to.
      jitZone->execAlloc().purge();
    }
    AutoLockGC lock(this);
    zone->changeGCState(Zone::Sweep, Zone::Finished);
    zone->arenas.unmarkPreMarkedFreeCells();
    zone->arenas.checkNoArenasToUpdate();
    zone->pretenuring.clearCellCountsInNewlyCreatedArenas();
  }

  /*
   * Start background thread to sweep zones if required, sweeping any atoms
   * zones last if present.
   */
  ZoneList zones;
  for (SweepGroupZonesIter zone(this); !zone.done(); zone.next()) {
    if (zone->isAtomsZone()) {
      zones.append(zone);
    } else {
      zones.prepend(zone);
    }
  }

  queueZonesAndStartBackgroundSweep(std::move(zones));

  return Finished;
}

IncrementalProgress GCRuntime::markDuringSweeping(JS::GCContext* gcx,
                                                  SliceBudget& budget) {
  MOZ_ASSERT(markTask.isIdle());

  if (markOnBackgroundThreadDuringSweeping) {
    if (!marker().isDrained() || hasDelayedMarking()) {
      AutoLockHelperThreadState lock;
      MOZ_ASSERT(markTask.isIdle(lock));
      markTask.setBudget(budget);
      markTask.startOrRunIfIdle(lock);
    }
    return Finished;  // This means don't yield to the mutator here.
  }

  gcstats::AutoPhase ap(stats(), gcstats::PhaseKind::MARK);
  return markUntilBudgetExhausted(budget, useParallelMarking);
}

void GCRuntime::beginSweepPhase(JS::GCReason reason, AutoGCSession& session) {
  /*
   * Sweep phase.
   *
   * Finalize as we sweep, outside of lock but with RuntimeHeapIsBusy()
   * true so that any attempt to allocate a GC-thing from a finalizer will
   * fail, rather than nest badly and leave the unmarked newborn to be swept.
   */

  MOZ_ASSERT(!abortSweepAfterCurrentGroup);
  MOZ_ASSERT(!markOnBackgroundThreadDuringSweeping);

#ifdef DEBUG
  releaseHeldRelocatedArenas();
  verifyAllChunks();
#endif

#ifdef JS_GC_ZEAL
  computeNonIncrementalMarkingForValidation(session);
#endif

  gcstats::AutoPhase ap(stats(), gcstats::PhaseKind::SWEEP);

  AssertNoWrappersInGrayList(rt);
  dropStringWrappers();

  groupZonesForSweeping(reason);

  sweepActions->assertFinished();
}

bool GCRuntime::foregroundFinalize(JS::GCContext* gcx, Zone* zone,
                                   AllocKind thingKind,
                                   SliceBudget& sliceBudget,
                                   SortedArenaList& sweepList) {
  ArenaLists& lists = zone->arenas;
  lists.checkNoArenasToUpdateForKind(thingKind);

  // Non-empty arenas are reused for use for new allocations as soon as the
  // finalizers for that allocation kind have run. Empty arenas are only
  // released when everything in the zone has been swept (see
  // GCRuntime::sweepBackgroundThings for more details).
  if (!FinalizeArenas(gcx, lists.collectingArenaList(thingKind), sweepList,
                      thingKind, sliceBudget)) {
    return false;
  }

  sweepList.extractEmptyTo(&lists.savedEmptyArenas.ref());
  lists.mergeFinalizedArenas(thingKind, sweepList);

  return true;
}

BackgroundMarkTask::BackgroundMarkTask(GCRuntime* gc)
    : GCParallelTask(gc, gcstats::PhaseKind::MARK, GCUse::Marking),
      budget(SliceBudget::unlimited()) {}

void js::gc::BackgroundMarkTask::run(AutoLockHelperThreadState& lock) {
  AutoUnlockHelperThreadState unlock(lock);

  // Time reporting is handled separately for parallel tasks.
  gc->sweepMarkResult = gc->markUntilBudgetExhausted(
      this->budget, GCRuntime::SingleThreadedMarking, DontReportMarkTime);
}

IncrementalProgress GCRuntime::joinBackgroundMarkTask() {
  AutoLockHelperThreadState lock;
  if (markTask.isIdle(lock)) {
    return Finished;
  }

  joinTask(markTask, lock);

  IncrementalProgress result = sweepMarkResult;
  sweepMarkResult = Finished;
  return result;
}

template <typename T>
static void SweepThing(JS::GCContext* gcx, T* thing) {
  if (!TenuredThingIsMarkedAny(thing)) {
    thing->sweep(gcx);
  }
}

template <typename T>
static bool SweepArenaList(JS::GCContext* gcx, Arena** arenasToSweep,
                           SliceBudget& sliceBudget) {
  while (Arena* arena = *arenasToSweep) {
    MOZ_ASSERT(arena->zone->isGCSweeping());

    for (ArenaCellIterUnderGC cell(arena); !cell.done(); cell.next()) {
      SweepThing(gcx, cell.as<T>());
    }

    Arena* next = arena->next;
    MOZ_ASSERT_IF(next, next->zone == arena->zone);
    *arenasToSweep = next;

    AllocKind kind = MapTypeToAllocKind<T>::kind;
    sliceBudget.step(Arena::thingsPerArena(kind));
    if (sliceBudget.isOverBudget()) {
      return false;
    }
  }

  return true;
}

void GCRuntime::startSweepingAtomsTable() {
  auto& maybeAtoms = maybeAtomsToSweep.ref();
  MOZ_ASSERT(maybeAtoms.isNothing());

  AtomsTable* atomsTable = rt->atomsForSweeping();
  if (!atomsTable) {
    return;
  }

  // Create secondary tables to hold new atoms added while we're sweeping the
  // main tables incrementally.
  if (!atomsTable->startIncrementalSweep(maybeAtoms)) {
    SweepingTracer trc(rt);
    atomsTable->traceWeak(&trc);
  }
}

IncrementalProgress GCRuntime::sweepAtomsTable(JS::GCContext* gcx,
                                               SliceBudget& budget) {
  if (!atomsZone()->isGCSweeping()) {
    return Finished;
  }

  gcstats::AutoPhase ap(stats(), gcstats::PhaseKind::SWEEP_ATOMS_TABLE);

  auto& maybeAtoms = maybeAtomsToSweep.ref();
  if (!maybeAtoms) {
    return Finished;
  }

  if (!rt->atomsForSweeping()->sweepIncrementally(maybeAtoms.ref(), budget)) {
    return NotFinished;
  }

  maybeAtoms.reset();

  return Finished;
}

static size_t IncrementalSweepWeakCache(GCRuntime* gc,
                                        const WeakCacheToSweep& item) {
  AutoSetThreadIsSweeping threadIsSweeping(item.zone);

  JS::detail::WeakCacheBase* cache = item.cache;
  MOZ_ASSERT(cache->needsIncrementalBarrier());

  SweepingTracer trc(gc->rt);
  size_t steps =
      cache->traceWeak(&trc, JS::detail::WeakCacheBase::LockStoreBuffer);
  cache->setIncrementalBarrierTracer(nullptr);

  return steps;
}

WeakCacheSweepIterator::WeakCacheSweepIterator(JS::Zone* sweepGroup)
    : sweepZone(sweepGroup), sweepCache(sweepZone->weakCaches().getFirst()) {
  settle();
}

bool WeakCacheSweepIterator::done() const { return !sweepZone; }

WeakCacheToSweep WeakCacheSweepIterator::get() const {
  MOZ_ASSERT(!done());

  return {sweepCache, sweepZone};
}

void WeakCacheSweepIterator::next() {
  MOZ_ASSERT(!done());

  sweepCache = sweepCache->getNext();
  settle();
}

void WeakCacheSweepIterator::settle() {
  while (sweepZone) {
    while (sweepCache && !sweepCache->needsIncrementalBarrier()) {
      sweepCache = sweepCache->getNext();
    }

    if (sweepCache) {
      break;
    }

    sweepZone = sweepZone->nextNodeInGroup();
    if (sweepZone) {
      sweepCache = sweepZone->weakCaches().getFirst();
    }
  }

  MOZ_ASSERT((!sweepZone && !sweepCache) ||
             (sweepCache && sweepCache->needsIncrementalBarrier()));
}

IncrementalProgress GCRuntime::sweepWeakCaches(JS::GCContext* gcx,
                                               SliceBudget& budget) {
  if (weakCachesToSweep.ref().isNothing()) {
    return Finished;
  }

  gcstats::AutoPhase ap(stats(), gcstats::PhaseKind::SWEEP_COMPARTMENTS);

  WeakCacheSweepIterator& work = weakCachesToSweep.ref().ref();

  AutoLockHelperThreadState lock;

  {
    AutoRunParallelWork runWork(this, IncrementalSweepWeakCache,
                                gcstats::PhaseKind::SWEEP_WEAK_CACHES,
                                GCUse::Sweeping, work, budget, lock);
    AutoUnlockHelperThreadState unlock(lock);
  }

  if (work.done()) {
    weakCachesToSweep.ref().reset();
    return Finished;
  }

  return NotFinished;
}

IncrementalProgress GCRuntime::finalizeAllocKind(JS::GCContext* gcx,
                                                 SliceBudget& budget) {
  MOZ_ASSERT(sweepZone->isGCSweeping());

  auto& finalizedArenas = foregroundFinalizedArenas.ref();
  if (!finalizedArenas) {
    finalizedArenas.emplace(sweepAllocKind);
    foregroundFinalizedZone = sweepZone;
    foregroundFinalizedAllocKind = sweepAllocKind;
  } else {
    MOZ_ASSERT(finalizedArenas->allocKind() == sweepAllocKind);
    MOZ_ASSERT(foregroundFinalizedZone == sweepZone);
    MOZ_ASSERT(foregroundFinalizedAllocKind == sweepAllocKind);
  }

  AutoSetThreadIsFinalizing threadIsFinalizing(gcx);
  if (!foregroundFinalize(gcx, sweepZone, sweepAllocKind, budget,
                          finalizedArenas.ref())) {
    return NotFinished;
  }

  finalizedArenas.reset();
  foregroundFinalizedZone = nullptr;
  foregroundFinalizedAllocKind = AllocKind::LIMIT;

  return Finished;
}

SortedArenaList* GCRuntime::maybeGetForegroundFinalizedArenas(Zone* zone,
                                                              AllocKind kind) {
  MOZ_ASSERT(zone);
  MOZ_ASSERT(IsValidAllocKind(kind));

  auto& finalizedArenas = foregroundFinalizedArenas.ref();

  if (finalizedArenas.isNothing() || zone != foregroundFinalizedZone ||
      kind != foregroundFinalizedAllocKind) {
    return nullptr;
  }

  return finalizedArenas.ptr();
}

IncrementalProgress GCRuntime::sweepPropMapTree(JS::GCContext* gcx,
                                                SliceBudget& budget) {
  // Remove dead SharedPropMaps from the tree. This happens incrementally on the
  // main thread. PropMaps are finalized later on the a background thread.

  gcstats::AutoPhase ap(stats(), gcstats::PhaseKind::SWEEP_PROP_MAP);

  ArenaLists& al = sweepZone->arenas;

  if (!SweepArenaList<CompactPropMap>(
          gcx, &al.gcCompactPropMapArenasToUpdate.ref(), budget)) {
    return NotFinished;
  }
  if (!SweepArenaList<NormalPropMap>(
          gcx, &al.gcNormalPropMapArenasToUpdate.ref(), budget)) {
    return NotFinished;
  }

  return Finished;
}

// An iterator for a standard container that provides an STL-like begin()/end()
// interface. This iterator provides a done()/get()/next() style interface.
template <typename Container>
class ContainerIter {
  using Iter = decltype(std::declval<const Container>().begin());
  using Elem = decltype(*std::declval<Iter>());

  Iter iter;
  const Iter end;

 public:
  explicit ContainerIter(const Container& container)
      : iter(container.begin()), end(container.end()) {}

  bool done() const { return iter == end; }

  Elem get() const { return *iter; }

  void next() {
    MOZ_ASSERT(!done());
    ++iter;
  }
};

// IncrementalIter is a template class that makes a normal iterator into one
// that can be used to perform incremental work by using external state that
// persists between instantiations. The state is only initialised on the first
// use and subsequent uses carry on from the previous state.
template <typename Iter>
struct IncrementalIter {
  using State = mozilla::Maybe<Iter>;
  using Elem = decltype(std::declval<Iter>().get());

 private:
  State& maybeIter;

 public:
  template <typename... Args>
  explicit IncrementalIter(State& maybeIter, Args&&... args)
      : maybeIter(maybeIter) {
    if (maybeIter.isNothing()) {
      maybeIter.emplace(std::forward<Args>(args)...);
    }
  }

  ~IncrementalIter() {
    if (done()) {
      maybeIter.reset();
    }
  }

  bool done() const { return maybeIter.ref().done(); }

  Elem get() const { return maybeIter.ref().get(); }

  void next() { maybeIter.ref().next(); }
};

// Iterate through the sweep groups created by
// GCRuntime::groupZonesForSweeping().
class js::gc::SweepGroupsIter {
  GCRuntime* gc;

 public:
  explicit SweepGroupsIter(JSRuntime* rt) : gc(&rt->gc) {
    MOZ_ASSERT(gc->currentSweepGroup);
  }

  bool done() const { return !gc->currentSweepGroup; }

  Zone* get() const { return gc->currentSweepGroup; }

  void next() {
    MOZ_ASSERT(!done());
    gc->getNextSweepGroup();
  }
};

namespace sweepaction {

// Implementation of the SweepAction interface that calls a method on GCRuntime.
class SweepActionCall final : public SweepAction {
  using Method = IncrementalProgress (GCRuntime::*)(JS::GCContext* gcx,
                                                    SliceBudget& budget);

  Method method;

 public:
  explicit SweepActionCall(Method m) : method(m) {}
  IncrementalProgress run(Args& args) override {
    return (args.gc->*method)(args.gcx, args.budget);
  }
  void assertFinished() const override {}
};

// Implementation of the SweepAction interface that yields in a specified zeal
// mode.
class SweepActionMaybeYield final : public SweepAction {
#ifdef JS_GC_ZEAL
  ZealMode mode;
  bool isYielding;
#endif

 public:
  explicit SweepActionMaybeYield(ZealMode mode)
#ifdef JS_GC_ZEAL
      : mode(mode),
        isYielding(false)
#endif
  {
  }

  IncrementalProgress run(Args& args) override {
#ifdef JS_GC_ZEAL
    if (!isYielding && args.gc->shouldYieldForZeal(mode)) {
      isYielding = true;
      return NotFinished;
    }

    isYielding = false;
#endif
    return Finished;
  }

  void assertFinished() const override { MOZ_ASSERT(!isYielding); }

  // These actions should be skipped if GC zeal is not configured.
#ifndef JS_GC_ZEAL
  bool shouldSkip() override { return true; }
#endif
};

// Implementation of the SweepAction interface that calls a list of actions in
// sequence.
class SweepActionSequence final : public SweepAction {
  using ActionVector = Vector<UniquePtr<SweepAction>, 0, SystemAllocPolicy>;
  using Iter = IncrementalIter<ContainerIter<ActionVector>>;

  ActionVector actions;
  typename Iter::State iterState;

 public:
  bool init(UniquePtr<SweepAction>* acts, size_t count) {
    for (size_t i = 0; i < count; i++) {
      auto& action = acts[i];
      if (!action) {
        return false;
      }
      if (action->shouldSkip()) {
        continue;
      }
      if (!actions.emplaceBack(std::move(action))) {
        return false;
      }
    }
    return true;
  }

  IncrementalProgress run(Args& args) override {
    for (Iter iter(iterState, actions); !iter.done(); iter.next()) {
      if (iter.get()->run(args) == NotFinished) {
        return NotFinished;
      }
    }
    return Finished;
  }

  void assertFinished() const override {
    MOZ_ASSERT(iterState.isNothing());
    for (const auto& action : actions) {
      action->assertFinished();
    }
  }
};

template <typename Iter, typename Init>
class SweepActionForEach final : public SweepAction {
  using Elem = decltype(std::declval<Iter>().get());
  using IncrIter = IncrementalIter<Iter>;

  Init iterInit;
  Elem* elemOut;
  UniquePtr<SweepAction> action;
  typename IncrIter::State iterState;

 public:
  SweepActionForEach(const Init& init, Elem* maybeElemOut,
                     UniquePtr<SweepAction> action)
      : iterInit(init), elemOut(maybeElemOut), action(std::move(action)) {}

  IncrementalProgress run(Args& args) override {
    MOZ_ASSERT_IF(elemOut, *elemOut == Elem());
    auto clearElem = mozilla::MakeScopeExit([&] { setElem(Elem()); });
    for (IncrIter iter(iterState, iterInit); !iter.done(); iter.next()) {
      setElem(iter.get());
      if (action->run(args) == NotFinished) {
        return NotFinished;
      }
    }
    return Finished;
  }

  void assertFinished() const override {
    MOZ_ASSERT(iterState.isNothing());
    MOZ_ASSERT_IF(elemOut, *elemOut == Elem());
    action->assertFinished();
  }

 private:
  void setElem(const Elem& value) {
    if (elemOut) {
      *elemOut = value;
    }
  }
};

static UniquePtr<SweepAction> Call(IncrementalProgress (GCRuntime::*method)(
    JS::GCContext* gcx, SliceBudget& budget)) {
  return MakeUnique<SweepActionCall>(method);
}

static UniquePtr<SweepAction> MaybeYield(ZealMode zealMode) {
  return MakeUnique<SweepActionMaybeYield>(zealMode);
}

template <typename... Rest>
static UniquePtr<SweepAction> Sequence(UniquePtr<SweepAction> first,
                                       Rest... rest) {
  UniquePtr<SweepAction> actions[] = {std::move(first), std::move(rest)...};
  auto seq = MakeUnique<SweepActionSequence>();
  if (!seq || !seq->init(actions, std::size(actions))) {
    return nullptr;
  }

  return UniquePtr<SweepAction>(std::move(seq));
}

static UniquePtr<SweepAction> RepeatForSweepGroup(
    JSRuntime* rt, UniquePtr<SweepAction> action) {
  if (!action) {
    return nullptr;
  }

  using Action = SweepActionForEach<SweepGroupsIter, JSRuntime*>;
  return js::MakeUnique<Action>(rt, nullptr, std::move(action));
}

static UniquePtr<SweepAction> ForEachZoneInSweepGroup(
    JSRuntime* rt, Zone** zoneOut, UniquePtr<SweepAction> action) {
  if (!action) {
    return nullptr;
  }

  using Action = SweepActionForEach<SweepGroupZonesIter, JSRuntime*>;
  return js::MakeUnique<Action>(rt, zoneOut, std::move(action));
}

static UniquePtr<SweepAction> ForEachAllocKind(AllocKinds kinds,
                                               AllocKind* kindOut,
                                               UniquePtr<SweepAction> action) {
  if (!action) {
    return nullptr;
  }

  using Action = SweepActionForEach<ContainerIter<AllocKinds>, AllocKinds>;
  return js::MakeUnique<Action>(kinds, kindOut, std::move(action));
}

}  // namespace sweepaction

bool GCRuntime::initSweepActions() {
  using namespace sweepaction;
  using sweepaction::Call;

  sweepActions.ref() = RepeatForSweepGroup(
      rt,
      Sequence(
          Call(&GCRuntime::beginMarkingSweepGroup),
          Call(&GCRuntime::markGrayRootsInCurrentGroup),
          MaybeYield(ZealMode::YieldWhileGrayMarking),
          Call(&GCRuntime::markGray), Call(&GCRuntime::endMarkingSweepGroup),
          Call(&GCRuntime::beginSweepingSweepGroup),
          MaybeYield(ZealMode::IncrementalMultipleSlices),
          MaybeYield(ZealMode::YieldBeforeSweepingAtoms),
          Call(&GCRuntime::sweepAtomsTable),
          MaybeYield(ZealMode::YieldBeforeSweepingCaches),
          Call(&GCRuntime::sweepWeakCaches),
          ForEachZoneInSweepGroup(
              rt, &sweepZone.ref(),
              Sequence(MaybeYield(ZealMode::YieldBeforeSweepingObjects),
                       ForEachAllocKind(ForegroundObjectFinalizePhase.kinds,
                                        &sweepAllocKind.ref(),
                                        Call(&GCRuntime::finalizeAllocKind)),
                       MaybeYield(ZealMode::YieldBeforeSweepingNonObjects),
                       ForEachAllocKind(ForegroundNonObjectFinalizePhase.kinds,
                                        &sweepAllocKind.ref(),
                                        Call(&GCRuntime::finalizeAllocKind)),
                       MaybeYield(ZealMode::YieldBeforeSweepingPropMapTrees),
                       Call(&GCRuntime::sweepPropMapTree))),
          Call(&GCRuntime::endSweepingSweepGroup)));

  return sweepActions != nullptr;
}

void GCRuntime::prepareForSweepSlice(JS::GCReason reason) {
  // Work that must be done at the start of each slice where we sweep.
  //
  // Since this must happen at the start of the slice, it must be called in
  // marking slices before any sweeping happens. Therefore it is called
  // conservatively since we may not always transition to sweeping from marking.

  // Clear out whole cell store buffer entries to unreachable cells.
  if (storeBuffer().mayHavePointersToDeadCells()) {
    collectNurseryFromMajorGC(reason);
  }

  // Trace wrapper rooters before marking if we might start sweeping in
  // this slice.
  rt->mainContextFromOwnThread()->traceWrapperGCRooters(marker().tracer());
}

IncrementalProgress GCRuntime::performSweepActions(SliceBudget& budget) {
  MOZ_ASSERT(!storeBuffer().mayHavePointersToDeadCells());

  AutoMajorGCProfilerEntry s(this);
  gcstats::AutoPhase ap(stats(), gcstats::PhaseKind::SWEEP);

  JS::GCContext* gcx = rt->gcContext();
  AutoSetThreadIsSweeping threadIsSweeping(gcx);
  AutoPoisonFreedJitCode pjc(gcx);

  // Don't trigger pre-barriers when finalizing.
  AutoDisableBarriers disableBarriers(this);

  // Drain the mark stack, possibly in a parallel task if we're in a part of
  // sweeping that allows it.
  //
  // The first time we enter the sweep phase we must not yield to the mutator
  // until we've starting sweeping a sweep group but in that case the stack must
  // be empty already.

  MOZ_ASSERT(initialState <= State::Sweep);
  bool startOfSweeping = initialState < State::Sweep;

  if (startOfSweeping) {
    assertNoMarkingWork();
  } else {
    if (markDuringSweeping(gcx, budget) == NotFinished) {
      return NotFinished;
    }
  }

  // Then continue running sweep actions.

  SweepAction::Args args{this, gcx, budget};
  IncrementalProgress sweepProgress = sweepActions->run(args);
  IncrementalProgress markProgress = joinBackgroundMarkTask();

  if (sweepProgress == Finished && markProgress == Finished) {
    return Finished;
  }

  MOZ_ASSERT(isIncremental);
  return NotFinished;
}

bool GCRuntime::allCCVisibleZonesWereCollected() {
  // Calculate whether the gray marking state is now valid.
  //
  // The gray bits change from invalid to valid if we finished a full GC from
  // the point of view of the cycle collector. We ignore the following:
  //
  //  - Helper thread zones, as these are not reachable from the main heap.
  //  - The atoms zone, since strings and symbols are never marked gray.
  //  - Empty zones.
  //
  // These exceptions ensure that when the CC requests a full GC the gray mark
  // state ends up valid even it we don't collect all of the zones.

  for (ZonesIter zone(this, SkipAtoms); !zone.done(); zone.next()) {
    if (!zone->isCollecting() && !zone->arenas.arenaListsAreEmpty()) {
      return false;
    }
  }

  return true;
}

void GCRuntime::endSweepPhase(bool destroyingRuntime) {
  MOZ_ASSERT(!markOnBackgroundThreadDuringSweeping);

  sweepActions->assertFinished();

  gcstats::AutoPhase ap(stats(), gcstats::PhaseKind::SWEEP);

  MOZ_ASSERT_IF(destroyingRuntime, !useBackgroundThreads);

  // Release parallel marking threads for worker runtimes now we've finished
  // marking. The main thread keeps the reservation as long as parallel marking
  // is enabled.
  if (!rt->isMainRuntime()) {
    MOZ_ASSERT_IF(useParallelMarking, reservedMarkingThreads != 0);
    releaseMarkingThreads();
  }

  {
    gcstats::AutoPhase ap(stats(), gcstats::PhaseKind::DESTROY);

    // Sweep shared script bytecode now all zones have been swept and finalizers
    // for BaseScripts have released their references.
    SweepScriptData(rt);
  }

  {
    gcstats::AutoPhase ap(stats(), gcstats::PhaseKind::FINALIZE_END);
    AutoLockStoreBuffer lock(rt);
    callFinalizeCallbacks(rt->gcContext(), JSFINALIZE_COLLECTION_END);

    if (allCCVisibleZonesWereCollected()) {
      grayBitsValid = true;
    }
  }

  if (isIncremental) {
    findDeadCompartments();
  }

#ifdef JS_GC_ZEAL
  finishMarkingValidation();
#endif

#ifdef DEBUG
  for (ZonesIter zone(this, WithAtoms); !zone.done(); zone.next()) {
    for (auto i : AllAllocKinds()) {
      MOZ_ASSERT_IF(!IsBackgroundFinalized(i) || !useBackgroundThreads,
                    zone->arenas.collectingArenaList(i).isEmpty());
    }
  }
#endif

  AssertNoWrappersInGrayList(rt);
}
