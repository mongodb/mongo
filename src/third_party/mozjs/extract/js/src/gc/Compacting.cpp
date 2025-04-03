/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * Implementation of compacting GC.
 */

#include "mozilla/Maybe.h"

#include "debugger/DebugAPI.h"
#include "gc/ArenaList.h"
#include "gc/GCInternals.h"
#include "gc/GCLock.h"
#include "gc/ParallelWork.h"
#include "gc/Zone.h"
#include "jit/JitCode.h"
#include "jit/JitRuntime.h"
#include "jit/JitZone.h"
#include "js/GCAPI.h"
#include "vm/HelperThreads.h"
#include "vm/Realm.h"
#include "wasm/WasmGcObject.h"

#include "gc/Heap-inl.h"
#include "gc/Marking-inl.h"
#include "gc/PrivateIterators-inl.h"
#include "gc/StableCellHasher-inl.h"
#include "gc/TraceMethods-inl.h"
#include "vm/GeckoProfiler-inl.h"

using namespace js;
using namespace js::gc;

using mozilla::Maybe;

bool GCRuntime::canRelocateZone(Zone* zone) const {
  return !zone->isAtomsZone();
}

void GCRuntime::beginCompactPhase() {
  MOZ_ASSERT(!isBackgroundSweeping());
  assertBackgroundSweepingFinished();

  gcstats::AutoPhase ap(stats(), gcstats::PhaseKind::COMPACT);

  MOZ_ASSERT(zonesToMaybeCompact.ref().isEmpty());
  for (GCZonesIter zone(this); !zone.done(); zone.next()) {
    if (canRelocateZone(zone)) {
      zonesToMaybeCompact.ref().append(zone);
    }
  }

  startedCompacting = true;
  zonesCompacted = 0;

#ifdef DEBUG
  AutoLockGC lock(this);
  MOZ_ASSERT(!relocatedArenasToRelease);
#endif
}

IncrementalProgress GCRuntime::compactPhase(JS::GCReason reason,
                                            SliceBudget& sliceBudget,
                                            AutoGCSession& session) {
  assertBackgroundSweepingFinished();
  MOZ_ASSERT(startedCompacting);

  AutoMajorGCProfilerEntry s(this);
  gcstats::AutoPhase ap(stats(), gcstats::PhaseKind::COMPACT);

  // TODO: JSScripts can move. If the sampler interrupts the GC in the
  // middle of relocating an arena, invalid JSScript pointers may be
  // accessed. Suppress all sampling until a finer-grained solution can be
  // found. See bug 1295775.
  AutoSuppressProfilerSampling suppressSampling(rt->mainContextFromOwnThread());

  ZoneList relocatedZones;
  Arena* relocatedArenas = nullptr;
  while (!zonesToMaybeCompact.ref().isEmpty()) {
    Zone* zone = zonesToMaybeCompact.ref().front();
    zonesToMaybeCompact.ref().removeFront();

    MOZ_ASSERT(nursery().isEmpty());
    zone->changeGCState(Zone::Finished, Zone::Compact);

    if (relocateArenas(zone, reason, relocatedArenas, sliceBudget)) {
      updateZonePointersToRelocatedCells(zone);
      relocatedZones.append(zone);
      zonesCompacted++;
    } else {
      zone->changeGCState(Zone::Compact, Zone::Finished);
    }

    if (sliceBudget.isOverBudget()) {
      break;
    }
  }

  if (!relocatedZones.isEmpty()) {
    updateRuntimePointersToRelocatedCells(session);

    do {
      Zone* zone = relocatedZones.front();
      relocatedZones.removeFront();
      zone->changeGCState(Zone::Compact, Zone::Finished);
    } while (!relocatedZones.isEmpty());
  }

  clearRelocatedArenas(relocatedArenas, reason);

#ifdef DEBUG
  protectOrReleaseRelocatedArenas(relocatedArenas, reason);
#else
  releaseRelocatedArenas(relocatedArenas);
#endif

  // Clear caches that can contain cell pointers.
  rt->caches().purgeForCompaction();

#ifdef DEBUG
  checkHashTablesAfterMovingGC();
#endif

  return zonesToMaybeCompact.ref().isEmpty() ? Finished : NotFinished;
}

void GCRuntime::endCompactPhase() { startedCompacting = false; }

static bool ShouldRelocateAllArenas(JS::GCReason reason) {
  return reason == JS::GCReason::DEBUG_GC;
}

/*
 * Choose which arenas to relocate all cells from. Return an arena cursor that
 * can be passed to removeRemainingArenas().
 */
Arena** ArenaList::pickArenasToRelocate(size_t& arenaTotalOut,
                                        size_t& relocTotalOut) {
  // Relocate the greatest number of arenas such that the number of used cells
  // in relocated arenas is less than or equal to the number of free cells in
  // unrelocated arenas. In other words we only relocate cells we can move
  // into existing arenas, and we choose the least full areans to relocate.
  //
  // This is made easier by the fact that the arena list has been sorted in
  // descending order of number of used cells, so we will always relocate a
  // tail of the arena list. All we need to do is find the point at which to
  // start relocating.

  check();

  if (isCursorAtEnd()) {
    return nullptr;
  }

  Arena** arenap = cursorp_;      // Next arena to consider for relocation.
  size_t previousFreeCells = 0;   // Count of free cells before arenap.
  size_t followingUsedCells = 0;  // Count of used cells after arenap.
  size_t fullArenaCount = 0;      // Number of full arenas (not relocated).
  size_t nonFullArenaCount =
      0;  // Number of non-full arenas (considered for relocation).
  size_t arenaIndex = 0;  // Index of the next arena to consider.

  for (Arena* arena = head_; arena != *cursorp_; arena = arena->next) {
    fullArenaCount++;
  }

  for (Arena* arena = *cursorp_; arena; arena = arena->next) {
    followingUsedCells += arena->countUsedCells();
    nonFullArenaCount++;
  }

  mozilla::DebugOnly<size_t> lastFreeCells(0);
  size_t cellsPerArena = Arena::thingsPerArena((*arenap)->getAllocKind());

  while (*arenap) {
    Arena* arena = *arenap;
    if (followingUsedCells <= previousFreeCells) {
      break;
    }

    size_t freeCells = arena->countFreeCells();
    size_t usedCells = cellsPerArena - freeCells;
    followingUsedCells -= usedCells;
#ifdef DEBUG
    MOZ_ASSERT(freeCells >= lastFreeCells);
    lastFreeCells = freeCells;
#endif
    previousFreeCells += freeCells;
    arenap = &arena->next;
    arenaIndex++;
  }

  size_t relocCount = nonFullArenaCount - arenaIndex;
  MOZ_ASSERT(relocCount < nonFullArenaCount);
  MOZ_ASSERT((relocCount == 0) == (!*arenap));
  arenaTotalOut += fullArenaCount + nonFullArenaCount;
  relocTotalOut += relocCount;

  return arenap;
}

#ifdef DEBUG
inline bool PtrIsInRange(const void* ptr, const void* start, size_t length) {
  return uintptr_t(ptr) - uintptr_t(start) < length;
}
#endif

static void RelocateCell(Zone* zone, TenuredCell* src, AllocKind thingKind,
                         size_t thingSize) {
  JS::AutoSuppressGCAnalysis nogc;

  // Allocate a new cell.
  MOZ_ASSERT(zone == src->zone());
  TenuredCell* dst =
      reinterpret_cast<TenuredCell*>(AllocateCellInGC(zone, thingKind));

  // Copy source cell contents to destination.
  memcpy(dst, src, thingSize);

  // Move any uid attached to the object.
  gc::TransferUniqueId(dst, src);

  if (IsObjectAllocKind(thingKind)) {
    auto* srcObj = static_cast<JSObject*>(static_cast<Cell*>(src));
    auto* dstObj = static_cast<JSObject*>(static_cast<Cell*>(dst));

    if (srcObj->is<NativeObject>()) {
      NativeObject* srcNative = &srcObj->as<NativeObject>();
      NativeObject* dstNative = &dstObj->as<NativeObject>();

      // Fixup the pointer to inline object elements if necessary.
      if (srcNative->hasFixedElements()) {
        uint32_t numShifted =
            srcNative->getElementsHeader()->numShiftedElements();
        dstNative->setFixedElements(numShifted);
      }
    } else if (srcObj->is<ProxyObject>()) {
      if (srcObj->as<ProxyObject>().usingInlineValueArray()) {
        dstObj->as<ProxyObject>().setInlineValueArray();
      }
    }

    // Call object moved hook if present.
    if (JSObjectMovedOp op = srcObj->getClass()->extObjectMovedOp()) {
      op(dstObj, srcObj);
    }

    MOZ_ASSERT_IF(
        dstObj->is<NativeObject>(),
        !PtrIsInRange(
            (const Value*)dstObj->as<NativeObject>().getDenseElements(), src,
            thingSize));
  }

  // Copy the mark bits.
  dst->copyMarkBitsFrom(src);

  // Poison the source cell contents except for the forwarding flag and pointer
  // which will be stored in the first word. We can't do this for native object
  // with fixed elements because this would overwrite the element flags and
  // these are needed when updating COW elements referred to by other objects.
#ifdef DEBUG
  JSObject* srcObj = IsObjectAllocKind(thingKind)
                         ? static_cast<JSObject*>(static_cast<Cell*>(src))
                         : nullptr;
  if (!srcObj || !srcObj->is<NativeObject>() ||
      !srcObj->as<NativeObject>().hasFixedElements()) {
    AlwaysPoison(reinterpret_cast<uint8_t*>(src) + sizeof(uintptr_t),
                 JS_MOVED_TENURED_PATTERN, thingSize - sizeof(uintptr_t),
                 MemCheckKind::MakeNoAccess);
  }
#endif

  // Mark source cell as forwarded and leave a pointer to the destination.
  RelocationOverlay::forwardCell(src, dst);
}

static void RelocateArena(Arena* arena, SliceBudget& sliceBudget) {
  MOZ_ASSERT(arena->allocated());
  MOZ_ASSERT(!arena->onDelayedMarkingList());
  MOZ_ASSERT(arena->bufferedCells()->isEmpty());

  Zone* zone = arena->zone;

  AllocKind thingKind = arena->getAllocKind();
  size_t thingSize = arena->getThingSize();

  for (ArenaCellIterUnderGC cell(arena); !cell.done(); cell.next()) {
    RelocateCell(zone, cell, thingKind, thingSize);
    sliceBudget.step();
  }

#ifdef DEBUG
  for (ArenaCellIterUnderGC cell(arena); !cell.done(); cell.next()) {
    TenuredCell* src = cell;
    MOZ_ASSERT(src->isForwarded());
    TenuredCell* dest = Forwarded(src);
    MOZ_ASSERT(src->isMarkedBlack() == dest->isMarkedBlack());
    MOZ_ASSERT(src->isMarkedGray() == dest->isMarkedGray());
  }
#endif
}

/*
 * Relocate all arenas identified by pickArenasToRelocate: for each arena,
 * relocate each cell within it, then add it to a list of relocated arenas.
 */
Arena* ArenaList::relocateArenas(Arena* toRelocate, Arena* relocated,
                                 SliceBudget& sliceBudget,
                                 gcstats::Statistics& stats) {
  check();

  while (Arena* arena = toRelocate) {
    toRelocate = arena->next;
    RelocateArena(arena, sliceBudget);
    // Prepend to list of relocated arenas
    arena->next = relocated;
    relocated = arena;
    stats.count(gcstats::COUNT_ARENA_RELOCATED);
  }

  check();

  return relocated;
}

// Skip compacting zones unless we can free a certain proportion of their GC
// heap memory.
static const float MIN_ZONE_RECLAIM_PERCENT = 2.0;

static bool ShouldRelocateZone(size_t arenaCount, size_t relocCount,
                               JS::GCReason reason) {
  if (relocCount == 0) {
    return false;
  }

  if (IsOOMReason(reason)) {
    return true;
  }

  return (relocCount * 100.0f) / arenaCount >= MIN_ZONE_RECLAIM_PERCENT;
}

static AllocKinds CompactingAllocKinds() {
  AllocKinds result;
  for (AllocKind kind : AllAllocKinds()) {
    if (IsCompactingKind(kind)) {
      result += kind;
    }
  }
  return result;
}

bool ArenaLists::relocateArenas(Arena*& relocatedListOut, JS::GCReason reason,
                                SliceBudget& sliceBudget,
                                gcstats::Statistics& stats) {
  // This is only called from the main thread while we are doing a GC, so
  // there is no need to lock.
  MOZ_ASSERT(CurrentThreadCanAccessRuntime(runtime()));
  MOZ_ASSERT(runtime()->gc.isHeapCompacting());
  MOZ_ASSERT(!runtime()->gc.isBackgroundSweeping());

  // Relocate all compatible kinds
  AllocKinds allocKindsToRelocate = CompactingAllocKinds();

  // Clear all the free lists.
  clearFreeLists();

  if (ShouldRelocateAllArenas(reason)) {
    zone_->prepareForCompacting();
    for (auto kind : allocKindsToRelocate) {
      ArenaList& al = arenaList(kind);
      Arena* allArenas = al.head();
      al.clear();
      relocatedListOut =
          al.relocateArenas(allArenas, relocatedListOut, sliceBudget, stats);
    }
  } else {
    size_t arenaCount = 0;
    size_t relocCount = 0;
    AllAllocKindArray<Arena**> toRelocate;

    for (auto kind : allocKindsToRelocate) {
      toRelocate[kind] =
          arenaList(kind).pickArenasToRelocate(arenaCount, relocCount);
    }

    if (!ShouldRelocateZone(arenaCount, relocCount, reason)) {
      return false;
    }

    zone_->prepareForCompacting();
    for (auto kind : allocKindsToRelocate) {
      if (toRelocate[kind]) {
        ArenaList& al = arenaList(kind);
        Arena* arenas = al.removeRemainingArenas(toRelocate[kind]);
        relocatedListOut =
            al.relocateArenas(arenas, relocatedListOut, sliceBudget, stats);
      }
    }
  }

  return true;
}

bool GCRuntime::relocateArenas(Zone* zone, JS::GCReason reason,
                               Arena*& relocatedListOut,
                               SliceBudget& sliceBudget) {
  gcstats::AutoPhase ap(stats(), gcstats::PhaseKind::COMPACT_MOVE);

  MOZ_ASSERT(!zone->isPreservingCode());
  MOZ_ASSERT(canRelocateZone(zone));

  js::CancelOffThreadIonCompile(rt, JS::Zone::Compact);

  if (!zone->arenas.relocateArenas(relocatedListOut, reason, sliceBudget,
                                   stats())) {
    return false;
  }

#ifdef DEBUG
  // Check that we did as much compaction as we should have. There
  // should always be less than one arena's worth of free cells.
  for (auto kind : CompactingAllocKinds()) {
    ArenaList& al = zone->arenas.arenaList(kind);
    size_t freeCells = 0;
    for (Arena* arena = al.arenaAfterCursor(); arena; arena = arena->next) {
      freeCells += arena->countFreeCells();
    }
    MOZ_ASSERT(freeCells < Arena::thingsPerArena(kind));
  }
#endif

  return true;
}

MovingTracer::MovingTracer(JSRuntime* rt)
    : GenericTracerImpl(rt, JS::TracerKind::Moving,
                        JS::WeakMapTraceAction::TraceKeysAndValues) {}

template <typename T>
inline void MovingTracer::onEdge(T** thingp, const char* name) {
  T* thing = *thingp;
  if (thing->runtimeFromAnyThread() == runtime() && IsForwarded(thing)) {
    *thingp = Forwarded(thing);
  }
}

void Zone::prepareForCompacting() {
  JS::GCContext* gcx = runtimeFromMainThread()->gcContext();
  discardJitCode(gcx);
}

void GCRuntime::sweepZoneAfterCompacting(MovingTracer* trc, Zone* zone) {
  MOZ_ASSERT(zone->isGCCompacting());

  zone->traceWeakMaps(trc);

  traceWeakFinalizationObserverEdges(trc, zone);

  for (auto* cache : zone->weakCaches()) {
    cache->traceWeak(trc, nullptr);
  }

  if (jit::JitZone* jitZone = zone->jitZone()) {
    jitZone->traceWeak(trc);
  }

  for (CompartmentsInZoneIter c(zone); !c.done(); c.next()) {
    c->traceWeakNativeIterators(trc);

    for (RealmsInCompartmentIter r(c); !r.done(); r.next()) {
      r->traceWeakRegExps(trc);
      r->traceWeakSavedStacks(trc);
      r->traceWeakGlobalEdge(trc);
      r->traceWeakDebugEnvironmentEdges(trc);
      r->traceWeakEdgesInJitRealm(trc);
    }
  }
}

template <typename T>
static inline void UpdateCellPointers(MovingTracer* trc, T* cell) {
  // We only update unmoved GC things or the new copy of moved GC things, never
  // the old copy. If this happened it could clear the forwarded flag which
  // could lead to pointers to the old copy not being updated.
  MOZ_ASSERT(!cell->isForwarded());

  cell->fixupAfterMovingGC();
  cell->traceChildren(trc);
}

template <typename T>
static void UpdateArenaPointersTyped(MovingTracer* trc, Arena* arena) {
  for (ArenaCellIterUnderGC cell(arena); !cell.done(); cell.next()) {
    UpdateCellPointers(trc, cell.as<T>());
  }
}

static bool CanUpdateKindInBackground(AllocKind kind) {
  // We try to update as many GC things in parallel as we can, but there are
  // kinds for which this might not be safe:
  //  - we assume JSObjects that are foreground finalized are not safe to
  //    update in parallel
  //  - updating a SharedPropMap touches child maps in
  //    SharedPropMap::fixupAfterMovingGC
  return js::gc::IsBackgroundFinalized(kind) && !IsShapeAllocKind(kind) &&
         kind != AllocKind::BASE_SHAPE;
}

/*
 * Update the internal pointers for all cells in an arena.
 */
static void UpdateArenaPointers(MovingTracer* trc, Arena* arena) {
  AllocKind kind = arena->getAllocKind();

  MOZ_ASSERT_IF(!CanUpdateKindInBackground(kind),
                CurrentThreadCanAccessRuntime(trc->runtime()));

  switch (kind) {
#define EXPAND_CASE(allocKind, traceKind, type, sizedType, bgFinal, nursery, \
                    compact)                                                 \
  case AllocKind::allocKind:                                                 \
    UpdateArenaPointersTyped<type>(trc, arena);                              \
    return;
    FOR_EACH_ALLOCKIND(EXPAND_CASE)
#undef EXPAND_CASE

    default:
      MOZ_CRASH("Invalid alloc kind for UpdateArenaPointers");
  }
}

struct ArenaListSegment {
  Arena* begin;
  Arena* end;
};

/*
 * Update the internal pointers for all arenas in a segment of an arena list.
 *
 * Returns the number of steps to count against the slice budget.
 */
static size_t UpdateArenaListSegmentPointers(GCRuntime* gc,
                                             const ArenaListSegment& arenas) {
  MOZ_ASSERT(arenas.begin);
  MovingTracer trc(gc->rt);
  size_t count = 0;
  for (Arena* arena = arenas.begin; arena != arenas.end; arena = arena->next) {
    UpdateArenaPointers(&trc, arena);
    count++;
  }
  return count * 256;
}

class ArenasToUpdate {
  // Maximum number of arenas to update in one block.
#ifdef DEBUG
  static const unsigned MaxArenasToProcess = 16;
#else
  static const unsigned MaxArenasToProcess = 256;
#endif

 public:
  explicit ArenasToUpdate(Zone* zone);
  ArenasToUpdate(Zone* zone, const AllocKinds& kinds);

  bool done() const { return !segmentBegin; }

  ArenaListSegment get() const {
    MOZ_ASSERT(!done());
    return {segmentBegin, segmentEnd};
  }

  void next();

 private:
  Maybe<AllocKinds> kinds;            // Selects which thing kinds to update.
  Zone* zone;                         // Zone to process.
  AllocKind kind = AllocKind::FIRST;  // Current alloc kind to process.
  Arena* segmentBegin = nullptr;
  Arena* segmentEnd = nullptr;

  static AllocKind nextAllocKind(AllocKind i) {
    return AllocKind(uint8_t(i) + 1);
  }

  void settle();
  void findSegmentEnd();
};

ArenasToUpdate::ArenasToUpdate(Zone* zone) : zone(zone) { settle(); }

ArenasToUpdate::ArenasToUpdate(Zone* zone, const AllocKinds& kinds)
    : kinds(Some(kinds)), zone(zone) {
  settle();
}

void ArenasToUpdate::settle() {
  // Called when we have set |kind| to a new kind. Sets |arena| to the next
  // arena or null if there are no more arenas to update.

  MOZ_ASSERT(!segmentBegin);

  for (; kind < AllocKind::LIMIT; kind = nextAllocKind(kind)) {
    if (kinds && !kinds.ref().contains(kind)) {
      continue;
    }

    Arena* arena = zone->arenas.getFirstArena(kind);
    if (arena) {
      segmentBegin = arena;
      findSegmentEnd();
      break;
    }
  }
}

void ArenasToUpdate::findSegmentEnd() {
  // Take up to MaxArenasToProcess arenas from the list starting at
  // |segmentBegin| and set |segmentEnd|.
  Arena* arena = segmentBegin;
  for (size_t i = 0; arena && i < MaxArenasToProcess; i++) {
    arena = arena->next;
  }
  segmentEnd = arena;
}

void ArenasToUpdate::next() {
  MOZ_ASSERT(!done());

  segmentBegin = segmentEnd;
  if (segmentBegin) {
    findSegmentEnd();
    return;
  }

  kind = nextAllocKind(kind);
  settle();
}

static AllocKinds ForegroundUpdateKinds(AllocKinds kinds) {
  AllocKinds result;
  for (AllocKind kind : kinds) {
    if (!CanUpdateKindInBackground(kind)) {
      result += kind;
    }
  }
  return result;
}

void GCRuntime::updateCellPointers(Zone* zone, AllocKinds kinds) {
  AllocKinds fgKinds = ForegroundUpdateKinds(kinds);
  AllocKinds bgKinds = kinds - fgKinds;

  ArenasToUpdate fgArenas(zone, fgKinds);
  ArenasToUpdate bgArenas(zone, bgKinds);

  AutoLockHelperThreadState lock;

  AutoRunParallelWork bgTasks(this, UpdateArenaListSegmentPointers,
                              gcstats::PhaseKind::COMPACT_UPDATE_CELLS,
                              GCUse::Unspecified, bgArenas,
                              SliceBudget::unlimited(), lock);

  AutoUnlockHelperThreadState unlock(lock);

  for (; !fgArenas.done(); fgArenas.next()) {
    UpdateArenaListSegmentPointers(this, fgArenas.get());
  }
}

// After cells have been relocated any pointers to a cell's old locations must
// be updated to point to the new location.  This happens by iterating through
// all cells in heap and tracing their children (non-recursively) to update
// them.
//
// This is complicated by the fact that updating a GC thing sometimes depends on
// making use of other GC things.  After a moving GC these things may not be in
// a valid state since they may contain pointers which have not been updated
// yet.
//
// The main dependencies are:
//
//   - Updating a JSObject makes use of its shape
//   - Updating a typed object makes use of its type descriptor object
//
// This means we require at least three phases for update:
//
//  1) shapes
//  2) typed object type descriptor objects
//  3) all other objects
//
// Also, there can be data races calling IsForwarded() on the new location of a
// cell whose first word is being updated in parallel on another thread. This
// easiest way to avoid this is to not store a GC pointer in the first word of a
// cell. Otherwise this can be avoided by updating different kinds of cell in
// different phases.
//
// Since we want to minimize the number of phases, arrange kinds into three
// arbitrary phases.

static constexpr AllocKinds UpdatePhaseOne{AllocKind::SCRIPT,
                                           AllocKind::BASE_SHAPE,
                                           AllocKind::SHAPE,
                                           AllocKind::STRING,
                                           AllocKind::JITCODE,
                                           AllocKind::REGEXP_SHARED,
                                           AllocKind::SCOPE,
                                           AllocKind::GETTER_SETTER,
                                           AllocKind::COMPACT_PROP_MAP,
                                           AllocKind::NORMAL_PROP_MAP,
                                           AllocKind::DICT_PROP_MAP};

// UpdatePhaseTwo is typed object descriptor objects.

static constexpr AllocKinds UpdatePhaseThree{AllocKind::FUNCTION,
                                             AllocKind::FUNCTION_EXTENDED,
                                             AllocKind::OBJECT0,
                                             AllocKind::OBJECT0_BACKGROUND,
                                             AllocKind::OBJECT2,
                                             AllocKind::OBJECT2_BACKGROUND,
                                             AllocKind::ARRAYBUFFER4,
                                             AllocKind::OBJECT4,
                                             AllocKind::OBJECT4_BACKGROUND,
                                             AllocKind::ARRAYBUFFER8,
                                             AllocKind::OBJECT8,
                                             AllocKind::OBJECT8_BACKGROUND,
                                             AllocKind::ARRAYBUFFER12,
                                             AllocKind::OBJECT12,
                                             AllocKind::OBJECT12_BACKGROUND,
                                             AllocKind::ARRAYBUFFER16,
                                             AllocKind::OBJECT16,
                                             AllocKind::OBJECT16_BACKGROUND};

void GCRuntime::updateAllCellPointers(MovingTracer* trc, Zone* zone) {
  updateCellPointers(zone, UpdatePhaseOne);

  updateCellPointers(zone, UpdatePhaseThree);
}

/*
 * Update pointers to relocated cells in a single zone by doing a traversal of
 * that zone's arenas and calling per-zone sweep hooks.
 *
 * The latter is necessary to update weak references which are not marked as
 * part of the traversal.
 */
void GCRuntime::updateZonePointersToRelocatedCells(Zone* zone) {
  MOZ_ASSERT(!rt->isBeingDestroyed());
  MOZ_ASSERT(zone->isGCCompacting());

  AutoTouchingGrayThings tgt;

  gcstats::AutoPhase ap(stats(), gcstats::PhaseKind::COMPACT_UPDATE);
  MovingTracer trc(rt);

  zone->fixupAfterMovingGC();
  zone->fixupScriptMapsAfterMovingGC(&trc);

  // Fixup compartment global pointers as these get accessed during marking.
  for (CompartmentsInZoneIter comp(zone); !comp.done(); comp.next()) {
    comp->fixupAfterMovingGC(&trc);
  }

  zone->externalStringCache().purge();
  zone->functionToStringCache().purge();
  zone->shapeZone().purgeShapeCaches(rt->gcContext());
  rt->caches().stringToAtomCache.purge();

  // Iterate through all cells that can contain relocatable pointers to update
  // them. Since updating each cell is independent we try to parallelize this
  // as much as possible.
  updateAllCellPointers(&trc, zone);

  // Sweep everything to fix up weak pointers.
  sweepZoneAfterCompacting(&trc, zone);

  // Call callbacks to get the rest of the system to fixup other untraced
  // pointers.
  for (CompartmentsInZoneIter comp(zone); !comp.done(); comp.next()) {
    callWeakPointerCompartmentCallbacks(&trc, comp);
  }
}

/*
 * Update runtime-wide pointers to relocated cells.
 */
void GCRuntime::updateRuntimePointersToRelocatedCells(AutoGCSession& session) {
  MOZ_ASSERT(!rt->isBeingDestroyed());

  gcstats::AutoPhase ap1(stats(), gcstats::PhaseKind::COMPACT_UPDATE);
  MovingTracer trc(rt);

  Zone::fixupAllCrossCompartmentWrappersAfterMovingGC(&trc);

  rt->geckoProfiler().fixupStringsMapAfterMovingGC();

  // Mark roots to update them.

  traceRuntimeForMajorGC(&trc, session);

  {
    gcstats::AutoPhase ap2(stats(), gcstats::PhaseKind::MARK_ROOTS);
    DebugAPI::traceAllForMovingGC(&trc);
    DebugAPI::traceCrossCompartmentEdges(&trc);

    // Mark all gray roots.
    traceEmbeddingGrayRoots(&trc);
    Compartment::traceIncomingCrossCompartmentEdgesForZoneGC(
        &trc, Compartment::GrayEdges);
  }

  // Sweep everything to fix up weak pointers.
  jit::JitRuntime::TraceWeakJitcodeGlobalTable(rt, &trc);
  for (JS::detail::WeakCacheBase* cache : rt->weakCaches()) {
    cache->traceWeak(&trc, nullptr);
  }

  if (rt->hasJitRuntime() && rt->jitRuntime()->hasInterpreterEntryMap()) {
    rt->jitRuntime()->getInterpreterEntryMap()->updateScriptsAfterMovingGC();
  }

  // Type inference may put more blocks here to free.
  {
    AutoLockHelperThreadState lock;
    lifoBlocksToFree.ref().freeAll();
  }

  // Call callbacks to get the rest of the system to fixup other untraced
  // pointers.
  callWeakPointerZonesCallbacks(&trc);
}

void GCRuntime::clearRelocatedArenas(Arena* arenaList, JS::GCReason reason) {
  AutoLockGC lock(this);
  clearRelocatedArenasWithoutUnlocking(arenaList, reason, lock);
}

void GCRuntime::clearRelocatedArenasWithoutUnlocking(Arena* arenaList,
                                                     JS::GCReason reason,
                                                     const AutoLockGC& lock) {
  // Clear the relocated arenas, now containing only forwarding pointers
  while (arenaList) {
    Arena* arena = arenaList;
    arenaList = arenaList->next;

    // Clear the mark bits
    arena->unmarkAll();

    // Mark arena as empty
    arena->setAsFullyUnused();

#ifdef DEBUG
    // The cell contents have been partially marked no access in RelocateCell,
    // so we need to mark the region as undefined again so we can poison it.
    SetMemCheckKind(reinterpret_cast<void*>(arena->thingsStart()),
                    arena->getThingsSpan(), MemCheckKind::MakeUndefined);
#endif

    AlwaysPoison(reinterpret_cast<void*>(arena->thingsStart()),
                 JS_MOVED_TENURED_PATTERN, arena->getThingsSpan(),
                 MemCheckKind::MakeNoAccess);

    // Don't count emptied arenas as being freed by the current GC:
    //  - if we purposely moved everything to new arenas, as that will already
    //    have allocated a similar number of arenas. (This only happens for
    //    collections triggered by GC zeal.)
    //  - if they were allocated since the start of the GC.
    bool allArenasRelocated = ShouldRelocateAllArenas(reason);
    bool updateRetainedSize = !allArenasRelocated && !arena->isNewlyCreated();
    arena->zone->gcHeapSize.removeBytes(ArenaSize, updateRetainedSize,
                                        heapSize);

    // Release the arena but don't return it to the chunk yet.
    arena->release(lock);
  }
}

#ifdef DEBUG

// In debug mode we don't always release relocated arenas straight away.
// Sometimes protect them instead and hold onto them until the next GC sweep
// phase to catch any pointers to them that didn't get forwarded.

static inline bool CanProtectArenas() {
  // On some systems the page size is larger than the size of an arena so we
  // can't change the mapping permissions per arena.
  return SystemPageSize() <= ArenaSize;
}

static inline bool ShouldProtectRelocatedArenas(JS::GCReason reason) {
  // For zeal mode collections we don't release the relocated arenas
  // immediately. Instead we protect them and keep them around until the next
  // collection so we can catch any stray accesses to them.
  return reason == JS::GCReason::DEBUG_GC && CanProtectArenas();
}

void GCRuntime::protectOrReleaseRelocatedArenas(Arena* arenaList,
                                                JS::GCReason reason) {
  if (ShouldProtectRelocatedArenas(reason)) {
    protectAndHoldArenas(arenaList);
    return;
  }

  releaseRelocatedArenas(arenaList);
}

void GCRuntime::protectAndHoldArenas(Arena* arenaList) {
  for (Arena* arena = arenaList; arena;) {
    MOZ_ASSERT(!arena->allocated());
    Arena* next = arena->next;
    if (!next) {
      // Prepend to hold list before we protect the memory.
      AutoLockGC lock(this);
      arena->next = relocatedArenasToRelease;
      relocatedArenasToRelease = arenaList;
    }
    ProtectPages(arena, ArenaSize);
    arena = next;
  }
}

void GCRuntime::unprotectHeldRelocatedArenas(const AutoLockGC& lock) {
  for (Arena* arena = relocatedArenasToRelease; arena; arena = arena->next) {
    UnprotectPages(arena, ArenaSize);
    MOZ_ASSERT(!arena->allocated());
  }
}

void GCRuntime::releaseHeldRelocatedArenas() {
  AutoLockGC lock(this);
  unprotectHeldRelocatedArenas(lock);
  Arena* arenas = relocatedArenasToRelease;
  relocatedArenasToRelease = nullptr;
  releaseRelocatedArenasWithoutUnlocking(arenas, lock);
}

void GCRuntime::releaseHeldRelocatedArenasWithoutUnlocking(
    const AutoLockGC& lock) {
  unprotectHeldRelocatedArenas(lock);
  releaseRelocatedArenasWithoutUnlocking(relocatedArenasToRelease, lock);
  relocatedArenasToRelease = nullptr;
}

#endif

void GCRuntime::releaseRelocatedArenas(Arena* arenaList) {
  AutoLockGC lock(this);
  releaseRelocatedArenasWithoutUnlocking(arenaList, lock);
}

void GCRuntime::releaseRelocatedArenasWithoutUnlocking(Arena* arenaList,
                                                       const AutoLockGC& lock) {
  // Release relocated arenas previously cleared with clearRelocatedArenas().
  while (arenaList) {
    Arena* arena = arenaList;
    arenaList = arenaList->next;

    // We already updated the memory accounting so just call
    // Chunk::releaseArena.
    arena->chunk()->releaseArena(this, arena, lock);
  }
}
