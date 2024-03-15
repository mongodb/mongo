/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * [SMDOC] Garbage Collector
 *
 * This code implements an incremental mark-and-sweep garbage collector, with
 * most sweeping carried out in the background on a parallel thread.
 *
 * Full vs. zone GC
 * ----------------
 *
 * The collector can collect all zones at once, or a subset. These types of
 * collection are referred to as a full GC and a zone GC respectively.
 *
 * It is possible for an incremental collection that started out as a full GC to
 * become a zone GC if new zones are created during the course of the
 * collection.
 *
 * Incremental collection
 * ----------------------
 *
 * For a collection to be carried out incrementally the following conditions
 * must be met:
 *  - the collection must be run by calling js::GCSlice() rather than js::GC()
 *  - the GC parameter JSGC_INCREMENTAL_GC_ENABLED must be true.
 *
 * The last condition is an engine-internal mechanism to ensure that incremental
 * collection is not carried out without the correct barriers being implemented.
 * For more information see 'Incremental marking' below.
 *
 * If the collection is not incremental, all foreground activity happens inside
 * a single call to GC() or GCSlice(). However the collection is not complete
 * until the background sweeping activity has finished.
 *
 * An incremental collection proceeds as a series of slices, interleaved with
 * mutator activity, i.e. running JavaScript code. Slices are limited by a time
 * budget. The slice finishes as soon as possible after the requested time has
 * passed.
 *
 * Collector states
 * ----------------
 *
 * The collector proceeds through the following states, the current state being
 * held in JSRuntime::gcIncrementalState:
 *
 *  - Prepare    - unmarks GC things, discards JIT code and other setup
 *  - MarkRoots  - marks the stack and other roots
 *  - Mark       - incrementally marks reachable things
 *  - Sweep      - sweeps zones in groups and continues marking unswept zones
 *  - Finalize   - performs background finalization, concurrent with mutator
 *  - Compact    - incrementally compacts by zone
 *  - Decommit   - performs background decommit and chunk removal
 *
 * Roots are marked in the first MarkRoots slice; this is the start of the GC
 * proper. The following states can take place over one or more slices.
 *
 * In other words an incremental collection proceeds like this:
 *
 * Slice 1:   Prepare:    Starts background task to unmark GC things
 *
 *          ... JS code runs, background unmarking finishes ...
 *
 * Slice 2:   MarkRoots:  Roots are pushed onto the mark stack.
 *            Mark:       The mark stack is processed by popping an element,
 *                        marking it, and pushing its children.
 *
 *          ... JS code runs ...
 *
 * Slice 3:   Mark:       More mark stack processing.
 *
 *          ... JS code runs ...
 *
 * Slice n-1: Mark:       More mark stack processing.
 *
 *          ... JS code runs ...
 *
 * Slice n:   Mark:       Mark stack is completely drained.
 *            Sweep:      Select first group of zones to sweep and sweep them.
 *
 *          ... JS code runs ...
 *
 * Slice n+1: Sweep:      Mark objects in unswept zones that were newly
 *                        identified as alive (see below). Then sweep more zone
 *                        sweep groups.
 *
 *          ... JS code runs ...
 *
 * Slice n+2: Sweep:      Mark objects in unswept zones that were newly
 *                        identified as alive. Then sweep more zones.
 *
 *          ... JS code runs ...
 *
 * Slice m:   Sweep:      Sweeping is finished, and background sweeping
 *                        started on the helper thread.
 *
 *          ... JS code runs, remaining sweeping done on background thread ...
 *
 * When background sweeping finishes the GC is complete.
 *
 * Incremental marking
 * -------------------
 *
 * Incremental collection requires close collaboration with the mutator (i.e.,
 * JS code) to guarantee correctness.
 *
 *  - During an incremental GC, if a memory location (except a root) is written
 *    to, then the value it previously held must be marked. Write barriers
 *    ensure this.
 *
 *  - Any object that is allocated during incremental GC must start out marked.
 *
 *  - Roots are marked in the first slice and hence don't need write barriers.
 *    Roots are things like the C stack and the VM stack.
 *
 * The problem that write barriers solve is that between slices the mutator can
 * change the object graph. We must ensure that it cannot do this in such a way
 * that makes us fail to mark a reachable object (marking an unreachable object
 * is tolerable).
 *
 * We use a snapshot-at-the-beginning algorithm to do this. This means that we
 * promise to mark at least everything that is reachable at the beginning of
 * collection. To implement it we mark the old contents of every non-root memory
 * location written to by the mutator while the collection is in progress, using
 * write barriers. This is described in gc/Barrier.h.
 *
 * Incremental sweeping
 * --------------------
 *
 * Sweeping is difficult to do incrementally because object finalizers must be
 * run at the start of sweeping, before any mutator code runs. The reason is
 * that some objects use their finalizers to remove themselves from caches. If
 * mutator code was allowed to run after the start of sweeping, it could observe
 * the state of the cache and create a new reference to an object that was just
 * about to be destroyed.
 *
 * Sweeping all finalizable objects in one go would introduce long pauses, so
 * instead sweeping broken up into groups of zones. Zones which are not yet
 * being swept are still marked, so the issue above does not apply.
 *
 * The order of sweeping is restricted by cross compartment pointers - for
 * example say that object |a| from zone A points to object |b| in zone B and
 * neither object was marked when we transitioned to the Sweep phase. Imagine we
 * sweep B first and then return to the mutator. It's possible that the mutator
 * could cause |a| to become alive through a read barrier (perhaps it was a
 * shape that was accessed via a shape table). Then we would need to mark |b|,
 * which |a| points to, but |b| has already been swept.
 *
 * So if there is such a pointer then marking of zone B must not finish before
 * marking of zone A.  Pointers which form a cycle between zones therefore
 * restrict those zones to being swept at the same time, and these are found
 * using Tarjan's algorithm for finding the strongly connected components of a
 * graph.
 *
 * GC things without finalizers, and things with finalizers that are able to run
 * in the background, are swept on the background thread. This accounts for most
 * of the sweeping work.
 *
 * Reset
 * -----
 *
 * During incremental collection it is possible, although unlikely, for
 * conditions to change such that incremental collection is no longer safe. In
 * this case, the collection is 'reset' by resetIncrementalGC(). If we are in
 * the mark state, this just stops marking, but if we have started sweeping
 * already, we continue non-incrementally until we have swept the current sweep
 * group. Following a reset, a new collection is started.
 *
 * Compacting GC
 * -------------
 *
 * Compacting GC happens at the end of a major GC as part of the last slice.
 * There are three parts:
 *
 *  - Arenas are selected for compaction.
 *  - The contents of those arenas are moved to new arenas.
 *  - All references to moved things are updated.
 *
 * Collecting Atoms
 * ----------------
 *
 * Atoms are collected differently from other GC things. They are contained in
 * a special zone and things in other zones may have pointers to them that are
 * not recorded in the cross compartment pointer map. Each zone holds a bitmap
 * with the atoms it might be keeping alive, and atoms are only collected if
 * they are not included in any zone's atom bitmap. See AtomMarking.cpp for how
 * this bitmap is managed.
 */

#include "gc/GC-inl.h"

#include "mozilla/Range.h"
#include "mozilla/ScopeExit.h"
#include "mozilla/TextUtils.h"
#include "mozilla/TimeStamp.h"

#include <algorithm>
#include <initializer_list>
#include <iterator>
#include <stdlib.h>
#include <string.h>
#include <utility>

#include "jsapi.h"  // JS_AbortIfWrongThread
#include "jstypes.h"

#include "debugger/DebugAPI.h"
#include "gc/ClearEdgesTracer.h"
#include "gc/GCContext.h"
#include "gc/GCInternals.h"
#include "gc/GCLock.h"
#include "gc/GCProbes.h"
#include "gc/Memory.h"
#include "gc/ParallelMarking.h"
#include "gc/ParallelWork.h"
#include "gc/WeakMap.h"
#include "jit/ExecutableAllocator.h"
#include "jit/JitCode.h"
#include "jit/JitRealm.h"
#include "jit/JitRuntime.h"
#include "jit/ProcessExecutableMemory.h"
#include "js/HeapAPI.h"  // JS::GCCellPtr
#include "js/Printer.h"
#include "js/SliceBudget.h"
#include "util/DifferentialTesting.h"
#include "vm/BigIntType.h"
#include "vm/EnvironmentObject.h"
#include "vm/GetterSetter.h"
#include "vm/HelperThreadState.h"
#include "vm/JitActivation.h"
#include "vm/JSObject.h"
#include "vm/JSScript.h"
#include "vm/PropMap.h"
#include "vm/Realm.h"
#include "vm/Shape.h"
#include "vm/StringType.h"
#include "vm/SymbolType.h"
#include "vm/Time.h"

#include "gc/Heap-inl.h"
#include "gc/Nursery-inl.h"
#include "gc/ObjectKind-inl.h"
#include "gc/PrivateIterators-inl.h"
#include "vm/GeckoProfiler-inl.h"
#include "vm/JSContext-inl.h"
#include "vm/Realm-inl.h"
#include "vm/Stack-inl.h"

using namespace js;
using namespace js::gc;

using mozilla::MakeScopeExit;
using mozilla::Maybe;
using mozilla::Nothing;
using mozilla::Some;
using mozilla::TimeDuration;
using mozilla::TimeStamp;

using JS::AutoGCRooter;

const AllocKind gc::slotsToThingKind[] = {
    // clang-format off
    /*  0 */ AllocKind::OBJECT0,  AllocKind::OBJECT2,  AllocKind::OBJECT2,  AllocKind::OBJECT4,
    /*  4 */ AllocKind::OBJECT4,  AllocKind::OBJECT8,  AllocKind::OBJECT8,  AllocKind::OBJECT8,
    /*  8 */ AllocKind::OBJECT8,  AllocKind::OBJECT12, AllocKind::OBJECT12, AllocKind::OBJECT12,
    /* 12 */ AllocKind::OBJECT12, AllocKind::OBJECT16, AllocKind::OBJECT16, AllocKind::OBJECT16,
    /* 16 */ AllocKind::OBJECT16
    // clang-format on
};

static_assert(std::size(slotsToThingKind) == SLOTS_TO_THING_KIND_LIMIT,
              "We have defined a slot count for each kind.");

MOZ_THREAD_LOCAL(JS::GCContext*) js::TlsGCContext;

JS::GCContext::GCContext(JSRuntime* runtime) : runtime_(runtime) {}

JS::GCContext::~GCContext() {
  MOZ_ASSERT(!hasJitCodeToPoison());
  MOZ_ASSERT(!isCollecting());
  MOZ_ASSERT(gcUse() == GCUse::None);
  MOZ_ASSERT(!gcSweepZone());
  MOZ_ASSERT(!isTouchingGrayThings());
}

void JS::GCContext::poisonJitCode() {
  if (hasJitCodeToPoison()) {
    jit::ExecutableAllocator::poisonCode(runtime(), jitPoisonRanges);
    jitPoisonRanges.clearAndFree();
  }
}

#ifdef DEBUG
void GCRuntime::verifyAllChunks() {
  AutoLockGC lock(this);
  fullChunks(lock).verifyChunks();
  availableChunks(lock).verifyChunks();
  emptyChunks(lock).verifyChunks();
}
#endif

void GCRuntime::setMinEmptyChunkCount(uint32_t value, const AutoLockGC& lock) {
  minEmptyChunkCount_ = value;
  if (minEmptyChunkCount_ > maxEmptyChunkCount_) {
    maxEmptyChunkCount_ = minEmptyChunkCount_;
  }
  MOZ_ASSERT(maxEmptyChunkCount_ >= minEmptyChunkCount_);
}

void GCRuntime::setMaxEmptyChunkCount(uint32_t value, const AutoLockGC& lock) {
  maxEmptyChunkCount_ = value;
  if (minEmptyChunkCount_ > maxEmptyChunkCount_) {
    minEmptyChunkCount_ = maxEmptyChunkCount_;
  }
  MOZ_ASSERT(maxEmptyChunkCount_ >= minEmptyChunkCount_);
}

inline bool GCRuntime::tooManyEmptyChunks(const AutoLockGC& lock) {
  return emptyChunks(lock).count() > minEmptyChunkCount(lock);
}

ChunkPool GCRuntime::expireEmptyChunkPool(const AutoLockGC& lock) {
  MOZ_ASSERT(emptyChunks(lock).verify());
  MOZ_ASSERT(minEmptyChunkCount(lock) <= maxEmptyChunkCount(lock));

  ChunkPool expired;
  while (tooManyEmptyChunks(lock)) {
    TenuredChunk* chunk = emptyChunks(lock).pop();
    prepareToFreeChunk(chunk->info);
    expired.push(chunk);
  }

  MOZ_ASSERT(expired.verify());
  MOZ_ASSERT(emptyChunks(lock).verify());
  MOZ_ASSERT(emptyChunks(lock).count() <= maxEmptyChunkCount(lock));
  MOZ_ASSERT(emptyChunks(lock).count() <= minEmptyChunkCount(lock));
  return expired;
}

static void FreeChunkPool(ChunkPool& pool) {
  for (ChunkPool::Iter iter(pool); !iter.done();) {
    TenuredChunk* chunk = iter.get();
    iter.next();
    pool.remove(chunk);
    MOZ_ASSERT(chunk->unused());
    UnmapPages(static_cast<void*>(chunk), ChunkSize);
  }
  MOZ_ASSERT(pool.count() == 0);
}

void GCRuntime::freeEmptyChunks(const AutoLockGC& lock) {
  FreeChunkPool(emptyChunks(lock));
}

inline void GCRuntime::prepareToFreeChunk(TenuredChunkInfo& info) {
  MOZ_ASSERT(numArenasFreeCommitted >= info.numArenasFreeCommitted);
  numArenasFreeCommitted -= info.numArenasFreeCommitted;
  stats().count(gcstats::COUNT_DESTROY_CHUNK);
#ifdef DEBUG
  /*
   * Let FreeChunkPool detect a missing prepareToFreeChunk call before it
   * frees chunk.
   */
  info.numArenasFreeCommitted = 0;
#endif
}

void GCRuntime::releaseArena(Arena* arena, const AutoLockGC& lock) {
  MOZ_ASSERT(arena->allocated());
  MOZ_ASSERT(!arena->onDelayedMarkingList());
  MOZ_ASSERT(TlsGCContext.get()->isFinalizing());

  arena->zone->gcHeapSize.removeGCArena(heapSize);
  arena->release(lock);
  arena->chunk()->releaseArena(this, arena, lock);
}

GCRuntime::GCRuntime(JSRuntime* rt)
    : rt(rt),
      systemZone(nullptr),
      mainThreadContext(rt),
      heapState_(JS::HeapState::Idle),
      stats_(this),
      sweepingTracer(rt),
      fullGCRequested(false),
      helperThreadRatio(TuningDefaults::HelperThreadRatio),
      maxHelperThreads(TuningDefaults::MaxHelperThreads),
      helperThreadCount(1),
      createBudgetCallback(nullptr),
      minEmptyChunkCount_(TuningDefaults::MinEmptyChunkCount),
      maxEmptyChunkCount_(TuningDefaults::MaxEmptyChunkCount),
      rootsHash(256),
      nextCellUniqueId_(LargestTaggedNullCellPointer +
                        1),  // Ensure disjoint from null tagged pointers.
      numArenasFreeCommitted(0),
      verifyPreData(nullptr),
      lastGCStartTime_(TimeStamp::Now()),
      lastGCEndTime_(TimeStamp::Now()),
      incrementalGCEnabled(TuningDefaults::IncrementalGCEnabled),
      perZoneGCEnabled(TuningDefaults::PerZoneGCEnabled),
      numActiveZoneIters(0),
      cleanUpEverything(false),
      grayBitsValid(true),
      majorGCTriggerReason(JS::GCReason::NO_REASON),
      minorGCNumber(0),
      majorGCNumber(0),
      number(0),
      sliceNumber(0),
      isFull(false),
      incrementalState(gc::State::NotActive),
      initialState(gc::State::NotActive),
      useZeal(false),
      lastMarkSlice(false),
      safeToYield(true),
      markOnBackgroundThreadDuringSweeping(false),
      useBackgroundThreads(false),
#ifdef DEBUG
      hadShutdownGC(false),
#endif
      requestSliceAfterBackgroundTask(false),
      lifoBlocksToFree((size_t)JSContext::TEMP_LIFO_ALLOC_PRIMARY_CHUNK_SIZE),
      lifoBlocksToFreeAfterMinorGC(
          (size_t)JSContext::TEMP_LIFO_ALLOC_PRIMARY_CHUNK_SIZE),
      sweepGroupIndex(0),
      sweepGroups(nullptr),
      currentSweepGroup(nullptr),
      sweepZone(nullptr),
      abortSweepAfterCurrentGroup(false),
      sweepMarkResult(IncrementalProgress::NotFinished),
#ifdef DEBUG
      testMarkQueue(rt),
#endif
      startedCompacting(false),
      zonesCompacted(0),
#ifdef DEBUG
      relocatedArenasToRelease(nullptr),
#endif
#ifdef JS_GC_ZEAL
      markingValidator(nullptr),
#endif
      defaultTimeBudgetMS_(TuningDefaults::DefaultTimeBudgetMS),
      incrementalAllowed(true),
      compactingEnabled(TuningDefaults::CompactingEnabled),
      parallelMarkingEnabled(TuningDefaults::ParallelMarkingEnabled),
      rootsRemoved(false),
#ifdef JS_GC_ZEAL
      zealModeBits(0),
      zealFrequency(0),
      nextScheduled(0),
      deterministicOnly(false),
      zealSliceBudget(0),
      selectedForMarking(rt),
#endif
      fullCompartmentChecks(false),
      gcCallbackDepth(0),
      alwaysPreserveCode(false),
      lowMemoryState(false),
      lock(mutexid::GCLock),
      delayedMarkingLock(mutexid::GCDelayedMarkingLock),
      allocTask(this, emptyChunks_.ref()),
      unmarkTask(this),
      markTask(this),
      sweepTask(this),
      freeTask(this),
      decommitTask(this),
      nursery_(this),
      storeBuffer_(rt, nursery()),
      lastAllocRateUpdateTime(TimeStamp::Now()) {
}

using CharRange = mozilla::Range<const char>;
using CharRangeVector = Vector<CharRange, 0, SystemAllocPolicy>;

static bool SplitStringBy(CharRange text, char delimiter,
                          CharRangeVector* result) {
  auto start = text.begin();
  for (auto ptr = start; ptr != text.end(); ptr++) {
    if (*ptr == delimiter) {
      if (!result->emplaceBack(start, ptr)) {
        return false;
      }
      start = ptr + 1;
    }
  }

  return result->emplaceBack(start, text.end());
}

static bool ParseTimeDuration(CharRange text, TimeDuration* durationOut) {
  const char* str = text.begin().get();
  char* end;
  *durationOut = TimeDuration::FromMilliseconds(strtol(str, &end, 10));
  return str != end && end == text.end().get();
}

static void PrintProfileHelpAndExit(const char* envName, const char* helpText) {
  fprintf(stderr, "%s=N[,(main|all)]\n", envName);
  fprintf(stderr, "%s", helpText);
  exit(0);
}

void js::gc::ReadProfileEnv(const char* envName, const char* helpText,
                            bool* enableOut, bool* workersOut,
                            TimeDuration* thresholdOut) {
  *enableOut = false;
  *workersOut = false;
  *thresholdOut = TimeDuration();

  const char* env = getenv(envName);
  if (!env) {
    return;
  }

  if (strcmp(env, "help") == 0) {
    PrintProfileHelpAndExit(envName, helpText);
  }

  CharRangeVector parts;
  auto text = CharRange(env, strlen(env));
  if (!SplitStringBy(text, ',', &parts)) {
    MOZ_CRASH("OOM parsing environment variable");
  }

  if (parts.length() == 0 || parts.length() > 2) {
    PrintProfileHelpAndExit(envName, helpText);
  }

  *enableOut = true;

  if (!ParseTimeDuration(parts[0], thresholdOut)) {
    PrintProfileHelpAndExit(envName, helpText);
  }

  if (parts.length() == 2) {
    const char* threads = parts[1].begin().get();
    if (strcmp(threads, "all") == 0) {
      *workersOut = true;
    } else if (strcmp(threads, "main") != 0) {
      PrintProfileHelpAndExit(envName, helpText);
    }
  }
}

bool js::gc::ShouldPrintProfile(JSRuntime* runtime, bool enable,
                                bool profileWorkers, TimeDuration threshold,
                                TimeDuration duration) {
  return enable && (runtime->isMainRuntime() || profileWorkers) &&
         duration >= threshold;
}

#ifdef JS_GC_ZEAL

void GCRuntime::getZealBits(uint32_t* zealBits, uint32_t* frequency,
                            uint32_t* scheduled) {
  *zealBits = zealModeBits;
  *frequency = zealFrequency;
  *scheduled = nextScheduled;
}

const char gc::ZealModeHelpText[] =
    "  Specifies how zealous the garbage collector should be. Some of these "
    "modes can\n"
    "  be set simultaneously, by passing multiple level options, e.g. \"2;4\" "
    "will activate\n"
    "  both modes 2 and 4. Modes can be specified by name or number.\n"
    "  \n"
    "  Values:\n"
    "    0:  (None) Normal amount of collection (resets all modes)\n"
    "    1:  (RootsChange) Collect when roots are added or removed\n"
    "    2:  (Alloc) Collect when every N allocations (default: 100)\n"
    "    4:  (VerifierPre) Verify pre write barriers between instructions\n"
    "    6:  (YieldBeforeRootMarking) Incremental GC in two slices that yields "
    "before root marking\n"
    "    7:  (GenerationalGC) Collect the nursery every N nursery allocations\n"
    "    8:  (YieldBeforeMarking) Incremental GC in two slices that yields "
    "between\n"
    "        the root marking and marking phases\n"
    "    9:  (YieldBeforeSweeping) Incremental GC in two slices that yields "
    "between\n"
    "        the marking and sweeping phases\n"
    "    10: (IncrementalMultipleSlices) Incremental GC in many slices\n"
    "    11: (IncrementalMarkingValidator) Verify incremental marking\n"
    "    12: (ElementsBarrier) Use the individual element post-write barrier\n"
    "        regardless of elements size\n"
    "    13: (CheckHashTablesOnMinorGC) Check internal hashtables on minor GC\n"
    "    14: (Compact) Perform a shrinking collection every N allocations\n"
    "    15: (CheckHeapAfterGC) Walk the heap to check its integrity after "
    "every GC\n"
    "    17: (YieldBeforeSweepingAtoms) Incremental GC in two slices that "
    "yields\n"
    "        before sweeping the atoms table\n"
    "    18: (CheckGrayMarking) Check gray marking invariants after every GC\n"
    "    19: (YieldBeforeSweepingCaches) Incremental GC in two slices that "
    "yields\n"
    "        before sweeping weak caches\n"
    "    21: (YieldBeforeSweepingObjects) Incremental GC in two slices that "
    "yields\n"
    "        before sweeping foreground finalized objects\n"
    "    22: (YieldBeforeSweepingNonObjects) Incremental GC in two slices that "
    "yields\n"
    "        before sweeping non-object GC things\n"
    "    23: (YieldBeforeSweepingPropMapTrees) Incremental GC in two slices "
    "that "
    "yields\n"
    "        before sweeping shape trees\n"
    "    24: (CheckWeakMapMarking) Check weak map marking invariants after "
    "every GC\n"
    "    25: (YieldWhileGrayMarking) Incremental GC in two slices that yields\n"
    "        during gray marking\n";

// The set of zeal modes that control incremental slices. These modes are
// mutually exclusive.
static const mozilla::EnumSet<ZealMode> IncrementalSliceZealModes = {
    ZealMode::YieldBeforeRootMarking,
    ZealMode::YieldBeforeMarking,
    ZealMode::YieldBeforeSweeping,
    ZealMode::IncrementalMultipleSlices,
    ZealMode::YieldBeforeSweepingAtoms,
    ZealMode::YieldBeforeSweepingCaches,
    ZealMode::YieldBeforeSweepingObjects,
    ZealMode::YieldBeforeSweepingNonObjects,
    ZealMode::YieldBeforeSweepingPropMapTrees};

void GCRuntime::setZeal(uint8_t zeal, uint32_t frequency) {
  MOZ_ASSERT(zeal <= unsigned(ZealMode::Limit));

  if (verifyPreData) {
    VerifyBarriers(rt, PreBarrierVerifier);
  }

  if (zeal == 0) {
    if (hasZealMode(ZealMode::GenerationalGC)) {
      evictNursery(JS::GCReason::DEBUG_GC);
      nursery().leaveZealMode();
    }

    if (isIncrementalGCInProgress()) {
      finishGC(JS::GCReason::DEBUG_GC);
    }
  }

  ZealMode zealMode = ZealMode(zeal);
  if (zealMode == ZealMode::GenerationalGC) {
    evictNursery(JS::GCReason::DEBUG_GC);
    nursery().enterZealMode();
  }

  // Some modes are mutually exclusive. If we're setting one of those, we
  // first reset all of them.
  if (IncrementalSliceZealModes.contains(zealMode)) {
    for (auto mode : IncrementalSliceZealModes) {
      clearZealMode(mode);
    }
  }

  bool schedule = zealMode >= ZealMode::Alloc;
  if (zeal != 0) {
    zealModeBits |= 1 << unsigned(zeal);
  } else {
    zealModeBits = 0;
  }
  zealFrequency = frequency;
  nextScheduled = schedule ? frequency : 0;
}

void GCRuntime::unsetZeal(uint8_t zeal) {
  MOZ_ASSERT(zeal <= unsigned(ZealMode::Limit));
  ZealMode zealMode = ZealMode(zeal);

  if (!hasZealMode(zealMode)) {
    return;
  }

  if (verifyPreData) {
    VerifyBarriers(rt, PreBarrierVerifier);
  }

  if (zealMode == ZealMode::GenerationalGC) {
    evictNursery(JS::GCReason::DEBUG_GC);
    nursery().leaveZealMode();
  }

  clearZealMode(zealMode);

  if (zealModeBits == 0) {
    if (isIncrementalGCInProgress()) {
      finishGC(JS::GCReason::DEBUG_GC);
    }

    zealFrequency = 0;
    nextScheduled = 0;
  }
}

void GCRuntime::setNextScheduled(uint32_t count) { nextScheduled = count; }

static bool ParseZealModeName(CharRange text, uint32_t* modeOut) {
  struct ModeInfo {
    const char* name;
    size_t length;
    uint32_t value;
  };

  static const ModeInfo zealModes[] = {{"None", 0},
#  define ZEAL_MODE(name, value) {#name, strlen(#name), value},
                                       JS_FOR_EACH_ZEAL_MODE(ZEAL_MODE)
#  undef ZEAL_MODE
  };

  for (auto mode : zealModes) {
    if (text.length() == mode.length &&
        memcmp(text.begin().get(), mode.name, mode.length) == 0) {
      *modeOut = mode.value;
      return true;
    }
  }

  return false;
}

static bool ParseZealModeNumericParam(CharRange text, uint32_t* paramOut) {
  if (text.length() == 0) {
    return false;
  }

  for (auto c : text) {
    if (!mozilla::IsAsciiDigit(c)) {
      return false;
    }
  }

  *paramOut = atoi(text.begin().get());
  return true;
}

static bool PrintZealHelpAndFail() {
  fprintf(stderr, "Format: JS_GC_ZEAL=level(;level)*[,N]\n");
  fputs(ZealModeHelpText, stderr);
  return false;
}

bool GCRuntime::parseAndSetZeal(const char* str) {
  // Set the zeal mode from a string consisting of one or more mode specifiers
  // separated by ';', optionally followed by a ',' and the trigger frequency.
  // The mode specifiers can by a mode name or its number.

  auto text = CharRange(str, strlen(str));

  CharRangeVector parts;
  if (!SplitStringBy(text, ',', &parts)) {
    return false;
  }

  if (parts.length() == 0 || parts.length() > 2) {
    return PrintZealHelpAndFail();
  }

  uint32_t frequency = JS_DEFAULT_ZEAL_FREQ;
  if (parts.length() == 2 && !ParseZealModeNumericParam(parts[1], &frequency)) {
    return PrintZealHelpAndFail();
  }

  CharRangeVector modes;
  if (!SplitStringBy(parts[0], ';', &modes)) {
    return false;
  }

  for (const auto& descr : modes) {
    uint32_t mode;
    if (!ParseZealModeName(descr, &mode) &&
        !(ParseZealModeNumericParam(descr, &mode) &&
          mode <= unsigned(ZealMode::Limit))) {
      return PrintZealHelpAndFail();
    }

    setZeal(mode, frequency);
  }

  return true;
}

const char* js::gc::AllocKindName(AllocKind kind) {
  static const char* const names[] = {
#  define EXPAND_THING_NAME(allocKind, _1, _2, _3, _4, _5, _6) #allocKind,
      FOR_EACH_ALLOCKIND(EXPAND_THING_NAME)
#  undef EXPAND_THING_NAME
  };
  static_assert(std::size(names) == AllocKindCount,
                "names array should have an entry for every AllocKind");

  size_t i = size_t(kind);
  MOZ_ASSERT(i < std::size(names));
  return names[i];
}

void js::gc::DumpArenaInfo() {
  fprintf(stderr, "Arena header size: %zu\n\n", ArenaHeaderSize);

  fprintf(stderr, "GC thing kinds:\n");
  fprintf(stderr, "%25s %8s %8s %8s\n",
          "AllocKind:", "Size:", "Count:", "Padding:");
  for (auto kind : AllAllocKinds()) {
    fprintf(stderr, "%25s %8zu %8zu %8zu\n", AllocKindName(kind),
            Arena::thingSize(kind), Arena::thingsPerArena(kind),
            Arena::firstThingOffset(kind) - ArenaHeaderSize);
  }
}

#endif  // JS_GC_ZEAL

bool GCRuntime::init(uint32_t maxbytes) {
  MOZ_ASSERT(!wasInitialized());

  MOZ_ASSERT(SystemPageSize());
  Arena::checkLookupTables();

  if (!TlsGCContext.init()) {
    return false;
  }
  TlsGCContext.set(&mainThreadContext.ref());

  updateHelperThreadCount();

#ifdef JS_GC_ZEAL
  const char* size = getenv("JSGC_MARK_STACK_LIMIT");
  if (size) {
    maybeMarkStackLimit = atoi(size);
  }
#endif

  if (!updateMarkersVector()) {
    return false;
  }

  {
    AutoLockGCBgAlloc lock(this);

    MOZ_ALWAYS_TRUE(tunables.setParameter(JSGC_MAX_BYTES, maxbytes));

    if (!nursery().init(lock)) {
      return false;
    }

    const char* pretenureThresholdStr = getenv("JSGC_PRETENURE_THRESHOLD");
    if (pretenureThresholdStr && pretenureThresholdStr[0]) {
      char* last;
      long pretenureThreshold = strtol(pretenureThresholdStr, &last, 10);
      if (last[0] || !tunables.setParameter(JSGC_PRETENURE_THRESHOLD,
                                            pretenureThreshold)) {
        fprintf(stderr, "Invalid value for JSGC_PRETENURE_THRESHOLD: %s\n",
                pretenureThresholdStr);
      }
    }
  }

#ifdef JS_GC_ZEAL
  const char* zealSpec = getenv("JS_GC_ZEAL");
  if (zealSpec && zealSpec[0] && !parseAndSetZeal(zealSpec)) {
    return false;
  }
#endif

  for (auto& marker : markers) {
    if (!marker->init()) {
      return false;
    }
  }

  if (!initSweepActions()) {
    return false;
  }

  UniquePtr<Zone> zone = MakeUnique<Zone>(rt, Zone::AtomsZone);
  if (!zone || !zone->init()) {
    return false;
  }

  // The atoms zone is stored as the first element of the zones vector.
  MOZ_ASSERT(zone->isAtomsZone());
  MOZ_ASSERT(zones().empty());
  MOZ_ALWAYS_TRUE(zones().reserve(1));  // ZonesVector has inline capacity 4.
  zones().infallibleAppend(zone.release());

  gcprobes::Init(this);

  initialized = true;
  return true;
}

void GCRuntime::finish() {
  MOZ_ASSERT(inPageLoadCount == 0);
  MOZ_ASSERT(!sharedAtomsZone_);

  // Wait for nursery background free to end and disable it to release memory.
  if (nursery().isEnabled()) {
    nursery().disable();
  }

  // Wait until the background finalization and allocation stops and the
  // helper thread shuts down before we forcefully release any remaining GC
  // memory.
  sweepTask.join();
  markTask.join();
  freeTask.join();
  allocTask.cancelAndWait();
  decommitTask.cancelAndWait();

#ifdef JS_GC_ZEAL
  // Free memory associated with GC verification.
  finishVerifier();
#endif

  // Delete all remaining zones.
  for (ZonesIter zone(this, WithAtoms); !zone.done(); zone.next()) {
    AutoSetThreadIsSweeping threadIsSweeping(rt->gcContext(), zone);
    for (CompartmentsInZoneIter comp(zone); !comp.done(); comp.next()) {
      for (RealmsInCompartmentIter realm(comp); !realm.done(); realm.next()) {
        js_delete(realm.get());
      }
      comp->realms().clear();
      js_delete(comp.get());
    }
    zone->compartments().clear();
    js_delete(zone.get());
  }

  zones().clear();

  FreeChunkPool(fullChunks_.ref());
  FreeChunkPool(availableChunks_.ref());
  FreeChunkPool(emptyChunks_.ref());

  TlsGCContext.set(nullptr);

  gcprobes::Finish(this);

  nursery().printTotalProfileTimes();
  stats().printTotalProfileTimes();
}

bool GCRuntime::freezeSharedAtomsZone() {
  // This is called just after permanent atoms and well-known symbols have been
  // created. At this point all existing atoms and symbols are permanent.
  //
  // This method makes the current atoms zone into a shared atoms zone and
  // removes it from the zones list. Everything in it is marked black. A new
  // empty atoms zone is created, where all atoms local to this runtime will
  // live.
  //
  // The shared atoms zone will not be collected until shutdown when it is
  // returned to the zone list by restoreSharedAtomsZone().

  MOZ_ASSERT(rt->isMainRuntime());
  MOZ_ASSERT(!sharedAtomsZone_);
  MOZ_ASSERT(zones().length() == 1);
  MOZ_ASSERT(atomsZone());
  MOZ_ASSERT(!atomsZone()->wasGCStarted());
  MOZ_ASSERT(!atomsZone()->needsIncrementalBarrier());

  AutoAssertEmptyNursery nurseryIsEmpty(rt->mainContextFromOwnThread());

  atomsZone()->arenas.clearFreeLists();

  for (auto kind : AllAllocKinds()) {
    for (auto thing =
             atomsZone()->cellIterUnsafe<TenuredCell>(kind, nurseryIsEmpty);
         !thing.done(); thing.next()) {
      TenuredCell* cell = thing.getCell();
      MOZ_ASSERT((cell->is<JSString>() &&
                  cell->as<JSString>()->isPermanentAndMayBeShared()) ||
                 (cell->is<JS::Symbol>() &&
                  cell->as<JS::Symbol>()->isPermanentAndMayBeShared()));
      cell->markBlack();
    }
  }

  sharedAtomsZone_ = atomsZone();
  zones().clear();

  UniquePtr<Zone> zone = MakeUnique<Zone>(rt, Zone::AtomsZone);
  if (!zone || !zone->init()) {
    return false;
  }

  MOZ_ASSERT(zone->isAtomsZone());
  zones().infallibleAppend(zone.release());

  return true;
}

void GCRuntime::restoreSharedAtomsZone() {
  // Return the shared atoms zone to the zone list. This allows the contents of
  // the shared atoms zone to be collected when the parent runtime is shut down.

  if (!sharedAtomsZone_) {
    return;
  }

  MOZ_ASSERT(rt->isMainRuntime());
  MOZ_ASSERT(rt->childRuntimeCount == 0);

  AutoEnterOOMUnsafeRegion oomUnsafe;
  if (!zones().append(sharedAtomsZone_)) {
    oomUnsafe.crash("restoreSharedAtomsZone");
  }

  sharedAtomsZone_ = nullptr;
}

bool GCRuntime::setParameter(JSContext* cx, JSGCParamKey key, uint32_t value) {
  MOZ_ASSERT(CurrentThreadCanAccessRuntime(rt));

  AutoStopVerifyingBarriers pauseVerification(rt, false);
  FinishGC(cx);
  waitBackgroundSweepEnd();

  AutoLockGC lock(this);
  return setParameter(key, value, lock);
}

static bool IsGCThreadParameter(JSGCParamKey key) {
  return key == JSGC_HELPER_THREAD_RATIO || key == JSGC_MAX_HELPER_THREADS ||
         key == JSGC_MARKING_THREAD_COUNT;
}

bool GCRuntime::setParameter(JSGCParamKey key, uint32_t value,
                             AutoLockGC& lock) {
  switch (key) {
    case JSGC_SLICE_TIME_BUDGET_MS:
      defaultTimeBudgetMS_ = value;
      break;
    case JSGC_INCREMENTAL_GC_ENABLED:
      setIncrementalGCEnabled(value != 0);
      break;
    case JSGC_PER_ZONE_GC_ENABLED:
      perZoneGCEnabled = value != 0;
      break;
    case JSGC_COMPACTING_ENABLED:
      compactingEnabled = value != 0;
      break;
    case JSGC_PARALLEL_MARKING_ENABLED:
      // Not supported on workers.
      parallelMarkingEnabled = rt->isMainRuntime() && value != 0;
      updateMarkersVector();
      break;
    case JSGC_INCREMENTAL_WEAKMAP_ENABLED:
      for (auto& marker : markers) {
        marker->incrementalWeakMapMarkingEnabled = value != 0;
      }
      break;
    case JSGC_MIN_EMPTY_CHUNK_COUNT:
      setMinEmptyChunkCount(value, lock);
      break;
    case JSGC_MAX_EMPTY_CHUNK_COUNT:
      setMaxEmptyChunkCount(value, lock);
      break;
    default:
      if (IsGCThreadParameter(key)) {
        return setThreadParameter(key, value, lock);
      }

      if (!tunables.setParameter(key, value)) {
        return false;
      }
      updateAllGCStartThresholds();
  }

  return true;
}

bool GCRuntime::setThreadParameter(JSGCParamKey key, uint32_t value,
                                   AutoLockGC& lock) {
  if (rt->parentRuntime) {
    // Don't allow these to be set for worker runtimes.
    return false;
  }

  switch (key) {
    case JSGC_HELPER_THREAD_RATIO:
      if (value == 0) {
        return false;
      }
      helperThreadRatio = double(value) / 100.0;
      break;
    case JSGC_MAX_HELPER_THREADS:
      if (value == 0) {
        return false;
      }
      maxHelperThreads = value;
      break;
    case JSGC_MARKING_THREAD_COUNT:
      markingThreadCount = std::min(size_t(value), MaxParallelWorkers);
      break;
    default:
      MOZ_CRASH("Unexpected parameter key");
  }

  updateHelperThreadCount();
  updateMarkersVector();

  return true;
}

void GCRuntime::resetParameter(JSContext* cx, JSGCParamKey key) {
  MOZ_ASSERT(CurrentThreadCanAccessRuntime(rt));

  AutoStopVerifyingBarriers pauseVerification(rt, false);
  FinishGC(cx);
  waitBackgroundSweepEnd();

  AutoLockGC lock(this);
  resetParameter(key, lock);
}

void GCRuntime::resetParameter(JSGCParamKey key, AutoLockGC& lock) {
  switch (key) {
    case JSGC_SLICE_TIME_BUDGET_MS:
      defaultTimeBudgetMS_ = TuningDefaults::DefaultTimeBudgetMS;
      break;
    case JSGC_INCREMENTAL_GC_ENABLED:
      setIncrementalGCEnabled(TuningDefaults::IncrementalGCEnabled);
      break;
    case JSGC_PER_ZONE_GC_ENABLED:
      perZoneGCEnabled = TuningDefaults::PerZoneGCEnabled;
      break;
    case JSGC_COMPACTING_ENABLED:
      compactingEnabled = TuningDefaults::CompactingEnabled;
      break;
    case JSGC_PARALLEL_MARKING_ENABLED:
      parallelMarkingEnabled = TuningDefaults::ParallelMarkingEnabled;
      updateMarkersVector();
      break;
    case JSGC_INCREMENTAL_WEAKMAP_ENABLED:
      for (auto& marker : markers) {
        marker->incrementalWeakMapMarkingEnabled =
            TuningDefaults::IncrementalWeakMapMarkingEnabled;
      }
      break;
    case JSGC_MIN_EMPTY_CHUNK_COUNT:
      setMinEmptyChunkCount(TuningDefaults::MinEmptyChunkCount, lock);
      break;
    case JSGC_MAX_EMPTY_CHUNK_COUNT:
      setMaxEmptyChunkCount(TuningDefaults::MaxEmptyChunkCount, lock);
      break;
    default:
      if (IsGCThreadParameter(key)) {
        resetThreadParameter(key, lock);
        return;
      }

      tunables.resetParameter(key);
      updateAllGCStartThresholds();
  }
}

void GCRuntime::resetThreadParameter(JSGCParamKey key, AutoLockGC& lock) {
  if (rt->parentRuntime) {
    return;
  }

  switch (key) {
    case JSGC_HELPER_THREAD_RATIO:
      helperThreadRatio = TuningDefaults::HelperThreadRatio;
      break;
    case JSGC_MAX_HELPER_THREADS:
      maxHelperThreads = TuningDefaults::MaxHelperThreads;
      break;
    case JSGC_MARKING_THREAD_COUNT:
      markingThreadCount = 0;
      break;
    default:
      MOZ_CRASH("Unexpected parameter key");
  }

  updateHelperThreadCount();
  updateMarkersVector();
}

uint32_t GCRuntime::getParameter(JSGCParamKey key) {
  MOZ_ASSERT(CurrentThreadCanAccessRuntime(rt));
  AutoLockGC lock(this);
  return getParameter(key, lock);
}

uint32_t GCRuntime::getParameter(JSGCParamKey key, const AutoLockGC& lock) {
  switch (key) {
    case JSGC_BYTES:
      return uint32_t(heapSize.bytes());
    case JSGC_NURSERY_BYTES:
      return nursery().capacity();
    case JSGC_NUMBER:
      return uint32_t(number);
    case JSGC_MAJOR_GC_NUMBER:
      return uint32_t(majorGCNumber);
    case JSGC_MINOR_GC_NUMBER:
      return uint32_t(minorGCNumber);
    case JSGC_INCREMENTAL_GC_ENABLED:
      return incrementalGCEnabled;
    case JSGC_PER_ZONE_GC_ENABLED:
      return perZoneGCEnabled;
    case JSGC_UNUSED_CHUNKS:
      return uint32_t(emptyChunks(lock).count());
    case JSGC_TOTAL_CHUNKS:
      return uint32_t(fullChunks(lock).count() + availableChunks(lock).count() +
                      emptyChunks(lock).count());
    case JSGC_SLICE_TIME_BUDGET_MS:
      MOZ_RELEASE_ASSERT(defaultTimeBudgetMS_ >= 0);
      MOZ_RELEASE_ASSERT(defaultTimeBudgetMS_ <= UINT32_MAX);
      return uint32_t(defaultTimeBudgetMS_);
    case JSGC_MIN_EMPTY_CHUNK_COUNT:
      return minEmptyChunkCount(lock);
    case JSGC_MAX_EMPTY_CHUNK_COUNT:
      return maxEmptyChunkCount(lock);
    case JSGC_COMPACTING_ENABLED:
      return compactingEnabled;
    case JSGC_PARALLEL_MARKING_ENABLED:
      return parallelMarkingEnabled;
    case JSGC_INCREMENTAL_WEAKMAP_ENABLED:
      return marker().incrementalWeakMapMarkingEnabled;
    case JSGC_CHUNK_BYTES:
      return ChunkSize;
    case JSGC_HELPER_THREAD_RATIO:
      MOZ_ASSERT(helperThreadRatio > 0.0);
      return uint32_t(helperThreadRatio * 100.0);
    case JSGC_MAX_HELPER_THREADS:
      MOZ_ASSERT(maxHelperThreads <= UINT32_MAX);
      return maxHelperThreads;
    case JSGC_HELPER_THREAD_COUNT:
      return helperThreadCount;
    case JSGC_MARKING_THREAD_COUNT:
      return markingThreadCount;
    case JSGC_SYSTEM_PAGE_SIZE_KB:
      return SystemPageSize() / 1024;
    default:
      return tunables.getParameter(key);
  }
}

#ifdef JS_GC_ZEAL
void GCRuntime::setMarkStackLimit(size_t limit, AutoLockGC& lock) {
  MOZ_ASSERT(!JS::RuntimeHeapIsBusy());

  maybeMarkStackLimit = limit;

  AutoUnlockGC unlock(lock);
  AutoStopVerifyingBarriers pauseVerification(rt, false);
  for (auto& marker : markers) {
    marker->setMaxCapacity(limit);
  }
}
#endif

void GCRuntime::setIncrementalGCEnabled(bool enabled) {
  incrementalGCEnabled = enabled;
}

void GCRuntime::updateHelperThreadCount() {
  if (!CanUseExtraThreads()) {
    // startTask will run the work on the main thread if the count is 1.
    MOZ_ASSERT(helperThreadCount == 1);
    return;
  }

  // Number of extra threads required during parallel marking to ensure we can
  // start the necessary marking tasks. Background free and background
  // allocation may already be running and we want to avoid these tasks blocking
  // marking. In real configurations there will be enough threads that this
  // won't affect anything.
  static constexpr size_t SpareThreadsDuringParallelMarking = 2;

  // The count of helper threads used for GC tasks is process wide. Don't set it
  // for worker JS runtimes.
  if (rt->parentRuntime) {
    helperThreadCount = rt->parentRuntime->gc.helperThreadCount;
    return;
  }

  // Calculate the target thread count for GC parallel tasks.
  double cpuCount = GetHelperThreadCPUCount();
  helperThreadCount = std::clamp(size_t(cpuCount * helperThreadRatio.ref()),
                                 size_t(1), maxHelperThreads.ref());

  // Calculate the overall target thread count taking into account the separate
  // parameter for parallel marking threads. Add spare threads to avoid blocking
  // parallel marking when there is other GC work happening.
  size_t targetCount =
      std::max(helperThreadCount.ref(),
               markingThreadCount.ref() + SpareThreadsDuringParallelMarking);

  // Attempt to create extra threads if possible. This is not supported when
  // using an external thread pool.
  AutoLockHelperThreadState lock;
  (void)HelperThreadState().ensureThreadCount(targetCount, lock);

  // Limit all thread counts based on the number of threads available, which may
  // be fewer than requested.
  size_t availableThreadCount = GetHelperThreadCount();
  MOZ_ASSERT(availableThreadCount != 0);
  targetCount = std::min(targetCount, availableThreadCount);
  helperThreadCount = std::min(helperThreadCount.ref(), availableThreadCount);
  markingThreadCount =
      std::min(markingThreadCount.ref(),
               availableThreadCount - SpareThreadsDuringParallelMarking);

  // Update the maximum number of threads that will be used for GC work.
  HelperThreadState().setGCParallelThreadCount(targetCount, lock);
}

size_t GCRuntime::markingWorkerCount() const {
  if (!CanUseExtraThreads() || !parallelMarkingEnabled) {
    return 1;
  }

  if (markingThreadCount) {
    return markingThreadCount;
  }

  // Limit parallel marking to use at most two threads initially.
  return 2;
}

#ifdef DEBUG
void GCRuntime::assertNoMarkingWork() const {
  for (auto& marker : markers) {
    MOZ_ASSERT(marker->isDrained());
  }
  MOZ_ASSERT(!hasDelayedMarking());
}
#endif

static size_t GetGCParallelThreadCount() {
  AutoLockHelperThreadState lock;
  return HelperThreadState().getGCParallelThreadCount(lock);
}

bool GCRuntime::updateMarkersVector() {
  MOZ_ASSERT(helperThreadCount >= 1,
             "There must always be at least one mark task");
  MOZ_ASSERT(CurrentThreadCanAccessRuntime(rt));
  assertNoMarkingWork();

  // Limit worker count to number of GC parallel tasks that can run
  // concurrently, otherwise one thread can deadlock waiting on another.
  size_t targetCount =
      std::min(markingWorkerCount(), GetGCParallelThreadCount());

  if (markers.length() > targetCount) {
    return markers.resize(targetCount);
  }

  while (markers.length() < targetCount) {
    auto marker = MakeUnique<GCMarker>(rt);
    if (!marker) {
      return false;
    }

#ifdef JS_GC_ZEAL
    if (maybeMarkStackLimit) {
      marker->setMaxCapacity(maybeMarkStackLimit);
    }
#endif

    if (!marker->init()) {
      return false;
    }

    if (!markers.emplaceBack(std::move(marker))) {
      return false;
    }
  }

  return true;
}

bool GCRuntime::addBlackRootsTracer(JSTraceDataOp traceOp, void* data) {
  AssertHeapIsIdle();
  return !!blackRootTracers.ref().append(
      Callback<JSTraceDataOp>(traceOp, data));
}

void GCRuntime::removeBlackRootsTracer(JSTraceDataOp traceOp, void* data) {
  // Can be called from finalizers
  for (size_t i = 0; i < blackRootTracers.ref().length(); i++) {
    Callback<JSTraceDataOp>* e = &blackRootTracers.ref()[i];
    if (e->op == traceOp && e->data == data) {
      blackRootTracers.ref().erase(e);
      break;
    }
  }
}

void GCRuntime::setGrayRootsTracer(JSGrayRootsTracer traceOp, void* data) {
  AssertHeapIsIdle();
  grayRootTracer.ref() = {traceOp, data};
}

void GCRuntime::clearBlackAndGrayRootTracers() {
  MOZ_ASSERT(rt->isBeingDestroyed());
  blackRootTracers.ref().clear();
  setGrayRootsTracer(nullptr, nullptr);
}

void GCRuntime::setGCCallback(JSGCCallback callback, void* data) {
  gcCallback.ref() = {callback, data};
}

void GCRuntime::callGCCallback(JSGCStatus status, JS::GCReason reason) const {
  const auto& callback = gcCallback.ref();
  MOZ_ASSERT(callback.op);
  callback.op(rt->mainContextFromOwnThread(), status, reason, callback.data);
}

void GCRuntime::setObjectsTenuredCallback(JSObjectsTenuredCallback callback,
                                          void* data) {
  tenuredCallback.ref() = {callback, data};
}

void GCRuntime::callObjectsTenuredCallback() {
  JS::AutoSuppressGCAnalysis nogc;
  const auto& callback = tenuredCallback.ref();
  if (callback.op) {
    callback.op(rt->mainContextFromOwnThread(), callback.data);
  }
}

bool GCRuntime::addFinalizeCallback(JSFinalizeCallback callback, void* data) {
  return finalizeCallbacks.ref().append(
      Callback<JSFinalizeCallback>(callback, data));
}

template <typename F>
static void EraseCallback(CallbackVector<F>& vector, F callback) {
  for (Callback<F>* p = vector.begin(); p != vector.end(); p++) {
    if (p->op == callback) {
      vector.erase(p);
      return;
    }
  }
}

void GCRuntime::removeFinalizeCallback(JSFinalizeCallback callback) {
  EraseCallback(finalizeCallbacks.ref(), callback);
}

void GCRuntime::callFinalizeCallbacks(JS::GCContext* gcx,
                                      JSFinalizeStatus status) const {
  for (auto& p : finalizeCallbacks.ref()) {
    p.op(gcx, status, p.data);
  }
}

void GCRuntime::setHostCleanupFinalizationRegistryCallback(
    JSHostCleanupFinalizationRegistryCallback callback, void* data) {
  hostCleanupFinalizationRegistryCallback.ref() = {callback, data};
}

void GCRuntime::callHostCleanupFinalizationRegistryCallback(
    JSFunction* doCleanup, GlobalObject* incumbentGlobal) {
  JS::AutoSuppressGCAnalysis nogc;
  const auto& callback = hostCleanupFinalizationRegistryCallback.ref();
  if (callback.op) {
    callback.op(doCleanup, incumbentGlobal, callback.data);
  }
}

bool GCRuntime::addWeakPointerZonesCallback(JSWeakPointerZonesCallback callback,
                                            void* data) {
  return updateWeakPointerZonesCallbacks.ref().append(
      Callback<JSWeakPointerZonesCallback>(callback, data));
}

void GCRuntime::removeWeakPointerZonesCallback(
    JSWeakPointerZonesCallback callback) {
  EraseCallback(updateWeakPointerZonesCallbacks.ref(), callback);
}

void GCRuntime::callWeakPointerZonesCallbacks(JSTracer* trc) const {
  for (auto const& p : updateWeakPointerZonesCallbacks.ref()) {
    p.op(trc, p.data);
  }
}

bool GCRuntime::addWeakPointerCompartmentCallback(
    JSWeakPointerCompartmentCallback callback, void* data) {
  return updateWeakPointerCompartmentCallbacks.ref().append(
      Callback<JSWeakPointerCompartmentCallback>(callback, data));
}

void GCRuntime::removeWeakPointerCompartmentCallback(
    JSWeakPointerCompartmentCallback callback) {
  EraseCallback(updateWeakPointerCompartmentCallbacks.ref(), callback);
}

void GCRuntime::callWeakPointerCompartmentCallbacks(
    JSTracer* trc, JS::Compartment* comp) const {
  for (auto const& p : updateWeakPointerCompartmentCallbacks.ref()) {
    p.op(trc, comp, p.data);
  }
}

JS::GCSliceCallback GCRuntime::setSliceCallback(JS::GCSliceCallback callback) {
  return stats().setSliceCallback(callback);
}

JS::GCNurseryCollectionCallback GCRuntime::setNurseryCollectionCallback(
    JS::GCNurseryCollectionCallback callback) {
  return stats().setNurseryCollectionCallback(callback);
}

JS::DoCycleCollectionCallback GCRuntime::setDoCycleCollectionCallback(
    JS::DoCycleCollectionCallback callback) {
  const auto prior = gcDoCycleCollectionCallback.ref();
  gcDoCycleCollectionCallback.ref() = {callback, nullptr};
  return prior.op;
}

void GCRuntime::callDoCycleCollectionCallback(JSContext* cx) {
  const auto& callback = gcDoCycleCollectionCallback.ref();
  if (callback.op) {
    callback.op(cx);
  }
}

bool GCRuntime::addRoot(Value* vp, const char* name) {
  /*
   * Sometimes Firefox will hold weak references to objects and then convert
   * them to strong references by calling AddRoot (e.g., via PreserveWrapper,
   * or ModifyBusyCount in workers). We need a read barrier to cover these
   * cases.
   */
  MOZ_ASSERT(vp);
  Value value = *vp;
  if (value.isGCThing()) {
    ValuePreWriteBarrier(value);
  }

  return rootsHash.ref().put(vp, name);
}

void GCRuntime::removeRoot(Value* vp) {
  rootsHash.ref().remove(vp);
  notifyRootsRemoved();
}

/* Compacting GC */

bool js::gc::IsCurrentlyAnimating(const TimeStamp& lastAnimationTime,
                                  const TimeStamp& currentTime) {
  // Assume that we're currently animating if js::NotifyAnimationActivity has
  // been called in the last second.
  static const auto oneSecond = TimeDuration::FromSeconds(1);
  return !lastAnimationTime.IsNull() &&
         currentTime < (lastAnimationTime + oneSecond);
}

static bool DiscardedCodeRecently(Zone* zone, const TimeStamp& currentTime) {
  static const auto thirtySeconds = TimeDuration::FromSeconds(30);
  return !zone->lastDiscardedCodeTime().IsNull() &&
         currentTime < (zone->lastDiscardedCodeTime() + thirtySeconds);
}

bool GCRuntime::shouldCompact() {
  // Compact on shrinking GC if enabled.  Skip compacting in incremental GCs
  // if we are currently animating, unless the user is inactive or we're
  // responding to memory pressure.

  if (!isShrinkingGC() || !isCompactingGCEnabled()) {
    return false;
  }

  if (initialReason == JS::GCReason::USER_INACTIVE ||
      initialReason == JS::GCReason::MEM_PRESSURE) {
    return true;
  }

  return !isIncremental ||
         !IsCurrentlyAnimating(rt->lastAnimationTime, TimeStamp::Now());
}

bool GCRuntime::isCompactingGCEnabled() const {
  return compactingEnabled &&
         rt->mainContextFromOwnThread()->compactingDisabledCount == 0;
}

JS_PUBLIC_API void JS::SetCreateGCSliceBudgetCallback(
    JSContext* cx, JS::CreateSliceBudgetCallback cb) {
  cx->runtime()->gc.createBudgetCallback = cb;
}

void TimeBudget::setDeadlineFromNow() { deadline = TimeStamp::Now() + budget; }

SliceBudget::SliceBudget(TimeBudget time, InterruptRequestFlag* interrupt)
    : budget(TimeBudget(time)),
      interruptRequested(interrupt),
      counter(StepsPerExpensiveCheck) {
  budget.as<TimeBudget>().setDeadlineFromNow();
}

SliceBudget::SliceBudget(WorkBudget work)
    : budget(work), interruptRequested(nullptr), counter(work.budget) {}

int SliceBudget::describe(char* buffer, size_t maxlen) const {
  if (isUnlimited()) {
    return snprintf(buffer, maxlen, "unlimited");
  } else if (isWorkBudget()) {
    return snprintf(buffer, maxlen, "work(%" PRId64 ")", workBudget());
  } else {
    const char* interruptStr = "";
    if (interruptRequested) {
      interruptStr = interrupted ? "INTERRUPTED " : "interruptible ";
    }
    const char* extra = "";
    if (idle) {
      extra = extended ? " (started idle but extended)" : " (idle)";
    }
    return snprintf(buffer, maxlen, "%s%" PRId64 "ms%s", interruptStr,
                    timeBudget(), extra);
  }
}

bool SliceBudget::checkOverBudget() {
  MOZ_ASSERT(counter <= 0);
  MOZ_ASSERT(!isUnlimited());

  if (isWorkBudget()) {
    return true;
  }

  if (interruptRequested && *interruptRequested) {
    *interruptRequested = false;
    interrupted = true;
  }

  if (interrupted) {
    return true;
  }

  if (TimeStamp::Now() >= budget.as<TimeBudget>().deadline) {
    return true;
  }

  counter = StepsPerExpensiveCheck;
  return false;
}

void GCRuntime::requestMajorGC(JS::GCReason reason) {
  MOZ_ASSERT_IF(reason != JS::GCReason::BG_TASK_FINISHED,
                !CurrentThreadIsPerformingGC());

  if (majorGCRequested()) {
    return;
  }

  majorGCTriggerReason = reason;
  rt->mainContextFromAnyThread()->requestInterrupt(InterruptReason::GC);
}

void Nursery::requestMinorGC(JS::GCReason reason) const {
  MOZ_ASSERT(CurrentThreadCanAccessRuntime(runtime()));

  if (minorGCRequested()) {
    return;
  }

  minorGCTriggerReason_ = reason;
  runtime()->mainContextFromOwnThread()->requestInterrupt(InterruptReason::GC);
}

bool GCRuntime::triggerGC(JS::GCReason reason) {
  /*
   * Don't trigger GCs if this is being called off the main thread from
   * onTooMuchMalloc().
   */
  if (!CurrentThreadCanAccessRuntime(rt)) {
    return false;
  }

  /* GC is already running. */
  if (JS::RuntimeHeapIsCollecting()) {
    return false;
  }

  JS::PrepareForFullGC(rt->mainContextFromOwnThread());
  requestMajorGC(reason);
  return true;
}

void GCRuntime::maybeTriggerGCAfterAlloc(Zone* zone) {
  MOZ_ASSERT(CurrentThreadCanAccessRuntime(rt));
  MOZ_ASSERT(!JS::RuntimeHeapIsCollecting());

  TriggerResult trigger =
      checkHeapThreshold(zone, zone->gcHeapSize, zone->gcHeapThreshold);

  if (trigger.shouldTrigger) {
    // Start or continue an in progress incremental GC. We do this to try to
    // avoid performing non-incremental GCs on zones which allocate a lot of
    // data, even when incremental slices can't be triggered via scheduling in
    // the event loop.
    triggerZoneGC(zone, JS::GCReason::ALLOC_TRIGGER, trigger.usedBytes,
                  trigger.thresholdBytes);
  }
}

void js::gc::MaybeMallocTriggerZoneGC(JSRuntime* rt, ZoneAllocator* zoneAlloc,
                                      const HeapSize& heap,
                                      const HeapThreshold& threshold,
                                      JS::GCReason reason) {
  rt->gc.maybeTriggerGCAfterMalloc(Zone::from(zoneAlloc), heap, threshold,
                                   reason);
}

void GCRuntime::maybeTriggerGCAfterMalloc(Zone* zone) {
  if (maybeTriggerGCAfterMalloc(zone, zone->mallocHeapSize,
                                zone->mallocHeapThreshold,
                                JS::GCReason::TOO_MUCH_MALLOC)) {
    return;
  }

  maybeTriggerGCAfterMalloc(zone, zone->jitHeapSize, zone->jitHeapThreshold,
                            JS::GCReason::TOO_MUCH_JIT_CODE);
}

bool GCRuntime::maybeTriggerGCAfterMalloc(Zone* zone, const HeapSize& heap,
                                          const HeapThreshold& threshold,
                                          JS::GCReason reason) {
  // Ignore malloc during sweeping, for example when we resize hash tables.
  if (heapState() != JS::HeapState::Idle) {
    return false;
  }

  MOZ_ASSERT(CurrentThreadCanAccessRuntime(rt));

  TriggerResult trigger = checkHeapThreshold(zone, heap, threshold);
  if (!trigger.shouldTrigger) {
    return false;
  }

  // Trigger a zone GC. budgetIncrementalGC() will work out whether to do an
  // incremental or non-incremental collection.
  triggerZoneGC(zone, reason, trigger.usedBytes, trigger.thresholdBytes);
  return true;
}

TriggerResult GCRuntime::checkHeapThreshold(
    Zone* zone, const HeapSize& heapSize, const HeapThreshold& heapThreshold) {
  MOZ_ASSERT_IF(heapThreshold.hasSliceThreshold(), zone->wasGCStarted());

  size_t usedBytes = heapSize.bytes();
  size_t thresholdBytes = heapThreshold.hasSliceThreshold()
                              ? heapThreshold.sliceBytes()
                              : heapThreshold.startBytes();

  // The incremental limit will be checked if we trigger a GC slice.
  MOZ_ASSERT(thresholdBytes <= heapThreshold.incrementalLimitBytes());

  return TriggerResult{usedBytes >= thresholdBytes, usedBytes, thresholdBytes};
}

bool GCRuntime::triggerZoneGC(Zone* zone, JS::GCReason reason, size_t used,
                              size_t threshold) {
  MOZ_ASSERT(CurrentThreadCanAccessRuntime(rt));

  /* GC is already running. */
  if (JS::RuntimeHeapIsBusy()) {
    return false;
  }

#ifdef JS_GC_ZEAL
  if (hasZealMode(ZealMode::Alloc)) {
    MOZ_RELEASE_ASSERT(triggerGC(reason));
    return true;
  }
#endif

  if (zone->isAtomsZone()) {
    stats().recordTrigger(used, threshold);
    MOZ_RELEASE_ASSERT(triggerGC(reason));
    return true;
  }

  stats().recordTrigger(used, threshold);
  zone->scheduleGC();
  requestMajorGC(reason);
  return true;
}

void GCRuntime::maybeGC() {
  MOZ_ASSERT(CurrentThreadCanAccessRuntime(rt));

#ifdef JS_GC_ZEAL
  if (hasZealMode(ZealMode::Alloc) || hasZealMode(ZealMode::RootsChange)) {
    JS::PrepareForFullGC(rt->mainContextFromOwnThread());
    gc(JS::GCOptions::Normal, JS::GCReason::DEBUG_GC);
    return;
  }
#endif

  (void)gcIfRequestedImpl(/* eagerOk = */ true);
}

JS::GCReason GCRuntime::wantMajorGC(bool eagerOk) {
  MOZ_ASSERT(CurrentThreadCanAccessRuntime(rt));

  if (majorGCRequested()) {
    return majorGCTriggerReason;
  }

  if (isIncrementalGCInProgress() || !eagerOk) {
    return JS::GCReason::NO_REASON;
  }

  JS::GCReason reason = JS::GCReason::NO_REASON;
  for (ZonesIter zone(this, WithAtoms); !zone.done(); zone.next()) {
    if (checkEagerAllocTrigger(zone->gcHeapSize, zone->gcHeapThreshold) ||
        checkEagerAllocTrigger(zone->mallocHeapSize,
                               zone->mallocHeapThreshold)) {
      zone->scheduleGC();
      reason = JS::GCReason::EAGER_ALLOC_TRIGGER;
    }
  }

  return reason;
}

bool GCRuntime::checkEagerAllocTrigger(const HeapSize& size,
                                       const HeapThreshold& threshold) {
  double thresholdBytes =
      threshold.eagerAllocTrigger(schedulingState.inHighFrequencyGCMode());
  double usedBytes = size.bytes();
  if (usedBytes <= 1024 * 1024 || usedBytes < thresholdBytes) {
    return false;
  }

  stats().recordTrigger(usedBytes, thresholdBytes);
  return true;
}

bool GCRuntime::shouldDecommit() const {
  // If we're doing a shrinking GC we always decommit to release as much memory
  // as possible.
  if (cleanUpEverything) {
    return true;
  }

  // If we are allocating heavily enough to trigger "high frequency" GC then
  // skip decommit so that we do not compete with the mutator.
  return !schedulingState.inHighFrequencyGCMode();
}

void GCRuntime::startDecommit() {
  gcstats::AutoPhase ap(stats(), gcstats::PhaseKind::DECOMMIT);

#ifdef DEBUG
  MOZ_ASSERT(CurrentThreadCanAccessRuntime(rt));
  MOZ_ASSERT(decommitTask.isIdle());

  {
    AutoLockGC lock(this);
    MOZ_ASSERT(fullChunks(lock).verify());
    MOZ_ASSERT(availableChunks(lock).verify());
    MOZ_ASSERT(emptyChunks(lock).verify());

    // Verify that all entries in the empty chunks pool are unused.
    for (ChunkPool::Iter chunk(emptyChunks(lock)); !chunk.done();
         chunk.next()) {
      MOZ_ASSERT(chunk->unused());
    }
  }
#endif

  if (!shouldDecommit()) {
    return;
  }

  {
    AutoLockGC lock(this);
    if (availableChunks(lock).empty() && !tooManyEmptyChunks(lock) &&
        emptyChunks(lock).empty()) {
      return;  // Nothing to do.
    }
  }

#ifdef DEBUG
  {
    AutoLockHelperThreadState lock;
    MOZ_ASSERT(!requestSliceAfterBackgroundTask);
  }
#endif

  if (useBackgroundThreads) {
    decommitTask.start();
    return;
  }

  decommitTask.runFromMainThread();
}

BackgroundDecommitTask::BackgroundDecommitTask(GCRuntime* gc)
    : GCParallelTask(gc, gcstats::PhaseKind::DECOMMIT) {}

void js::gc::BackgroundDecommitTask::run(AutoLockHelperThreadState& lock) {
  {
    AutoUnlockHelperThreadState unlock(lock);

    ChunkPool emptyChunksToFree;
    {
      AutoLockGC gcLock(gc);
      emptyChunksToFree = gc->expireEmptyChunkPool(gcLock);
    }

    FreeChunkPool(emptyChunksToFree);

    {
      AutoLockGC gcLock(gc);

      // To help minimize the total number of chunks needed over time, sort the
      // available chunks list so that we allocate into more-used chunks first.
      gc->availableChunks(gcLock).sort();

      if (DecommitEnabled()) {
        gc->decommitEmptyChunks(cancel_, gcLock);
        gc->decommitFreeArenas(cancel_, gcLock);
      }
    }
  }

  gc->maybeRequestGCAfterBackgroundTask(lock);
}

static inline bool CanDecommitWholeChunk(TenuredChunk* chunk) {
  return chunk->unused() && chunk->info.numArenasFreeCommitted != 0;
}

// Called from a background thread to decommit free arenas. Releases the GC
// lock.
void GCRuntime::decommitEmptyChunks(const bool& cancel, AutoLockGC& lock) {
  Vector<TenuredChunk*, 0, SystemAllocPolicy> chunksToDecommit;
  for (ChunkPool::Iter chunk(emptyChunks(lock)); !chunk.done(); chunk.next()) {
    if (CanDecommitWholeChunk(chunk) && !chunksToDecommit.append(chunk)) {
      onOutOfMallocMemory(lock);
      return;
    }
  }

  for (TenuredChunk* chunk : chunksToDecommit) {
    if (cancel) {
      break;
    }

    // Check whether something used the chunk while lock was released.
    if (!CanDecommitWholeChunk(chunk)) {
      continue;
    }

    // Temporarily remove the chunk while decommitting its memory so that the
    // mutator doesn't start allocating from it when we drop the lock.
    emptyChunks(lock).remove(chunk);

    {
      AutoUnlockGC unlock(lock);
      chunk->decommitAllArenas();
      MOZ_ASSERT(chunk->info.numArenasFreeCommitted == 0);
    }

    emptyChunks(lock).push(chunk);
  }
}

// Called from a background thread to decommit free arenas. Releases the GC
// lock.
void GCRuntime::decommitFreeArenas(const bool& cancel, AutoLockGC& lock) {
  MOZ_ASSERT(DecommitEnabled());

  // Since we release the GC lock while doing the decommit syscall below,
  // it is dangerous to iterate the available list directly, as the active
  // thread could modify it concurrently. Instead, we build and pass an
  // explicit Vector containing the Chunks we want to visit.
  Vector<TenuredChunk*, 0, SystemAllocPolicy> chunksToDecommit;
  for (ChunkPool::Iter chunk(availableChunks(lock)); !chunk.done();
       chunk.next()) {
    if (chunk->info.numArenasFreeCommitted != 0 &&
        !chunksToDecommit.append(chunk)) {
      onOutOfMallocMemory(lock);
      return;
    }
  }

  for (TenuredChunk* chunk : chunksToDecommit) {
    chunk->decommitFreeArenas(this, cancel, lock);
  }
}

// Do all possible decommit immediately from the current thread without
// releasing the GC lock or allocating any memory.
void GCRuntime::decommitFreeArenasWithoutUnlocking(const AutoLockGC& lock) {
  MOZ_ASSERT(DecommitEnabled());
  for (ChunkPool::Iter chunk(availableChunks(lock)); !chunk.done();
       chunk.next()) {
    chunk->decommitFreeArenasWithoutUnlocking(lock);
  }
  MOZ_ASSERT(availableChunks(lock).verify());
}

void GCRuntime::maybeRequestGCAfterBackgroundTask(
    const AutoLockHelperThreadState& lock) {
  if (requestSliceAfterBackgroundTask) {
    // Trigger a slice so the main thread can continue the collection
    // immediately.
    requestSliceAfterBackgroundTask = false;
    requestMajorGC(JS::GCReason::BG_TASK_FINISHED);
  }
}

void GCRuntime::cancelRequestedGCAfterBackgroundTask() {
  MOZ_ASSERT(CurrentThreadCanAccessRuntime(rt));

#ifdef DEBUG
  {
    AutoLockHelperThreadState lock;
    MOZ_ASSERT(!requestSliceAfterBackgroundTask);
  }
#endif

  majorGCTriggerReason.compareExchange(JS::GCReason::BG_TASK_FINISHED,
                                       JS::GCReason::NO_REASON);
}

bool GCRuntime::isWaitingOnBackgroundTask() const {
  AutoLockHelperThreadState lock;
  return requestSliceAfterBackgroundTask;
}

void GCRuntime::queueUnusedLifoBlocksForFree(LifoAlloc* lifo) {
  MOZ_ASSERT(JS::RuntimeHeapIsBusy());
  AutoLockHelperThreadState lock;
  lifoBlocksToFree.ref().transferUnusedFrom(lifo);
}

void GCRuntime::queueAllLifoBlocksForFreeAfterMinorGC(LifoAlloc* lifo) {
  lifoBlocksToFreeAfterMinorGC.ref().transferFrom(lifo);
}

void GCRuntime::queueBuffersForFreeAfterMinorGC(Nursery::BufferSet& buffers) {
  AutoLockHelperThreadState lock;

  if (!buffersToFreeAfterMinorGC.ref().empty()) {
    // In the rare case that this hasn't processed the buffers from a previous
    // minor GC we have to wait here.
    MOZ_ASSERT(!freeTask.isIdle(lock));
    freeTask.joinWithLockHeld(lock);
  }

  MOZ_ASSERT(buffersToFreeAfterMinorGC.ref().empty());
  std::swap(buffersToFreeAfterMinorGC.ref(), buffers);
}

void Realm::destroy(JS::GCContext* gcx) {
  JSRuntime* rt = gcx->runtime();
  if (auto callback = rt->destroyRealmCallback) {
    callback(gcx, this);
  }
  if (principals()) {
    JS_DropPrincipals(rt->mainContextFromOwnThread(), principals());
  }
  // Bug 1560019: Malloc memory associated with a zone but not with a specific
  // GC thing is not currently tracked.
  gcx->deleteUntracked(this);
}

void Compartment::destroy(JS::GCContext* gcx) {
  JSRuntime* rt = gcx->runtime();
  if (auto callback = rt->destroyCompartmentCallback) {
    callback(gcx, this);
  }
  // Bug 1560019: Malloc memory associated with a zone but not with a specific
  // GC thing is not currently tracked.
  gcx->deleteUntracked(this);
  rt->gc.stats().sweptCompartment();
}

void Zone::destroy(JS::GCContext* gcx) {
  MOZ_ASSERT(compartments().empty());
  JSRuntime* rt = gcx->runtime();
  if (auto callback = rt->destroyZoneCallback) {
    callback(gcx, this);
  }
  // Bug 1560019: Malloc memory associated with a zone but not with a specific
  // GC thing is not currently tracked.
  gcx->deleteUntracked(this);
  gcx->runtime()->gc.stats().sweptZone();
}

/*
 * It's simpler if we preserve the invariant that every zone (except atoms
 * zones) has at least one compartment, and every compartment has at least one
 * realm. If we know we're deleting the entire zone, then sweepCompartments is
 * allowed to delete all compartments. In this case, |keepAtleastOne| is false.
 * If any cells remain alive in the zone, set |keepAtleastOne| true to prohibit
 * sweepCompartments from deleting every compartment. Instead, it preserves an
 * arbitrary compartment in the zone.
 */
void Zone::sweepCompartments(JS::GCContext* gcx, bool keepAtleastOne,
                             bool destroyingRuntime) {
  MOZ_ASSERT_IF(!isAtomsZone(), !compartments().empty());
  MOZ_ASSERT_IF(destroyingRuntime, !keepAtleastOne);

  Compartment** read = compartments().begin();
  Compartment** end = compartments().end();
  Compartment** write = read;
  while (read < end) {
    Compartment* comp = *read++;

    /*
     * Don't delete the last compartment and realm if keepAtleastOne is
     * still true, meaning all the other compartments were deleted.
     */
    bool keepAtleastOneRealm = read == end && keepAtleastOne;
    comp->sweepRealms(gcx, keepAtleastOneRealm, destroyingRuntime);

    if (!comp->realms().empty()) {
      *write++ = comp;
      keepAtleastOne = false;
    } else {
      comp->destroy(gcx);
    }
  }
  compartments().shrinkTo(write - compartments().begin());
  MOZ_ASSERT_IF(keepAtleastOne, !compartments().empty());
  MOZ_ASSERT_IF(destroyingRuntime, compartments().empty());
}

void Compartment::sweepRealms(JS::GCContext* gcx, bool keepAtleastOne,
                              bool destroyingRuntime) {
  MOZ_ASSERT(!realms().empty());
  MOZ_ASSERT_IF(destroyingRuntime, !keepAtleastOne);

  Realm** read = realms().begin();
  Realm** end = realms().end();
  Realm** write = read;
  while (read < end) {
    Realm* realm = *read++;

    /*
     * Don't delete the last realm if keepAtleastOne is still true, meaning
     * all the other realms were deleted.
     */
    bool dontDelete = read == end && keepAtleastOne;
    if ((realm->marked() || dontDelete) && !destroyingRuntime) {
      *write++ = realm;
      keepAtleastOne = false;
    } else {
      realm->destroy(gcx);
    }
  }
  realms().shrinkTo(write - realms().begin());
  MOZ_ASSERT_IF(keepAtleastOne, !realms().empty());
  MOZ_ASSERT_IF(destroyingRuntime, realms().empty());
}

void GCRuntime::sweepZones(JS::GCContext* gcx, bool destroyingRuntime) {
  MOZ_ASSERT_IF(destroyingRuntime, numActiveZoneIters == 0);

  if (numActiveZoneIters) {
    return;
  }

  assertBackgroundSweepingFinished();

  // Sweep zones following the atoms zone.
  MOZ_ASSERT(zones()[0]->isAtomsZone());
  Zone** read = zones().begin() + 1;
  Zone** end = zones().end();
  Zone** write = read;

  while (read < end) {
    Zone* zone = *read++;

    if (zone->wasGCStarted()) {
      MOZ_ASSERT(!zone->isQueuedForBackgroundSweep());
      AutoSetThreadIsSweeping threadIsSweeping(zone);
      const bool zoneIsDead =
          zone->arenas.arenaListsAreEmpty() && !zone->hasMarkedRealms();
      MOZ_ASSERT_IF(destroyingRuntime, zoneIsDead);
      if (zoneIsDead) {
        zone->arenas.checkEmptyFreeLists();
        zone->sweepCompartments(gcx, false, destroyingRuntime);
        MOZ_ASSERT(zone->compartments().empty());
        zone->destroy(gcx);
        continue;
      }
      zone->sweepCompartments(gcx, true, destroyingRuntime);
    }
    *write++ = zone;
  }
  zones().shrinkTo(write - zones().begin());
}

void ArenaLists::checkEmptyArenaList(AllocKind kind) {
  MOZ_ASSERT(arenaList(kind).isEmpty());
}

void GCRuntime::purgeRuntimeForMinorGC() {
  // If external strings become nursery allocable, remember to call
  // zone->externalStringCache().purge() (and delete this assert.)
  MOZ_ASSERT(!IsNurseryAllocable(AllocKind::EXTERNAL_STRING));

  for (ZonesIter zone(this, SkipAtoms); !zone.done(); zone.next()) {
    zone->functionToStringCache().purge();
  }
}

void GCRuntime::purgeRuntime() {
  gcstats::AutoPhase ap(stats(), gcstats::PhaseKind::PURGE);

  for (GCRealmsIter realm(rt); !realm.done(); realm.next()) {
    realm->purge();
  }

  for (GCZonesIter zone(this); !zone.done(); zone.next()) {
    zone->purgeAtomCache();
    zone->externalStringCache().purge();
    zone->functionToStringCache().purge();
    zone->boundPrefixCache().clearAndCompact();
    zone->shapeZone().purgeShapeCaches(rt->gcContext());
  }

  JSContext* cx = rt->mainContextFromOwnThread();
  queueUnusedLifoBlocksForFree(&cx->tempLifoAlloc());
  cx->interpreterStack().purge(rt);
  cx->frontendCollectionPool().purge();

  rt->caches().purge();

  if (rt->isMainRuntime()) {
    SharedImmutableStringsCache::getSingleton().purge();
  }

  MOZ_ASSERT(marker().unmarkGrayStack.empty());
  marker().unmarkGrayStack.clearAndFree();

  // If we're the main runtime, tell helper threads to free their unused
  // memory when they are next idle.
  if (!rt->parentRuntime) {
    HelperThreadState().triggerFreeUnusedMemory();
  }
}

bool GCRuntime::shouldPreserveJITCode(Realm* realm,
                                      const TimeStamp& currentTime,
                                      JS::GCReason reason,
                                      bool canAllocateMoreCode,
                                      bool isActiveCompartment) {
  if (cleanUpEverything) {
    return false;
  }
  if (!canAllocateMoreCode) {
    return false;
  }

  if (isActiveCompartment) {
    return true;
  }
  if (alwaysPreserveCode) {
    return true;
  }
  if (realm->preserveJitCode()) {
    return true;
  }
  if (IsCurrentlyAnimating(realm->lastAnimationTime, currentTime) &&
      DiscardedCodeRecently(realm->zone(), currentTime)) {
    return true;
  }
  if (reason == JS::GCReason::DEBUG_GC) {
    return true;
  }

  return false;
}

#ifdef DEBUG
class CompartmentCheckTracer final : public JS::CallbackTracer {
  void onChild(JS::GCCellPtr thing, const char* name) override;
  bool edgeIsInCrossCompartmentMap(JS::GCCellPtr dst);

 public:
  explicit CompartmentCheckTracer(JSRuntime* rt)
      : JS::CallbackTracer(rt, JS::TracerKind::CompartmentCheck,
                           JS::WeakEdgeTraceAction::Skip),
        src(nullptr),
        zone(nullptr),
        compartment(nullptr) {}

  Cell* src;
  JS::TraceKind srcKind;
  Zone* zone;
  Compartment* compartment;
};

static bool InCrossCompartmentMap(JSRuntime* rt, JSObject* src,
                                  JS::GCCellPtr dst) {
  // Cross compartment edges are either in the cross compartment map or in a
  // debugger weakmap.

  Compartment* srccomp = src->compartment();

  if (dst.is<JSObject>()) {
    if (ObjectWrapperMap::Ptr p = srccomp->lookupWrapper(&dst.as<JSObject>())) {
      if (*p->value().unsafeGet() == src) {
        return true;
      }
    }
  }

  if (DebugAPI::edgeIsInDebuggerWeakmap(rt, src, dst)) {
    return true;
  }

  return false;
}

void CompartmentCheckTracer::onChild(JS::GCCellPtr thing, const char* name) {
  Compartment* comp =
      MapGCThingTyped(thing, [](auto t) { return t->maybeCompartment(); });
  if (comp && compartment) {
    MOZ_ASSERT(comp == compartment || edgeIsInCrossCompartmentMap(thing));
  } else {
    TenuredCell* tenured = &thing.asCell()->asTenured();
    Zone* thingZone = tenured->zoneFromAnyThread();
    MOZ_ASSERT(thingZone == zone || thingZone->isAtomsZone());
  }
}

bool CompartmentCheckTracer::edgeIsInCrossCompartmentMap(JS::GCCellPtr dst) {
  return srcKind == JS::TraceKind::Object &&
         InCrossCompartmentMap(runtime(), static_cast<JSObject*>(src), dst);
}

static bool IsPartiallyInitializedObject(Cell* cell) {
  if (!cell->is<JSObject>()) {
    return false;
  }

  JSObject* obj = cell->as<JSObject>();
  if (!obj->is<NativeObject>()) {
    return false;
  }

  NativeObject* nobj = &obj->as<NativeObject>();

  // Check for failed allocation of dynamic slots in
  // NativeObject::allocateInitialSlots.
  size_t nDynamicSlots = NativeObject::calculateDynamicSlots(
      nobj->numFixedSlots(), nobj->slotSpan(), nobj->getClass());
  return nDynamicSlots != 0 && !nobj->hasDynamicSlots();
}

void GCRuntime::checkForCompartmentMismatches() {
  JSContext* cx = rt->mainContextFromOwnThread();
  if (cx->disableStrictProxyCheckingCount) {
    return;
  }

  CompartmentCheckTracer trc(rt);
  AutoAssertEmptyNursery empty(cx);
  for (ZonesIter zone(this, SkipAtoms); !zone.done(); zone.next()) {
    trc.zone = zone;
    for (auto thingKind : AllAllocKinds()) {
      for (auto i = zone->cellIterUnsafe<TenuredCell>(thingKind, empty);
           !i.done(); i.next()) {
        // We may encounter partially initialized objects. These are unreachable
        // and it's safe to ignore them.
        if (IsPartiallyInitializedObject(i.getCell())) {
          continue;
        }

        trc.src = i.getCell();
        trc.srcKind = MapAllocToTraceKind(thingKind);
        trc.compartment = MapGCThingTyped(
            trc.src, trc.srcKind, [](auto t) { return t->maybeCompartment(); });
        JS::TraceChildren(&trc, JS::GCCellPtr(trc.src, trc.srcKind));
      }
    }
  }
}
#endif

static bool ShouldCleanUpEverything(JS::GCOptions options) {
  // During shutdown, we must clean everything up, for the sake of leak
  // detection. When a runtime has no contexts, or we're doing a GC before a
  // shutdown CC, those are strong indications that we're shutting down.
  return options == JS::GCOptions::Shutdown || options == JS::GCOptions::Shrink;
}

static bool ShouldUseBackgroundThreads(bool isIncremental,
                                       JS::GCReason reason) {
  bool shouldUse = isIncremental && CanUseExtraThreads();
  MOZ_ASSERT_IF(reason == JS::GCReason::DESTROY_RUNTIME, !shouldUse);
  return shouldUse;
}

void GCRuntime::startCollection(JS::GCReason reason) {
  checkGCStateNotInUse();
  MOZ_ASSERT_IF(
      isShuttingDown(),
      isShutdownGC() ||
          reason == JS::GCReason::XPCONNECT_SHUTDOWN /* Bug 1650075 */);

  initialReason = reason;
  cleanUpEverything = ShouldCleanUpEverything(gcOptions());
  isCompacting = shouldCompact();
  rootsRemoved = false;
  sweepGroupIndex = 0;
  lastGCStartTime_ = TimeStamp::Now();

#ifdef DEBUG
  if (isShutdownGC()) {
    hadShutdownGC = true;
  }

  for (ZonesIter zone(this, WithAtoms); !zone.done(); zone.next()) {
    zone->gcSweepGroupIndex = 0;
  }
#endif
}

static void RelazifyFunctions(Zone* zone, AllocKind kind) {
  MOZ_ASSERT(kind == AllocKind::FUNCTION ||
             kind == AllocKind::FUNCTION_EXTENDED);

  JSRuntime* rt = zone->runtimeFromMainThread();
  AutoAssertEmptyNursery empty(rt->mainContextFromOwnThread());

  for (auto i = zone->cellIterUnsafe<JSObject>(kind, empty); !i.done();
       i.next()) {
    JSFunction* fun = &i->as<JSFunction>();
    // When iterating over the GC-heap, we may encounter function objects that
    // are incomplete (missing a BaseScript when we expect one). We must check
    // for this case before we can call JSFunction::hasBytecode().
    if (fun->isIncomplete()) {
      continue;
    }
    if (fun->hasBytecode()) {
      fun->maybeRelazify(rt);
    }
  }
}

static bool ShouldCollectZone(Zone* zone, JS::GCReason reason) {
  // If we are repeating a GC because we noticed dead compartments haven't
  // been collected, then only collect zones containing those compartments.
  if (reason == JS::GCReason::COMPARTMENT_REVIVED) {
    for (CompartmentsInZoneIter comp(zone); !comp.done(); comp.next()) {
      if (comp->gcState.scheduledForDestruction) {
        return true;
      }
    }

    return false;
  }

  // Otherwise we only collect scheduled zones.
  return zone->isGCScheduled();
}

bool GCRuntime::prepareZonesForCollection(JS::GCReason reason,
                                          bool* isFullOut) {
#ifdef DEBUG
  /* Assert that zone state is as we expect */
  for (ZonesIter zone(this, WithAtoms); !zone.done(); zone.next()) {
    MOZ_ASSERT(!zone->isCollecting());
    MOZ_ASSERT_IF(!zone->isAtomsZone(), !zone->compartments().empty());
    for (auto i : AllAllocKinds()) {
      MOZ_ASSERT(zone->arenas.collectingArenaList(i).isEmpty());
    }
  }
#endif

  *isFullOut = true;
  bool any = false;

  for (ZonesIter zone(this, WithAtoms); !zone.done(); zone.next()) {
    /* Set up which zones will be collected. */
    bool shouldCollect = ShouldCollectZone(zone, reason);
    if (shouldCollect) {
      any = true;
      zone->changeGCState(Zone::NoGC, Zone::Prepare);
    } else {
      *isFullOut = false;
    }

    zone->setWasCollected(shouldCollect);
  }

  /* Check that at least one zone is scheduled for collection. */
  return any;
}

void GCRuntime::discardJITCodeForGC() {
  size_t nurserySiteResetCount = 0;
  size_t pretenuredSiteResetCount = 0;

  js::CancelOffThreadIonCompile(rt, JS::Zone::Prepare);
  for (GCZonesIter zone(this); !zone.done(); zone.next()) {
    gcstats::AutoPhase ap(stats(), gcstats::PhaseKind::MARK_DISCARD_CODE);

    // We may need to reset allocation sites and discard JIT code to recover if
    // we find object lifetimes have changed.
    PretenuringZone& pz = zone->pretenuring;
    bool resetNurserySites = pz.shouldResetNurseryAllocSites();
    bool resetPretenuredSites = pz.shouldResetPretenuredAllocSites();

    if (!zone->isPreservingCode()) {
      Zone::DiscardOptions options;
      options.discardBaselineCode = true;
      options.discardJitScripts = true;
      options.resetNurseryAllocSites = resetNurserySites;
      options.resetPretenuredAllocSites = resetPretenuredSites;
      zone->discardJitCode(rt->gcContext(), options);
    } else if (resetNurserySites || resetPretenuredSites) {
      zone->resetAllocSitesAndInvalidate(resetNurserySites,
                                         resetPretenuredSites);
    }

    if (resetNurserySites) {
      nurserySiteResetCount++;
    }
    if (resetPretenuredSites) {
      pretenuredSiteResetCount++;
    }
  }

  if (nursery().reportPretenuring()) {
    if (nurserySiteResetCount) {
      fprintf(
          stderr,
          "GC reset nursery alloc sites and invalidated code in %zu zones\n",
          nurserySiteResetCount);
    }
    if (pretenuredSiteResetCount) {
      fprintf(
          stderr,
          "GC reset pretenured alloc sites and invalidated code in %zu zones\n",
          pretenuredSiteResetCount);
    }
  }
}

void GCRuntime::relazifyFunctionsForShrinkingGC() {
  gcstats::AutoPhase ap(stats(), gcstats::PhaseKind::RELAZIFY_FUNCTIONS);
  for (GCZonesIter zone(this); !zone.done(); zone.next()) {
    RelazifyFunctions(zone, AllocKind::FUNCTION);
    RelazifyFunctions(zone, AllocKind::FUNCTION_EXTENDED);
  }
}

void GCRuntime::purgePropMapTablesForShrinkingGC() {
  gcstats::AutoPhase ap(stats(), gcstats::PhaseKind::PURGE_PROP_MAP_TABLES);
  for (GCZonesIter zone(this); !zone.done(); zone.next()) {
    if (!canRelocateZone(zone) || zone->keepPropMapTables()) {
      continue;
    }

    // Note: CompactPropMaps never have a table.
    for (auto map = zone->cellIterUnsafe<NormalPropMap>(); !map.done();
         map.next()) {
      if (map->asLinked()->hasTable()) {
        map->asLinked()->purgeTable(rt->gcContext());
      }
    }
    for (auto map = zone->cellIterUnsafe<DictionaryPropMap>(); !map.done();
         map.next()) {
      if (map->asLinked()->hasTable()) {
        map->asLinked()->purgeTable(rt->gcContext());
      }
    }
  }
}

// The debugger keeps track of the URLs for the sources of each realm's scripts.
// These URLs are purged on shrinking GCs.
void GCRuntime::purgeSourceURLsForShrinkingGC() {
  gcstats::AutoPhase ap(stats(), gcstats::PhaseKind::PURGE_SOURCE_URLS);
  for (GCZonesIter zone(this); !zone.done(); zone.next()) {
    // URLs are not tracked for realms in the system zone.
    if (!canRelocateZone(zone) || zone->isSystemZone()) {
      continue;
    }
    for (CompartmentsInZoneIter comp(zone); !comp.done(); comp.next()) {
      for (RealmsInCompartmentIter realm(comp); !realm.done(); realm.next()) {
        GlobalObject* global = realm.get()->unsafeUnbarrieredMaybeGlobal();
        if (global) {
          global->clearSourceURLSHolder();
        }
      }
    }
  }
}

void GCRuntime::unmarkWeakMaps() {
  for (GCZonesIter zone(this); !zone.done(); zone.next()) {
    /* Unmark all weak maps in the zones being collected. */
    WeakMapBase::unmarkZone(zone);
  }
}

bool GCRuntime::beginPreparePhase(JS::GCReason reason, AutoGCSession& session) {
  gcstats::AutoPhase ap(stats(), gcstats::PhaseKind::PREPARE);

  if (!prepareZonesForCollection(reason, &isFull.ref())) {
    return false;
  }

  /*
   * Start a parallel task to clear all mark state for the zones we are
   * collecting. This is linear in the size of the heap we are collecting and so
   * can be slow. This usually happens concurrently with the mutator and GC
   * proper does not start until this is complete.
   */
  unmarkTask.initZones();
  if (useBackgroundThreads) {
    unmarkTask.start();
  } else {
    unmarkTask.runFromMainThread();
  }

  /*
   * Process any queued source compressions during the start of a major
   * GC.
   *
   * Bug 1650075: When we start passing GCOptions::Shutdown for
   * GCReason::XPCONNECT_SHUTDOWN GCs we can remove the extra check.
   */
  if (!isShutdownGC() && reason != JS::GCReason::XPCONNECT_SHUTDOWN) {
    StartHandlingCompressionsOnGC(rt);
  }

  return true;
}

BackgroundUnmarkTask::BackgroundUnmarkTask(GCRuntime* gc)
    : GCParallelTask(gc, gcstats::PhaseKind::UNMARK) {}

void BackgroundUnmarkTask::initZones() {
  MOZ_ASSERT(isIdle());
  MOZ_ASSERT(zones.empty());
  MOZ_ASSERT(!isCancelled());

  // We can't safely iterate the zones vector from another thread so we copy the
  // zones to be collected into another vector.
  AutoEnterOOMUnsafeRegion oomUnsafe;
  for (GCZonesIter zone(gc); !zone.done(); zone.next()) {
    if (!zones.append(zone.get())) {
      oomUnsafe.crash("BackgroundUnmarkTask::initZones");
    }

    zone->arenas.clearFreeLists();
    zone->arenas.moveArenasToCollectingLists();
  }
}

void BackgroundUnmarkTask::run(AutoLockHelperThreadState& helperTheadLock) {
  AutoUnlockHelperThreadState unlock(helperTheadLock);

  for (Zone* zone : zones) {
    for (auto kind : AllAllocKinds()) {
      ArenaList& arenas = zone->arenas.collectingArenaList(kind);
      for (ArenaListIter arena(arenas.head()); !arena.done(); arena.next()) {
        arena->unmarkAll();
        if (isCancelled()) {
          break;
        }
      }
    }
  }

  zones.clear();
}

void GCRuntime::endPreparePhase(JS::GCReason reason) {
  MOZ_ASSERT(unmarkTask.isIdle());

  for (GCZonesIter zone(this); !zone.done(); zone.next()) {
    /*
     * In an incremental GC, clear the area free lists to ensure that subsequent
     * allocations refill them and end up marking new cells back. See
     * arenaAllocatedDuringGC().
     */
    zone->arenas.clearFreeLists();

    zone->markedStrings = 0;
    zone->finalizedStrings = 0;

    zone->setPreservingCode(false);

#ifdef JS_GC_ZEAL
    if (hasZealMode(ZealMode::YieldBeforeRootMarking)) {
      for (auto kind : AllAllocKinds()) {
        for (ArenaIter arena(zone, kind); !arena.done(); arena.next()) {
          arena->checkNoMarkedCells();
        }
      }
    }
#endif
  }

  // Discard JIT code more aggressively if the process is approaching its
  // executable code limit.
  bool canAllocateMoreCode = jit::CanLikelyAllocateMoreExecutableMemory();
  auto currentTime = TimeStamp::Now();

  Compartment* activeCompartment = nullptr;
  jit::JitActivationIterator activation(rt->mainContextFromOwnThread());
  if (!activation.done()) {
    activeCompartment = activation->compartment();
  }

  for (CompartmentsIter c(rt); !c.done(); c.next()) {
    c->gcState.scheduledForDestruction = false;
    c->gcState.maybeAlive = false;
    c->gcState.hasEnteredRealm = false;
    bool isActiveCompartment = c == activeCompartment;
    for (RealmsInCompartmentIter r(c); !r.done(); r.next()) {
      if (r->shouldTraceGlobal() || !r->zone()->isGCScheduled()) {
        c->gcState.maybeAlive = true;
      }
      if (shouldPreserveJITCode(r, currentTime, reason, canAllocateMoreCode,
                                isActiveCompartment)) {
        r->zone()->setPreservingCode(true);
      }
      if (r->hasBeenEnteredIgnoringJit()) {
        c->gcState.hasEnteredRealm = true;
      }
    }
  }

  /*
   * Perform remaining preparation work that must take place in the first true
   * GC slice.
   */

  {
    gcstats::AutoPhase ap1(stats(), gcstats::PhaseKind::PREPARE);

    AutoLockHelperThreadState helperLock;

    /* Clear mark state for WeakMaps in parallel with other work. */
    AutoRunParallelTask unmarkWeakMaps(this, &GCRuntime::unmarkWeakMaps,
                                       gcstats::PhaseKind::UNMARK_WEAKMAPS,
                                       GCUse::Unspecified, helperLock);

    AutoUnlockHelperThreadState unlock(helperLock);

    // Discard JIT code. For incremental collections, the sweep phase will
    // also discard JIT code.
    discardJITCodeForGC();

    /*
     * Relazify functions after discarding JIT code (we can't relazify
     * functions with JIT code) and before the actual mark phase, so that
     * the current GC can collect the JSScripts we're unlinking here.  We do
     * this only when we're performing a shrinking GC, as too much
     * relazification can cause performance issues when we have to reparse
     * the same functions over and over.
     */
    if (isShrinkingGC()) {
      relazifyFunctionsForShrinkingGC();
      purgePropMapTablesForShrinkingGC();
      purgeSourceURLsForShrinkingGC();
    }

    /*
     * We must purge the runtime at the beginning of an incremental GC. The
     * danger if we purge later is that the snapshot invariant of
     * incremental GC will be broken, as follows. If some object is
     * reachable only through some cache (say the dtoaCache) then it will
     * not be part of the snapshot.  If we purge after root marking, then
     * the mutator could obtain a pointer to the object and start using
     * it. This object might never be marked, so a GC hazard would exist.
     */
    purgeRuntime();

    startBackgroundFreeAfterMinorGC();

    if (isShutdownGC()) {
      /* Clear any engine roots that may hold external data live. */
      for (GCZonesIter zone(this); !zone.done(); zone.next()) {
        zone->clearRootsForShutdownGC();
      }

#ifdef DEBUG
      testMarkQueue.clear();
      queuePos = 0;
#endif
    }
  }

#ifdef DEBUG
  if (fullCompartmentChecks) {
    checkForCompartmentMismatches();
  }
#endif
}

AutoUpdateLiveCompartments::AutoUpdateLiveCompartments(GCRuntime* gc) : gc(gc) {
  for (GCCompartmentsIter c(gc->rt); !c.done(); c.next()) {
    c->gcState.hasMarkedCells = false;
  }
}

AutoUpdateLiveCompartments::~AutoUpdateLiveCompartments() {
  for (GCCompartmentsIter c(gc->rt); !c.done(); c.next()) {
    if (c->gcState.hasMarkedCells) {
      c->gcState.maybeAlive = true;
    }
  }
}

Zone::GCState Zone::initialMarkingState() const {
  if (isAtomsZone()) {
    // Don't delay gray marking in the atoms zone like we do in other zones.
    return MarkBlackAndGray;
  }

  return MarkBlackOnly;
}

void GCRuntime::beginMarkPhase(AutoGCSession& session) {
  /*
   * Mark phase.
   */
  gcstats::AutoPhase ap(stats(), gcstats::PhaseKind::MARK);

  // This is the slice we actually start collecting. The number can be used to
  // check whether a major GC has started so we must not increment it until we
  // get here.
  incMajorGcNumber();

  MOZ_ASSERT(!hasDelayedMarking());
  for (auto& marker : markers) {
    marker->start();
  }

#ifdef DEBUG
  queuePos = 0;
  queueMarkColor.reset();
#endif

  for (GCZonesIter zone(this); !zone.done(); zone.next()) {
    // Incremental marking barriers are enabled at this point.
    zone->changeGCState(Zone::Prepare, zone->initialMarkingState());

    // Merge arenas allocated during the prepare phase, then move all arenas to
    // the collecting arena lists.
    zone->arenas.mergeArenasFromCollectingLists();
    zone->arenas.moveArenasToCollectingLists();

    for (RealmsInZoneIter realm(zone); !realm.done(); realm.next()) {
      realm->clearAllocatedDuringGC();
    }
  }

  if (rt->isBeingDestroyed()) {
    checkNoRuntimeRoots(session);
  } else {
    AutoUpdateLiveCompartments updateLive(this);
    marker().setRootMarkingMode(true);
    traceRuntimeForMajorGC(marker().tracer(), session);
    marker().setRootMarkingMode(false);
  }

  updateSchedulingStateOnGCStart();
  stats().measureInitialHeapSize();
}

void GCRuntime::findDeadCompartments() {
  gcstats::AutoPhase ap1(stats(), gcstats::PhaseKind::FIND_DEAD_COMPARTMENTS);

  /*
   * This code ensures that if a compartment is "dead", then it will be
   * collected in this GC. A compartment is considered dead if its maybeAlive
   * flag is false. The maybeAlive flag is set if:
   *
   *   (1) the compartment has been entered (set in beginMarkPhase() above)
   *   (2) the compartment's zone is not being collected (set in
   *       beginMarkPhase() above)
   *   (3) an object in the compartment was marked during root marking, either
   *       as a black root or a gray root. This is arranged by
   *       SetCompartmentHasMarkedCells and AutoUpdateLiveCompartments.
   *   (4) the compartment has incoming cross-compartment edges from another
   *       compartment that has maybeAlive set (set by this method).
   *
   * If the maybeAlive is false, then we set the scheduledForDestruction flag.
   * At the end of the GC, we look for compartments where
   * scheduledForDestruction is true. These are compartments that were somehow
   * "revived" during the incremental GC. If any are found, we do a special,
   * non-incremental GC of those compartments to try to collect them.
   *
   * Compartments can be revived for a variety of reasons. On reason is bug
   * 811587, where a reflector that was dead can be revived by DOM code that
   * still refers to the underlying DOM node.
   *
   * Read barriers and allocations can also cause revival. This might happen
   * during a function like JS_TransplantObject, which iterates over all
   * compartments, live or dead, and operates on their objects. See bug 803376
   * for details on this problem. To avoid the problem, we try to avoid
   * allocation and read barriers during JS_TransplantObject and the like.
   */

  // Propagate the maybeAlive flag via cross-compartment edges.

  Vector<Compartment*, 0, js::SystemAllocPolicy> workList;

  for (CompartmentsIter comp(rt); !comp.done(); comp.next()) {
    if (comp->gcState.maybeAlive) {
      if (!workList.append(comp)) {
        return;
      }
    }
  }

  while (!workList.empty()) {
    Compartment* comp = workList.popCopy();
    for (Compartment::WrappedObjectCompartmentEnum e(comp); !e.empty();
         e.popFront()) {
      Compartment* dest = e.front();
      if (!dest->gcState.maybeAlive) {
        dest->gcState.maybeAlive = true;
        if (!workList.append(dest)) {
          return;
        }
      }
    }
  }

  // Set scheduledForDestruction based on maybeAlive.

  for (GCCompartmentsIter comp(rt); !comp.done(); comp.next()) {
    MOZ_ASSERT(!comp->gcState.scheduledForDestruction);
    if (!comp->gcState.maybeAlive) {
      comp->gcState.scheduledForDestruction = true;
    }
  }
}

void GCRuntime::updateSchedulingStateOnGCStart() {
  heapSize.updateOnGCStart();

  // Update memory counters for the zones we are collecting.
  for (GCZonesIter zone(this); !zone.done(); zone.next()) {
    zone->updateSchedulingStateOnGCStart();
  }
}

inline bool GCRuntime::canMarkInParallel() const {
#if defined(DEBUG) || defined(JS_OOM_BREAKPOINT)
  // OOM testing limits the engine to using a single helper thread.
  if (oom::simulator.targetThread() == THREAD_TYPE_GCPARALLEL) {
    return false;
  }
#endif

  return markers.length() > 1 && stats().initialCollectedBytes() >=
                                     tunables.parallelMarkingThresholdBytes();
}

IncrementalProgress GCRuntime::markUntilBudgetExhausted(
    SliceBudget& sliceBudget, ParallelMarking allowParallelMarking,
    ShouldReportMarkTime reportTime) {
  // Run a marking slice and return whether the stack is now empty.

  AutoMajorGCProfilerEntry s(this);

  if (processTestMarkQueue() == QueueYielded) {
    return NotFinished;
  }

  if (allowParallelMarking && canMarkInParallel()) {
    MOZ_ASSERT(parallelMarkingEnabled);
    MOZ_ASSERT(reportTime);
    MOZ_ASSERT(!isBackgroundMarking());

    ParallelMarker pm(this);
    if (!pm.mark(sliceBudget)) {
      return NotFinished;
    }

    assertNoMarkingWork();
    return Finished;
  }

#ifdef DEBUG
  AutoSetThreadIsMarking threadIsMarking;
#endif  // DEBUG

  return marker().markUntilBudgetExhausted(sliceBudget, reportTime)
             ? Finished
             : NotFinished;
}

void GCRuntime::drainMarkStack() {
  auto unlimited = SliceBudget::unlimited();
  MOZ_RELEASE_ASSERT(marker().markUntilBudgetExhausted(unlimited));
}

#ifdef DEBUG

const GCVector<HeapPtr<JS::Value>, 0, SystemAllocPolicy>&
GCRuntime::getTestMarkQueue() const {
  return testMarkQueue.get();
}

bool GCRuntime::appendTestMarkQueue(const JS::Value& value) {
  return testMarkQueue.append(value);
}

void GCRuntime::clearTestMarkQueue() {
  testMarkQueue.clear();
  queuePos = 0;
}

size_t GCRuntime::testMarkQueuePos() const { return queuePos; }

#endif

GCRuntime::MarkQueueProgress GCRuntime::processTestMarkQueue() {
#ifdef DEBUG
  if (testMarkQueue.empty()) {
    return QueueComplete;
  }

  if (queueMarkColor == mozilla::Some(MarkColor::Gray) &&
      state() != State::Sweep) {
    return QueueSuspended;
  }

  // If the queue wants to be gray marking, but we've pushed a black object
  // since set-color-gray was processed, then we can't switch to gray and must
  // again wait until gray marking is possible.
  //
  // Remove this code if the restriction against marking gray during black is
  // relaxed.
  if (queueMarkColor == mozilla::Some(MarkColor::Gray) &&
      marker().hasBlackEntries()) {
    return QueueSuspended;
  }

  // If the queue wants to be marking a particular color, switch to that color.
  // In any case, restore the mark color to whatever it was when we entered
  // this function.
  bool willRevertToGray = marker().markColor() == MarkColor::Gray;
  AutoSetMarkColor autoRevertColor(
      marker(), queueMarkColor.valueOr(marker().markColor()));

  // Process the mark queue by taking each object in turn, pushing it onto the
  // mark stack, and processing just the top element with processMarkStackTop
  // without recursing into reachable objects.
  while (queuePos < testMarkQueue.length()) {
    Value val = testMarkQueue[queuePos++].get();
    if (val.isObject()) {
      JSObject* obj = &val.toObject();
      JS::Zone* zone = obj->zone();
      if (!zone->isGCMarking() || obj->isMarkedAtLeast(marker().markColor())) {
        continue;
      }

      // If we have started sweeping, obey sweep group ordering. But note that
      // we will first be called during the initial sweep slice, when the sweep
      // group indexes have not yet been computed. In that case, we can mark
      // freely.
      if (state() == State::Sweep && initialState != State::Sweep) {
        if (zone->gcSweepGroupIndex < getCurrentSweepGroupIndex()) {
          // Too late. This must have been added after we started collecting,
          // and we've already processed its sweep group. Skip it.
          continue;
        }
        if (zone->gcSweepGroupIndex > getCurrentSweepGroupIndex()) {
          // Not ready yet. Wait until we reach the object's sweep group.
          queuePos--;
          return QueueSuspended;
        }
      }

      if (marker().markColor() == MarkColor::Gray &&
          zone->isGCMarkingBlackOnly()) {
        // Have not yet reached the point where we can mark this object, so
        // continue with the GC.
        queuePos--;
        return QueueSuspended;
      }

      if (marker().markColor() == MarkColor::Black && willRevertToGray) {
        // If we put any black objects on the stack, we wouldn't be able to
        // return to gray marking. So delay the marking until we're back to
        // black marking.
        queuePos--;
        return QueueSuspended;
      }

      // Mark the object and push it onto the stack.
      size_t oldPosition = marker().stack.position();
      marker().markAndTraverse<NormalMarkingOptions>(obj);

      // If we overflow the stack here and delay marking, then we won't be
      // testing what we think we're testing.
      if (marker().stack.position() == oldPosition) {
        MOZ_ASSERT(obj->asTenured().arena()->onDelayedMarkingList());
        AutoEnterOOMUnsafeRegion oomUnsafe;
        oomUnsafe.crash("Overflowed stack while marking test queue");
      }

      SliceBudget unlimited = SliceBudget::unlimited();
      marker().processMarkStackTop<NormalMarkingOptions>(unlimited);
    } else if (val.isString()) {
      JSLinearString* str = &val.toString()->asLinear();
      if (js::StringEqualsLiteral(str, "yield") && isIncrementalGc()) {
        return QueueYielded;
      } else if (js::StringEqualsLiteral(str, "enter-weak-marking-mode") ||
                 js::StringEqualsLiteral(str, "abort-weak-marking-mode")) {
        if (marker().isRegularMarking()) {
          // We can't enter weak marking mode at just any time, so instead
          // we'll stop processing the queue and continue on with the GC. Once
          // we enter weak marking mode, we can continue to the rest of the
          // queue. Note that we will also suspend for aborting, and then abort
          // the earliest following weak marking mode.
          queuePos--;
          return QueueSuspended;
        }
        if (js::StringEqualsLiteral(str, "abort-weak-marking-mode")) {
          marker().abortLinearWeakMarking();
        }
      } else if (js::StringEqualsLiteral(str, "drain")) {
        auto unlimited = SliceBudget::unlimited();
        MOZ_RELEASE_ASSERT(
            marker().markUntilBudgetExhausted(unlimited, DontReportMarkTime));
      } else if (js::StringEqualsLiteral(str, "set-color-gray")) {
        queueMarkColor = mozilla::Some(MarkColor::Gray);
        if (state() != State::Sweep || marker().hasBlackEntries()) {
          // Cannot mark gray yet, so continue with the GC.
          queuePos--;
          return QueueSuspended;
        }
        marker().setMarkColor(MarkColor::Gray);
      } else if (js::StringEqualsLiteral(str, "set-color-black")) {
        queueMarkColor = mozilla::Some(MarkColor::Black);
        marker().setMarkColor(MarkColor::Black);
      } else if (js::StringEqualsLiteral(str, "unset-color")) {
        queueMarkColor.reset();
      }
    }
  }
#endif

  return QueueComplete;
}

static bool IsEmergencyGC(JS::GCReason reason) {
  return reason == JS::GCReason::LAST_DITCH ||
         reason == JS::GCReason::MEM_PRESSURE;
}

void GCRuntime::finishCollection(JS::GCReason reason) {
  assertBackgroundSweepingFinished();

  MOZ_ASSERT(!hasDelayedMarking());
  for (auto& marker : markers) {
    marker->stop();
  }

  maybeStopPretenuring();

  if (IsEmergencyGC(reason)) {
    waitBackgroundFreeEnd();
  }

  TimeStamp currentTime = TimeStamp::Now();

  updateSchedulingStateAfterCollection(currentTime);

  for (GCZonesIter zone(this); !zone.done(); zone.next()) {
    zone->changeGCState(Zone::Finished, Zone::NoGC);
    zone->notifyObservingDebuggers();
  }

#ifdef JS_GC_ZEAL
  clearSelectedForMarking();
#endif

  schedulingState.updateHighFrequencyMode(lastGCEndTime_, currentTime,
                                          tunables);
  lastGCEndTime_ = currentTime;

  checkGCStateNotInUse();
}

void GCRuntime::checkGCStateNotInUse() {
#ifdef DEBUG
  for (auto& marker : markers) {
    MOZ_ASSERT(!marker->isActive());
    MOZ_ASSERT(marker->isDrained());
  }
  MOZ_ASSERT(!hasDelayedMarking());

  MOZ_ASSERT(!lastMarkSlice);

  for (ZonesIter zone(this, WithAtoms); !zone.done(); zone.next()) {
    if (zone->wasCollected()) {
      zone->arenas.checkGCStateNotInUse();
    }
    MOZ_ASSERT(!zone->wasGCStarted());
    MOZ_ASSERT(!zone->needsIncrementalBarrier());
    MOZ_ASSERT(!zone->isOnList());
  }

  MOZ_ASSERT(zonesToMaybeCompact.ref().isEmpty());
  MOZ_ASSERT(cellsToAssertNotGray.ref().empty());

  AutoLockHelperThreadState lock;
  MOZ_ASSERT(!requestSliceAfterBackgroundTask);
  MOZ_ASSERT(unmarkTask.isIdle(lock));
  MOZ_ASSERT(markTask.isIdle(lock));
  MOZ_ASSERT(sweepTask.isIdle(lock));
  MOZ_ASSERT(decommitTask.isIdle(lock));
#endif
}

void GCRuntime::maybeStopPretenuring() {
  nursery().maybeStopPretenuring(this);

  for (GCZonesIter zone(this); !zone.done(); zone.next()) {
    if (!zone->nurseryStringsDisabled) {
      continue;
    }

    // Count the number of strings before the major GC.
    size_t numStrings = zone->markedStrings + zone->finalizedStrings;
    double rate = double(zone->finalizedStrings) / double(numStrings);
    if (rate > tunables.stopPretenureStringThreshold()) {
      zone->markedStrings = 0;
      zone->finalizedStrings = 0;
      zone->nurseryStringsDisabled = false;
      nursery().updateAllocFlagsForZone(zone);
    }
  }
}

void GCRuntime::updateSchedulingStateAfterCollection(TimeStamp currentTime) {
  TimeDuration totalGCTime = stats().totalGCTime();
  size_t totalInitialBytes = stats().initialCollectedBytes();

  for (GCZonesIter zone(this); !zone.done(); zone.next()) {
    if (tunables.balancedHeapLimitsEnabled() && totalInitialBytes != 0) {
      zone->updateCollectionRate(totalGCTime, totalInitialBytes);
    }
    zone->clearGCSliceThresholds();
    zone->updateGCStartThresholds(*this);
  }
}

void GCRuntime::updateAllGCStartThresholds() {
  for (ZonesIter zone(this, WithAtoms); !zone.done(); zone.next()) {
    zone->updateGCStartThresholds(*this);
  }
}

void GCRuntime::updateAllocationRates() {
  // Calculate mutator time since the last update. This ignores the fact that
  // the zone could have been created since the last update.

  TimeStamp currentTime = TimeStamp::Now();
  TimeDuration totalTime = currentTime - lastAllocRateUpdateTime;
  if (collectorTimeSinceAllocRateUpdate >= totalTime) {
    // It shouldn't happen but occasionally we see collector time being larger
    // than total time. Skip the update in that case.
    return;
  }

  TimeDuration mutatorTime = totalTime - collectorTimeSinceAllocRateUpdate;

  for (AllZonesIter zone(this); !zone.done(); zone.next()) {
    zone->updateAllocationRate(mutatorTime);
    zone->updateGCStartThresholds(*this);
  }

  lastAllocRateUpdateTime = currentTime;
  collectorTimeSinceAllocRateUpdate = TimeDuration();
}

static const char* GCHeapStateToLabel(JS::HeapState heapState) {
  switch (heapState) {
    case JS::HeapState::MinorCollecting:
      return "js::Nursery::collect";
    case JS::HeapState::MajorCollecting:
      return "js::GCRuntime::collect";
    default:
      MOZ_CRASH("Unexpected heap state when pushing GC profiling stack frame");
  }
  MOZ_ASSERT_UNREACHABLE("Should have exhausted every JS::HeapState variant!");
  return nullptr;
}

static JS::ProfilingCategoryPair GCHeapStateToProfilingCategory(
    JS::HeapState heapState) {
  return heapState == JS::HeapState::MinorCollecting
             ? JS::ProfilingCategoryPair::GCCC_MinorGC
             : JS::ProfilingCategoryPair::GCCC_MajorGC;
}

/* Start a new heap session. */
AutoHeapSession::AutoHeapSession(GCRuntime* gc, JS::HeapState heapState)
    : gc(gc), prevState(gc->heapState_) {
  MOZ_ASSERT(CurrentThreadCanAccessRuntime(gc->rt));
  MOZ_ASSERT(prevState == JS::HeapState::Idle ||
             (prevState == JS::HeapState::MajorCollecting &&
              heapState == JS::HeapState::MinorCollecting));
  MOZ_ASSERT(heapState != JS::HeapState::Idle);

  gc->heapState_ = heapState;

  if (heapState == JS::HeapState::MinorCollecting ||
      heapState == JS::HeapState::MajorCollecting) {
    profilingStackFrame.emplace(gc->rt->mainContextFromOwnThread(),
                                GCHeapStateToLabel(heapState),
                                GCHeapStateToProfilingCategory(heapState));
  }
}

AutoHeapSession::~AutoHeapSession() {
  MOZ_ASSERT(JS::RuntimeHeapIsBusy());
  gc->heapState_ = prevState;
}

static const char* MajorGCStateToLabel(State state) {
  switch (state) {
    case State::Mark:
      return "js::GCRuntime::markUntilBudgetExhausted";
    case State::Sweep:
      return "js::GCRuntime::performSweepActions";
    case State::Compact:
      return "js::GCRuntime::compactPhase";
    default:
      MOZ_CRASH("Unexpected heap state when pushing GC profiling stack frame");
  }

  MOZ_ASSERT_UNREACHABLE("Should have exhausted every State variant!");
  return nullptr;
}

static JS::ProfilingCategoryPair MajorGCStateToProfilingCategory(State state) {
  switch (state) {
    case State::Mark:
      return JS::ProfilingCategoryPair::GCCC_MajorGC_Mark;
    case State::Sweep:
      return JS::ProfilingCategoryPair::GCCC_MajorGC_Sweep;
    case State::Compact:
      return JS::ProfilingCategoryPair::GCCC_MajorGC_Compact;
    default:
      MOZ_CRASH("Unexpected heap state when pushing GC profiling stack frame");
  }
}

AutoMajorGCProfilerEntry::AutoMajorGCProfilerEntry(GCRuntime* gc)
    : AutoGeckoProfilerEntry(gc->rt->mainContextFromAnyThread(),
                             MajorGCStateToLabel(gc->state()),
                             MajorGCStateToProfilingCategory(gc->state())) {
  MOZ_ASSERT(gc->heapState() == JS::HeapState::MajorCollecting);
}

GCRuntime::IncrementalResult GCRuntime::resetIncrementalGC(
    GCAbortReason reason) {
  MOZ_ASSERT(reason != GCAbortReason::None);

  // Drop as much work as possible from an ongoing incremental GC so
  // we can start a new GC after it has finished.
  if (incrementalState == State::NotActive) {
    return IncrementalResult::Ok;
  }

  AutoGCSession session(this, JS::HeapState::MajorCollecting);

  switch (incrementalState) {
    case State::NotActive:
    case State::MarkRoots:
    case State::Finish:
      MOZ_CRASH("Unexpected GC state in resetIncrementalGC");
      break;

    case State::Prepare:
      unmarkTask.cancelAndWait();

      for (GCZonesIter zone(this); !zone.done(); zone.next()) {
        zone->changeGCState(Zone::Prepare, Zone::NoGC);
        zone->clearGCSliceThresholds();
        zone->arenas.clearFreeLists();
        zone->arenas.mergeArenasFromCollectingLists();
      }

      incrementalState = State::NotActive;
      checkGCStateNotInUse();
      break;

    case State::Mark: {
      // Cancel any ongoing marking.
      for (auto& marker : markers) {
        marker->reset();
      }
      resetDelayedMarking();

      for (GCCompartmentsIter c(rt); !c.done(); c.next()) {
        resetGrayList(c);
      }

      for (GCZonesIter zone(this); !zone.done(); zone.next()) {
        zone->changeGCState(zone->initialMarkingState(), Zone::NoGC);
        zone->clearGCSliceThresholds();
        zone->arenas.unmarkPreMarkedFreeCells();
        zone->arenas.mergeArenasFromCollectingLists();
      }

      {
        AutoLockHelperThreadState lock;
        lifoBlocksToFree.ref().freeAll();
      }

      lastMarkSlice = false;
      incrementalState = State::Finish;

#ifdef DEBUG
      for (auto& marker : markers) {
        MOZ_ASSERT(!marker->shouldCheckCompartments());
      }
#endif

      break;
    }

    case State::Sweep: {
      // Finish sweeping the current sweep group, then abort.
      for (CompartmentsIter c(rt); !c.done(); c.next()) {
        c->gcState.scheduledForDestruction = false;
      }

      abortSweepAfterCurrentGroup = true;
      isCompacting = false;

      break;
    }

    case State::Finalize: {
      isCompacting = false;
      break;
    }

    case State::Compact: {
      // Skip any remaining zones that would have been compacted.
      MOZ_ASSERT(isCompacting);
      startedCompacting = true;
      zonesToMaybeCompact.ref().clear();
      break;
    }

    case State::Decommit: {
      break;
    }
  }

  stats().reset(reason);

  return IncrementalResult::ResetIncremental;
}

AutoDisableBarriers::AutoDisableBarriers(GCRuntime* gc) : gc(gc) {
  /*
   * Clear needsIncrementalBarrier early so we don't do any write barriers
   * during sweeping.
   */
  for (GCZonesIter zone(gc); !zone.done(); zone.next()) {
    if (zone->isGCMarking()) {
      MOZ_ASSERT(zone->needsIncrementalBarrier());
      zone->setNeedsIncrementalBarrier(false);
    }
    MOZ_ASSERT(!zone->needsIncrementalBarrier());
  }
}

AutoDisableBarriers::~AutoDisableBarriers() {
  for (GCZonesIter zone(gc); !zone.done(); zone.next()) {
    MOZ_ASSERT(!zone->needsIncrementalBarrier());
    if (zone->isGCMarking()) {
      zone->setNeedsIncrementalBarrier(true);
    }
  }
}

static bool NeedToCollectNursery(GCRuntime* gc) {
  return !gc->nursery().isEmpty() || !gc->storeBuffer().isEmpty();
}

#ifdef DEBUG
static const char* DescribeBudget(const SliceBudget& budget) {
  MOZ_ASSERT(TlsContext.get()->isMainThreadContext());
  constexpr size_t length = 32;
  static char buffer[length];
  budget.describe(buffer, length);
  return buffer;
}
#endif

static bool ShouldPauseMutatorWhileWaiting(const SliceBudget& budget,
                                           JS::GCReason reason,
                                           bool budgetWasIncreased) {
  // When we're nearing the incremental limit at which we will finish the
  // collection synchronously, pause the main thread if there is only background
  // GC work happening. This allows the GC to catch up and avoid hitting the
  // limit.
  return budget.isTimeBudget() &&
         (reason == JS::GCReason::ALLOC_TRIGGER ||
          reason == JS::GCReason::TOO_MUCH_MALLOC) &&
         budgetWasIncreased;
}

void GCRuntime::incrementalSlice(SliceBudget& budget, JS::GCReason reason,
                                 bool budgetWasIncreased) {
  MOZ_ASSERT_IF(isIncrementalGCInProgress(), isIncremental);

  AutoSetThreadIsPerformingGC performingGC(rt->gcContext());

  AutoGCSession session(this, JS::HeapState::MajorCollecting);

  bool destroyingRuntime = (reason == JS::GCReason::DESTROY_RUNTIME);

  initialState = incrementalState;
  isIncremental = !budget.isUnlimited();
  useBackgroundThreads = ShouldUseBackgroundThreads(isIncremental, reason);

#ifdef JS_GC_ZEAL
  // Do the incremental collection type specified by zeal mode if the collection
  // was triggered by runDebugGC() and incremental GC has not been cancelled by
  // resetIncrementalGC().
  useZeal = isIncremental && reason == JS::GCReason::DEBUG_GC;
#endif

#ifdef DEBUG
  stats().log(
      "Incremental: %d, lastMarkSlice: %d, useZeal: %d, budget: %s, "
      "budgetWasIncreased: %d",
      bool(isIncremental), bool(lastMarkSlice), bool(useZeal),
      DescribeBudget(budget), budgetWasIncreased);
#endif

  if (useZeal && hasIncrementalTwoSliceZealMode()) {
    // Yields between slices occurs at predetermined points in these modes; the
    // budget is not used. |isIncremental| is still true.
    stats().log("Using unlimited budget for two-slice zeal mode");
    budget = SliceBudget::unlimited();
  }

  bool shouldPauseMutator =
      ShouldPauseMutatorWhileWaiting(budget, reason, budgetWasIncreased);

  switch (incrementalState) {
    case State::NotActive:
      startCollection(reason);

      incrementalState = State::Prepare;
      if (!beginPreparePhase(reason, session)) {
        incrementalState = State::NotActive;
        break;
      }

      if (useZeal && hasZealMode(ZealMode::YieldBeforeRootMarking)) {
        break;
      }

      [[fallthrough]];

    case State::Prepare:
      if (waitForBackgroundTask(unmarkTask, budget, shouldPauseMutator,
                                DontTriggerSliceWhenFinished) == NotFinished) {
        break;
      }

      incrementalState = State::MarkRoots;
      [[fallthrough]];

    case State::MarkRoots:
      if (NeedToCollectNursery(this)) {
        collectNurseryFromMajorGC(reason);
      }

      endPreparePhase(reason);
      beginMarkPhase(session);
      incrementalState = State::Mark;

      if (useZeal && hasZealMode(ZealMode::YieldBeforeMarking) &&
          isIncremental) {
        break;
      }

      [[fallthrough]];

    case State::Mark:
      if (mightSweepInThisSlice(budget.isUnlimited())) {
        // Trace wrapper rooters before marking if we might start sweeping in
        // this slice.
        rt->mainContextFromOwnThread()->traceWrapperGCRooters(
            marker().tracer());
      }

      {
        gcstats::AutoPhase ap(stats(), gcstats::PhaseKind::MARK);
        if (markUntilBudgetExhausted(budget, AllowParallelMarking) ==
            NotFinished) {
          break;
        }
      }

      assertNoMarkingWork();

      /*
       * There are a number of reasons why we break out of collection here,
       * either ending the slice or to run a new interation of the loop in
       * GCRuntime::collect()
       */

      /*
       * In incremental GCs where we have already performed more than one
       * slice we yield after marking with the aim of starting the sweep in
       * the next slice, since the first slice of sweeping can be expensive.
       *
       * This is modified by the various zeal modes.  We don't yield in
       * YieldBeforeMarking mode and we always yield in YieldBeforeSweeping
       * mode.
       *
       * We will need to mark anything new on the stack when we resume, so
       * we stay in Mark state.
       */
      if (isIncremental && !lastMarkSlice) {
        if ((initialState == State::Mark &&
             !(useZeal && hasZealMode(ZealMode::YieldBeforeMarking))) ||
            (useZeal && hasZealMode(ZealMode::YieldBeforeSweeping))) {
          lastMarkSlice = true;
          stats().log("Yielding before starting sweeping");
          break;
        }
      }

      incrementalState = State::Sweep;
      lastMarkSlice = false;

      beginSweepPhase(reason, session);

      [[fallthrough]];

    case State::Sweep:
      if (storeBuffer().mayHavePointersToDeadCells()) {
        collectNurseryFromMajorGC(reason);
      }

      if (initialState == State::Sweep) {
        rt->mainContextFromOwnThread()->traceWrapperGCRooters(
            marker().tracer());
      }

      if (performSweepActions(budget) == NotFinished) {
        break;
      }

      endSweepPhase(destroyingRuntime);

      incrementalState = State::Finalize;

      [[fallthrough]];

    case State::Finalize:
      if (waitForBackgroundTask(sweepTask, budget, shouldPauseMutator,
                                TriggerSliceWhenFinished) == NotFinished) {
        break;
      }

      assertBackgroundSweepingFinished();

      {
        // Sweep the zones list now that background finalization is finished to
        // remove and free dead zones, compartments and realms.
        gcstats::AutoPhase ap1(stats(), gcstats::PhaseKind::SWEEP);
        gcstats::AutoPhase ap2(stats(), gcstats::PhaseKind::DESTROY);
        sweepZones(rt->gcContext(), destroyingRuntime);
      }

      MOZ_ASSERT(!startedCompacting);
      incrementalState = State::Compact;

      // Always yield before compacting since it is not incremental.
      if (isCompacting && !budget.isUnlimited()) {
        break;
      }

      [[fallthrough]];

    case State::Compact:
      if (isCompacting) {
        if (NeedToCollectNursery(this)) {
          collectNurseryFromMajorGC(reason);
        }

        storeBuffer().checkEmpty();
        if (!startedCompacting) {
          beginCompactPhase();
        }

        if (compactPhase(reason, budget, session) == NotFinished) {
          break;
        }

        endCompactPhase();
      }

      startDecommit();
      incrementalState = State::Decommit;

      [[fallthrough]];

    case State::Decommit:
      if (waitForBackgroundTask(decommitTask, budget, shouldPauseMutator,
                                TriggerSliceWhenFinished) == NotFinished) {
        break;
      }

      incrementalState = State::Finish;

      [[fallthrough]];

    case State::Finish:
      finishCollection(reason);
      incrementalState = State::NotActive;
      break;
  }

#ifdef DEBUG
  MOZ_ASSERT(safeToYield);
  for (auto& marker : markers) {
    MOZ_ASSERT(marker->markColor() == MarkColor::Black);
  }
  MOZ_ASSERT(!rt->gcContext()->hasJitCodeToPoison());
#endif
}

void GCRuntime::collectNurseryFromMajorGC(JS::GCReason reason) {
  collectNursery(gcOptions(), reason,
                 gcstats::PhaseKind::EVICT_NURSERY_FOR_MAJOR_GC);
}

bool GCRuntime::hasForegroundWork() const {
  switch (incrementalState) {
    case State::NotActive:
      // Incremental GC is not running and no work is pending.
      return false;
    case State::Prepare:
      // We yield in the Prepare state after starting unmarking.
      return !unmarkTask.wasStarted();
    case State::Finalize:
      // We yield in the Finalize state to wait for background sweeping.
      return !isBackgroundSweeping();
    case State::Decommit:
      // We yield in the Decommit state to wait for background decommit.
      return !decommitTask.wasStarted();
    default:
      // In all other states there is still work to do.
      return true;
  }
}

IncrementalProgress GCRuntime::waitForBackgroundTask(
    GCParallelTask& task, const SliceBudget& budget, bool shouldPauseMutator,
    ShouldTriggerSliceWhenFinished triggerSlice) {
  // Wait here in non-incremental collections, or if we want to pause the
  // mutator to let the GC catch up.
  if (budget.isUnlimited() || shouldPauseMutator) {
    gcstats::AutoPhase ap(stats(), gcstats::PhaseKind::WAIT_BACKGROUND_THREAD);
    Maybe<TimeStamp> deadline;
    if (budget.isTimeBudget()) {
      deadline.emplace(budget.deadline());
    }
    task.join(deadline);
  }

  // In incremental collections, yield if the task has not finished and
  // optionally request a slice to notify us when this happens.
  if (!budget.isUnlimited()) {
    AutoLockHelperThreadState lock;
    if (task.wasStarted(lock)) {
      if (triggerSlice) {
        requestSliceAfterBackgroundTask = true;
      }
      return NotFinished;
    }

    task.joinWithLockHeld(lock);
  }

  MOZ_ASSERT(task.isIdle());

  if (triggerSlice) {
    cancelRequestedGCAfterBackgroundTask();
  }

  return Finished;
}

GCAbortReason gc::IsIncrementalGCUnsafe(JSRuntime* rt) {
  MOZ_ASSERT(!rt->mainContextFromOwnThread()->suppressGC);

  if (!rt->gc.isIncrementalGCAllowed()) {
    return GCAbortReason::IncrementalDisabled;
  }

  return GCAbortReason::None;
}

inline void GCRuntime::checkZoneIsScheduled(Zone* zone, JS::GCReason reason,
                                            const char* trigger) {
#ifdef DEBUG
  if (zone->isGCScheduled()) {
    return;
  }

  fprintf(stderr,
          "checkZoneIsScheduled: Zone %p not scheduled as expected in %s GC "
          "for %s trigger\n",
          zone, JS::ExplainGCReason(reason), trigger);
  for (ZonesIter zone(this, WithAtoms); !zone.done(); zone.next()) {
    fprintf(stderr, "  Zone %p:%s%s\n", zone.get(),
            zone->isAtomsZone() ? " atoms" : "",
            zone->isGCScheduled() ? " scheduled" : "");
  }
  fflush(stderr);
  MOZ_CRASH("Zone not scheduled");
#endif
}

GCRuntime::IncrementalResult GCRuntime::budgetIncrementalGC(
    bool nonincrementalByAPI, JS::GCReason reason, SliceBudget& budget) {
  if (nonincrementalByAPI) {
    stats().nonincremental(GCAbortReason::NonIncrementalRequested);
    budget = SliceBudget::unlimited();

    // Reset any in progress incremental GC if this was triggered via the
    // API. This isn't required for correctness, but sometimes during tests
    // the caller expects this GC to collect certain objects, and we need
    // to make sure to collect everything possible.
    if (reason != JS::GCReason::ALLOC_TRIGGER) {
      return resetIncrementalGC(GCAbortReason::NonIncrementalRequested);
    }

    return IncrementalResult::Ok;
  }

  if (reason == JS::GCReason::ABORT_GC) {
    budget = SliceBudget::unlimited();
    stats().nonincremental(GCAbortReason::AbortRequested);
    return resetIncrementalGC(GCAbortReason::AbortRequested);
  }

  if (!budget.isUnlimited()) {
    GCAbortReason unsafeReason = IsIncrementalGCUnsafe(rt);
    if (unsafeReason == GCAbortReason::None) {
      if (reason == JS::GCReason::COMPARTMENT_REVIVED) {
        unsafeReason = GCAbortReason::CompartmentRevived;
      } else if (!incrementalGCEnabled) {
        unsafeReason = GCAbortReason::ModeChange;
      }
    }

    if (unsafeReason != GCAbortReason::None) {
      budget = SliceBudget::unlimited();
      stats().nonincremental(unsafeReason);
      return resetIncrementalGC(unsafeReason);
    }
  }

  GCAbortReason resetReason = GCAbortReason::None;
  for (ZonesIter zone(this, WithAtoms); !zone.done(); zone.next()) {
    if (zone->gcHeapSize.bytes() >=
        zone->gcHeapThreshold.incrementalLimitBytes()) {
      checkZoneIsScheduled(zone, reason, "GC bytes");
      budget = SliceBudget::unlimited();
      stats().nonincremental(GCAbortReason::GCBytesTrigger);
      if (zone->wasGCStarted() && zone->gcState() > Zone::Sweep) {
        resetReason = GCAbortReason::GCBytesTrigger;
      }
    }

    if (zone->mallocHeapSize.bytes() >=
        zone->mallocHeapThreshold.incrementalLimitBytes()) {
      checkZoneIsScheduled(zone, reason, "malloc bytes");
      budget = SliceBudget::unlimited();
      stats().nonincremental(GCAbortReason::MallocBytesTrigger);
      if (zone->wasGCStarted() && zone->gcState() > Zone::Sweep) {
        resetReason = GCAbortReason::MallocBytesTrigger;
      }
    }

    if (zone->jitHeapSize.bytes() >=
        zone->jitHeapThreshold.incrementalLimitBytes()) {
      checkZoneIsScheduled(zone, reason, "JIT code bytes");
      budget = SliceBudget::unlimited();
      stats().nonincremental(GCAbortReason::JitCodeBytesTrigger);
      if (zone->wasGCStarted() && zone->gcState() > Zone::Sweep) {
        resetReason = GCAbortReason::JitCodeBytesTrigger;
      }
    }

    if (isIncrementalGCInProgress() &&
        zone->isGCScheduled() != zone->wasGCStarted()) {
      budget = SliceBudget::unlimited();
      resetReason = GCAbortReason::ZoneChange;
    }
  }

  if (resetReason != GCAbortReason::None) {
    return resetIncrementalGC(resetReason);
  }

  return IncrementalResult::Ok;
}

bool GCRuntime::maybeIncreaseSliceBudget(SliceBudget& budget) {
  if (js::SupportDifferentialTesting()) {
    return false;
  }

  if (!budget.isTimeBudget() || !isIncrementalGCInProgress()) {
    return false;
  }

  bool wasIncreasedForLongCollections =
      maybeIncreaseSliceBudgetForLongCollections(budget);
  bool wasIncreasedForUgentCollections =
      maybeIncreaseSliceBudgetForUrgentCollections(budget);

  return wasIncreasedForLongCollections || wasIncreasedForUgentCollections;
}

// Return true if the budget is actually extended after rounding.
static bool ExtendBudget(SliceBudget& budget, double newDuration) {
  long newDurationMS = lround(newDuration);
  if (newDurationMS <= budget.timeBudget()) {
    return false;
  }

  bool idleTriggered = budget.idle;
  budget = SliceBudget(TimeBudget(newDuration), nullptr);  // Uninterruptible.
  budget.idle = idleTriggered;
  budget.extended = true;
  return true;
}

bool GCRuntime::maybeIncreaseSliceBudgetForLongCollections(
    SliceBudget& budget) {
  // For long-running collections, enforce a minimum time budget that increases
  // linearly with time up to a maximum.

  // All times are in milliseconds.
  struct BudgetAtTime {
    double time;
    double budget;
  };
  const BudgetAtTime MinBudgetStart{1500, 0.0};
  const BudgetAtTime MinBudgetEnd{2500, 100.0};

  double totalTime = (TimeStamp::Now() - lastGCStartTime()).ToMilliseconds();

  double minBudget =
      LinearInterpolate(totalTime, MinBudgetStart.time, MinBudgetStart.budget,
                        MinBudgetEnd.time, MinBudgetEnd.budget);

  return ExtendBudget(budget, minBudget);
}

bool GCRuntime::maybeIncreaseSliceBudgetForUrgentCollections(
    SliceBudget& budget) {
  // Enforce a minimum time budget based on how close we are to the incremental
  // limit.

  size_t minBytesRemaining = SIZE_MAX;
  for (AllZonesIter zone(this); !zone.done(); zone.next()) {
    if (!zone->wasGCStarted()) {
      continue;
    }
    size_t gcBytesRemaining =
        zone->gcHeapThreshold.incrementalBytesRemaining(zone->gcHeapSize);
    minBytesRemaining = std::min(minBytesRemaining, gcBytesRemaining);
    size_t mallocBytesRemaining =
        zone->mallocHeapThreshold.incrementalBytesRemaining(
            zone->mallocHeapSize);
    minBytesRemaining = std::min(minBytesRemaining, mallocBytesRemaining);
  }

  if (minBytesRemaining < tunables.urgentThresholdBytes() &&
      minBytesRemaining != 0) {
    // Increase budget based on the reciprocal of the fraction remaining.
    double fractionRemaining =
        double(minBytesRemaining) / double(tunables.urgentThresholdBytes());
    double minBudget = double(defaultSliceBudgetMS()) / fractionRemaining;
    return ExtendBudget(budget, minBudget);
  }

  return false;
}

static void ScheduleZones(GCRuntime* gc, JS::GCReason reason) {
  for (ZonesIter zone(gc, WithAtoms); !zone.done(); zone.next()) {
    // Re-check heap threshold for alloc-triggered zones that were not
    // previously collected. Now we have allocation rate data, the heap limit
    // may have been increased beyond the current size.
    if (gc->tunables.balancedHeapLimitsEnabled() && zone->isGCScheduled() &&
        zone->smoothedCollectionRate.ref().isNothing() &&
        reason == JS::GCReason::ALLOC_TRIGGER &&
        zone->gcHeapSize.bytes() < zone->gcHeapThreshold.startBytes()) {
      zone->unscheduleGC();  // May still be re-scheduled below.
    }

    if (gc->isShutdownGC()) {
      zone->scheduleGC();
    }

    if (!gc->isPerZoneGCEnabled()) {
      zone->scheduleGC();
    }

    // To avoid resets, continue to collect any zones that were being
    // collected in a previous slice.
    if (gc->isIncrementalGCInProgress() && zone->wasGCStarted()) {
      zone->scheduleGC();
    }

    // This is a heuristic to reduce the total number of collections.
    bool inHighFrequencyMode = gc->schedulingState.inHighFrequencyGCMode();
    if (zone->gcHeapSize.bytes() >=
            zone->gcHeapThreshold.eagerAllocTrigger(inHighFrequencyMode) ||
        zone->mallocHeapSize.bytes() >=
            zone->mallocHeapThreshold.eagerAllocTrigger(inHighFrequencyMode) ||
        zone->jitHeapSize.bytes() >= zone->jitHeapThreshold.startBytes()) {
      zone->scheduleGC();
    }
  }
}

static void UnscheduleZones(GCRuntime* gc) {
  for (ZonesIter zone(gc->rt, WithAtoms); !zone.done(); zone.next()) {
    zone->unscheduleGC();
  }
}

class js::gc::AutoCallGCCallbacks {
  GCRuntime& gc_;
  JS::GCReason reason_;

 public:
  explicit AutoCallGCCallbacks(GCRuntime& gc, JS::GCReason reason)
      : gc_(gc), reason_(reason) {
    gc_.maybeCallGCCallback(JSGC_BEGIN, reason);
  }
  ~AutoCallGCCallbacks() { gc_.maybeCallGCCallback(JSGC_END, reason_); }
};

void GCRuntime::maybeCallGCCallback(JSGCStatus status, JS::GCReason reason) {
  if (!gcCallback.ref().op) {
    return;
  }

  if (isIncrementalGCInProgress()) {
    return;
  }

  if (gcCallbackDepth == 0) {
    // Save scheduled zone information in case the callback clears it.
    for (ZonesIter zone(this, WithAtoms); !zone.done(); zone.next()) {
      zone->gcScheduledSaved_ = zone->gcScheduled_;
    }
  }

  // Save and clear GC options and state in case the callback reenters GC.
  JS::GCOptions options = gcOptions();
  maybeGcOptions = Nothing();
  bool savedFullGCRequested = fullGCRequested;
  fullGCRequested = false;

  gcCallbackDepth++;

  callGCCallback(status, reason);

  MOZ_ASSERT(gcCallbackDepth != 0);
  gcCallbackDepth--;

  // Restore the original GC options.
  maybeGcOptions = Some(options);

  // At the end of a GC, clear out the fullGCRequested state. At the start,
  // restore the previous setting.
  fullGCRequested = (status == JSGC_END) ? false : savedFullGCRequested;

  if (gcCallbackDepth == 0) {
    // Ensure any zone that was originally scheduled stays scheduled.
    for (ZonesIter zone(this, WithAtoms); !zone.done(); zone.next()) {
      zone->gcScheduled_ = zone->gcScheduled_ || zone->gcScheduledSaved_;
    }
  }
}

/*
 * We disable inlining to ensure that the bottom of the stack with possible GC
 * roots recorded in MarkRuntime excludes any pointers we use during the marking
 * implementation.
 */
MOZ_NEVER_INLINE GCRuntime::IncrementalResult GCRuntime::gcCycle(
    bool nonincrementalByAPI, const SliceBudget& budgetArg,
    JS::GCReason reason) {
  // Assert if this is a GC unsafe region.
  rt->mainContextFromOwnThread()->verifyIsSafeToGC();

  // It's ok if threads other than the main thread have suppressGC set, as
  // they are operating on zones which will not be collected from here.
  MOZ_ASSERT(!rt->mainContextFromOwnThread()->suppressGC);

  // This reason is used internally. See below.
  MOZ_ASSERT(reason != JS::GCReason::RESET);

  // Background finalization and decommit are finished by definition before we
  // can start a new major GC.  Background allocation may still be running, but
  // that's OK because chunk pools are protected by the GC lock.
  if (!isIncrementalGCInProgress()) {
    assertBackgroundSweepingFinished();
    MOZ_ASSERT(decommitTask.isIdle());
  }

  // Note that GC callbacks are allowed to re-enter GC.
  AutoCallGCCallbacks callCallbacks(*this, reason);

  // Increase slice budget for long running collections before it is recorded by
  // AutoGCSlice.
  SliceBudget budget(budgetArg);
  bool budgetWasIncreased = maybeIncreaseSliceBudget(budget);

  ScheduleZones(this, reason);

  auto updateCollectorTime = MakeScopeExit([&] {
    if (const gcstats::Statistics::SliceData* slice = stats().lastSlice()) {
      collectorTimeSinceAllocRateUpdate += slice->duration();
    }
  });

  gcstats::AutoGCSlice agc(stats(), scanZonesBeforeGC(), gcOptions(), budget,
                           reason, budgetWasIncreased);

  IncrementalResult result =
      budgetIncrementalGC(nonincrementalByAPI, reason, budget);
  if (result == IncrementalResult::ResetIncremental) {
    if (incrementalState == State::NotActive) {
      // The collection was reset and has finished.
      return result;
    }

    // The collection was reset but we must finish up some remaining work.
    reason = JS::GCReason::RESET;
  }

  majorGCTriggerReason = JS::GCReason::NO_REASON;
  MOZ_ASSERT(!stats().hasTrigger());

  incGcNumber();
  incGcSliceNumber();

  gcprobes::MajorGCStart();
  incrementalSlice(budget, reason, budgetWasIncreased);
  gcprobes::MajorGCEnd();

  MOZ_ASSERT_IF(result == IncrementalResult::ResetIncremental,
                !isIncrementalGCInProgress());
  return result;
}

inline bool GCRuntime::mightSweepInThisSlice(bool nonIncremental) {
  MOZ_ASSERT(incrementalState < State::Sweep);
  return nonIncremental || lastMarkSlice || hasIncrementalTwoSliceZealMode();
}

#ifdef JS_GC_ZEAL
static bool IsDeterministicGCReason(JS::GCReason reason) {
  switch (reason) {
    case JS::GCReason::API:
    case JS::GCReason::DESTROY_RUNTIME:
    case JS::GCReason::LAST_DITCH:
    case JS::GCReason::TOO_MUCH_MALLOC:
    case JS::GCReason::TOO_MUCH_WASM_MEMORY:
    case JS::GCReason::TOO_MUCH_JIT_CODE:
    case JS::GCReason::ALLOC_TRIGGER:
    case JS::GCReason::DEBUG_GC:
    case JS::GCReason::CC_FORCED:
    case JS::GCReason::SHUTDOWN_CC:
    case JS::GCReason::ABORT_GC:
    case JS::GCReason::DISABLE_GENERATIONAL_GC:
    case JS::GCReason::FINISH_GC:
    case JS::GCReason::PREPARE_FOR_TRACING:
      return true;

    default:
      return false;
  }
}
#endif

gcstats::ZoneGCStats GCRuntime::scanZonesBeforeGC() {
  gcstats::ZoneGCStats zoneStats;
  for (ZonesIter zone(this, WithAtoms); !zone.done(); zone.next()) {
    zoneStats.zoneCount++;
    zoneStats.compartmentCount += zone->compartments().length();
    if (zone->isGCScheduled()) {
      zoneStats.collectedZoneCount++;
      zoneStats.collectedCompartmentCount += zone->compartments().length();
    }
  }

  return zoneStats;
}

// The GC can only clean up scheduledForDestruction realms that were marked live
// by a barrier (e.g. by RemapWrappers from a navigation event). It is also
// common to have realms held live because they are part of a cycle in gecko,
// e.g. involving the HTMLDocument wrapper. In this case, we need to run the
// CycleCollector in order to remove these edges before the realm can be freed.
void GCRuntime::maybeDoCycleCollection() {
  const static float ExcessiveGrayRealms = 0.8f;
  const static size_t LimitGrayRealms = 200;

  size_t realmsTotal = 0;
  size_t realmsGray = 0;
  for (RealmsIter realm(rt); !realm.done(); realm.next()) {
    ++realmsTotal;
    GlobalObject* global = realm->unsafeUnbarrieredMaybeGlobal();
    if (global && global->isMarkedGray()) {
      ++realmsGray;
    }
  }
  float grayFraction = float(realmsGray) / float(realmsTotal);
  if (grayFraction > ExcessiveGrayRealms || realmsGray > LimitGrayRealms) {
    callDoCycleCollectionCallback(rt->mainContextFromOwnThread());
  }
}

void GCRuntime::checkCanCallAPI() {
  MOZ_RELEASE_ASSERT(CurrentThreadCanAccessRuntime(rt));

  /* If we attempt to invoke the GC while we are running in the GC, assert. */
  MOZ_RELEASE_ASSERT(!JS::RuntimeHeapIsBusy());
}

bool GCRuntime::checkIfGCAllowedInCurrentState(JS::GCReason reason) {
  if (rt->mainContextFromOwnThread()->suppressGC) {
    return false;
  }

  // Only allow shutdown GCs when we're destroying the runtime. This keeps
  // the GC callback from triggering a nested GC and resetting global state.
  if (rt->isBeingDestroyed() && !isShutdownGC()) {
    return false;
  }

#ifdef JS_GC_ZEAL
  if (deterministicOnly && !IsDeterministicGCReason(reason)) {
    return false;
  }
#endif

  return true;
}

bool GCRuntime::shouldRepeatForDeadZone(JS::GCReason reason) {
  MOZ_ASSERT_IF(reason == JS::GCReason::COMPARTMENT_REVIVED, !isIncremental);
  MOZ_ASSERT(!isIncrementalGCInProgress());

  if (!isIncremental) {
    return false;
  }

  for (CompartmentsIter c(rt); !c.done(); c.next()) {
    if (c->gcState.scheduledForDestruction) {
      return true;
    }
  }

  return false;
}

struct MOZ_RAII AutoSetZoneSliceThresholds {
  explicit AutoSetZoneSliceThresholds(GCRuntime* gc) : gc(gc) {
    // On entry, zones that are already collecting should have a slice threshold
    // set.
    for (ZonesIter zone(gc, WithAtoms); !zone.done(); zone.next()) {
      MOZ_ASSERT(zone->wasGCStarted() ==
                 zone->gcHeapThreshold.hasSliceThreshold());
      MOZ_ASSERT(zone->wasGCStarted() ==
                 zone->mallocHeapThreshold.hasSliceThreshold());
    }
  }

  ~AutoSetZoneSliceThresholds() {
    // On exit, update the thresholds for all collecting zones.
    bool waitingOnBGTask = gc->isWaitingOnBackgroundTask();
    for (ZonesIter zone(gc, WithAtoms); !zone.done(); zone.next()) {
      if (zone->wasGCStarted()) {
        zone->setGCSliceThresholds(*gc, waitingOnBGTask);
      } else {
        MOZ_ASSERT(!zone->gcHeapThreshold.hasSliceThreshold());
        MOZ_ASSERT(!zone->mallocHeapThreshold.hasSliceThreshold());
      }
    }
  }

  GCRuntime* gc;
};

void GCRuntime::collect(bool nonincrementalByAPI, const SliceBudget& budget,
                        JS::GCReason reason) {
  TimeStamp startTime = TimeStamp::Now();
  auto timer = MakeScopeExit([&] {
    if (Realm* realm = rt->mainContextFromOwnThread()->realm()) {
      realm->timers.gcTime += TimeStamp::Now() - startTime;
    }
  });

  auto clearGCOptions = MakeScopeExit([&] {
    if (!isIncrementalGCInProgress()) {
      maybeGcOptions = Nothing();
    }
  });

  MOZ_ASSERT(reason != JS::GCReason::NO_REASON);

  // Checks run for each request, even if we do not actually GC.
  checkCanCallAPI();

  // Check if we are allowed to GC at this time before proceeding.
  if (!checkIfGCAllowedInCurrentState(reason)) {
    return;
  }

  stats().log("GC slice starting in state %s", StateName(incrementalState));

  AutoStopVerifyingBarriers av(rt, isShutdownGC());
  AutoMaybeLeaveAtomsZone leaveAtomsZone(rt->mainContextFromOwnThread());
  AutoSetZoneSliceThresholds sliceThresholds(this);

  schedulingState.updateHighFrequencyModeForReason(reason);

  if (!isIncrementalGCInProgress() && tunables.balancedHeapLimitsEnabled()) {
    updateAllocationRates();
  }

  bool repeat;
  do {
    IncrementalResult cycleResult =
        gcCycle(nonincrementalByAPI, budget, reason);

    if (reason == JS::GCReason::ABORT_GC) {
      MOZ_ASSERT(!isIncrementalGCInProgress());
      stats().log("GC aborted by request");
      break;
    }

    /*
     * Sometimes when we finish a GC we need to immediately start a new one.
     * This happens in the following cases:
     *  - when we reset the current GC
     *  - when finalizers drop roots during shutdown
     *  - when zones that we thought were dead at the start of GC are
     *    not collected (see the large comment in beginMarkPhase)
     */
    repeat = false;
    if (!isIncrementalGCInProgress()) {
      if (cycleResult == ResetIncremental) {
        repeat = true;
      } else if (rootsRemoved && isShutdownGC()) {
        /* Need to re-schedule all zones for GC. */
        JS::PrepareForFullGC(rt->mainContextFromOwnThread());
        repeat = true;
        reason = JS::GCReason::ROOTS_REMOVED;
      } else if (shouldRepeatForDeadZone(reason)) {
        repeat = true;
        reason = JS::GCReason::COMPARTMENT_REVIVED;
      }
    }
  } while (repeat);

  if (reason == JS::GCReason::COMPARTMENT_REVIVED) {
    maybeDoCycleCollection();
  }

#ifdef JS_GC_ZEAL
  if (hasZealMode(ZealMode::CheckHeapAfterGC)) {
    gcstats::AutoPhase ap(stats(), gcstats::PhaseKind::TRACE_HEAP);
    CheckHeapAfterGC(rt);
  }
  if (hasZealMode(ZealMode::CheckGrayMarking) && !isIncrementalGCInProgress()) {
    MOZ_RELEASE_ASSERT(CheckGrayMarkingState(rt));
  }
#endif
  stats().log("GC slice ending in state %s", StateName(incrementalState));

  UnscheduleZones(this);
}

SliceBudget GCRuntime::defaultBudget(JS::GCReason reason, int64_t millis) {
  // millis == 0 means use internal GC scheduling logic to come up with
  // a duration for the slice budget. This may end up still being zero
  // based on preferences.
  if (millis == 0) {
    millis = defaultSliceBudgetMS();
  }

  // If the embedding has registered a callback for creating SliceBudgets,
  // then use it.
  if (createBudgetCallback) {
    return createBudgetCallback(reason, millis);
  }

  // Otherwise, the preference can request an unlimited duration slice.
  if (millis == 0) {
    return SliceBudget::unlimited();
  }

  return SliceBudget(TimeBudget(millis));
}

void GCRuntime::gc(JS::GCOptions options, JS::GCReason reason) {
  if (!isIncrementalGCInProgress()) {
    setGCOptions(options);
  }

  collect(true, SliceBudget::unlimited(), reason);
}

void GCRuntime::startGC(JS::GCOptions options, JS::GCReason reason,
                        const js::SliceBudget& budget) {
  MOZ_ASSERT(!isIncrementalGCInProgress());
  setGCOptions(options);

  if (!JS::IsIncrementalGCEnabled(rt->mainContextFromOwnThread())) {
    collect(true, SliceBudget::unlimited(), reason);
    return;
  }

  collect(false, budget, reason);
}

void GCRuntime::setGCOptions(JS::GCOptions options) {
  MOZ_ASSERT(maybeGcOptions == Nothing());
  maybeGcOptions = Some(options);
}

void GCRuntime::gcSlice(JS::GCReason reason, const js::SliceBudget& budget) {
  MOZ_ASSERT(isIncrementalGCInProgress());
  collect(false, budget, reason);
}

void GCRuntime::finishGC(JS::GCReason reason) {
  MOZ_ASSERT(isIncrementalGCInProgress());

  // If we're not collecting because we're out of memory then skip the
  // compacting phase if we need to finish an ongoing incremental GC
  // non-incrementally to avoid janking the browser.
  if (!IsOOMReason(initialReason)) {
    if (incrementalState == State::Compact) {
      abortGC();
      return;
    }

    isCompacting = false;
  }

  collect(false, SliceBudget::unlimited(), reason);
}

void GCRuntime::abortGC() {
  MOZ_ASSERT(isIncrementalGCInProgress());
  checkCanCallAPI();
  MOZ_ASSERT(!rt->mainContextFromOwnThread()->suppressGC);

  collect(false, SliceBudget::unlimited(), JS::GCReason::ABORT_GC);
}

static bool ZonesSelected(GCRuntime* gc) {
  for (ZonesIter zone(gc, WithAtoms); !zone.done(); zone.next()) {
    if (zone->isGCScheduled()) {
      return true;
    }
  }
  return false;
}

void GCRuntime::startDebugGC(JS::GCOptions options, const SliceBudget& budget) {
  MOZ_ASSERT(!isIncrementalGCInProgress());
  setGCOptions(options);

  if (!ZonesSelected(this)) {
    JS::PrepareForFullGC(rt->mainContextFromOwnThread());
  }

  collect(false, budget, JS::GCReason::DEBUG_GC);
}

void GCRuntime::debugGCSlice(const SliceBudget& budget) {
  MOZ_ASSERT(isIncrementalGCInProgress());

  if (!ZonesSelected(this)) {
    JS::PrepareForIncrementalGC(rt->mainContextFromOwnThread());
  }

  collect(false, budget, JS::GCReason::DEBUG_GC);
}

/* Schedule a full GC unless a zone will already be collected. */
void js::PrepareForDebugGC(JSRuntime* rt) {
  if (!ZonesSelected(&rt->gc)) {
    JS::PrepareForFullGC(rt->mainContextFromOwnThread());
  }
}

void GCRuntime::onOutOfMallocMemory() {
  // Stop allocating new chunks.
  allocTask.cancelAndWait();

  // Make sure we release anything queued for release.
  decommitTask.join();
  nursery().joinDecommitTask();

  // Wait for background free of nursery huge slots to finish.
  sweepTask.join();

  AutoLockGC lock(this);
  onOutOfMallocMemory(lock);
}

void GCRuntime::onOutOfMallocMemory(const AutoLockGC& lock) {
#ifdef DEBUG
  // Release any relocated arenas we may be holding on to, without releasing
  // the GC lock.
  releaseHeldRelocatedArenasWithoutUnlocking(lock);
#endif

  // Throw away any excess chunks we have lying around.
  freeEmptyChunks(lock);

  // Immediately decommit as many arenas as possible in the hopes that this
  // might let the OS scrape together enough pages to satisfy the failing
  // malloc request.
  if (DecommitEnabled()) {
    decommitFreeArenasWithoutUnlocking(lock);
  }
}

void GCRuntime::minorGC(JS::GCReason reason, gcstats::PhaseKind phase) {
  MOZ_ASSERT(!JS::RuntimeHeapIsBusy());

  MOZ_ASSERT_IF(reason == JS::GCReason::EVICT_NURSERY,
                !rt->mainContextFromOwnThread()->suppressGC);
  if (rt->mainContextFromOwnThread()->suppressGC) {
    return;
  }

  incGcNumber();

  collectNursery(JS::GCOptions::Normal, reason, phase);

#ifdef JS_GC_ZEAL
  if (hasZealMode(ZealMode::CheckHeapAfterGC)) {
    gcstats::AutoPhase ap(stats(), phase);
    CheckHeapAfterGC(rt);
  }
#endif

  for (ZonesIter zone(this, WithAtoms); !zone.done(); zone.next()) {
    maybeTriggerGCAfterAlloc(zone);
    maybeTriggerGCAfterMalloc(zone);
  }
}

void GCRuntime::collectNursery(JS::GCOptions options, JS::GCReason reason,
                               gcstats::PhaseKind phase) {
  AutoMaybeLeaveAtomsZone leaveAtomsZone(rt->mainContextFromOwnThread());

  uint32_t numAllocs = 0;
  for (ZonesIter zone(this, WithAtoms); !zone.done(); zone.next()) {
    numAllocs += zone->getAndResetTenuredAllocsSinceMinorGC();
  }
  stats().setAllocsSinceMinorGCTenured(numAllocs);

  gcstats::AutoPhase ap(stats(), phase);

  nursery().clearMinorGCRequest();
  nursery().collect(options, reason);
  MOZ_ASSERT(nursery().isEmpty());

  startBackgroundFreeAfterMinorGC();
}

void GCRuntime::startBackgroundFreeAfterMinorGC() {
  MOZ_ASSERT(nursery().isEmpty());

  {
    AutoLockHelperThreadState lock;

    lifoBlocksToFree.ref().transferFrom(&lifoBlocksToFreeAfterMinorGC.ref());

    if (lifoBlocksToFree.ref().isEmpty() &&
        buffersToFreeAfterMinorGC.ref().empty()) {
      return;
    }
  }

  startBackgroundFree();
}

bool GCRuntime::gcIfRequestedImpl(bool eagerOk) {
  // This method returns whether a major GC was performed.

  if (nursery().minorGCRequested()) {
    minorGC(nursery().minorGCTriggerReason());
  }

  JS::GCReason reason = wantMajorGC(eagerOk);
  if (reason == JS::GCReason::NO_REASON) {
    return false;
  }

  SliceBudget budget = defaultBudget(reason, 0);
  if (!isIncrementalGCInProgress()) {
    startGC(JS::GCOptions::Normal, reason, budget);
  } else {
    gcSlice(reason, budget);
  }
  return true;
}

void js::gc::FinishGC(JSContext* cx, JS::GCReason reason) {
  // Calling this when GC is suppressed won't have any effect.
  MOZ_ASSERT(!cx->suppressGC);

  // GC callbacks may run arbitrary code, including JS. Check this regardless of
  // whether we GC for this invocation.
  MOZ_ASSERT(cx->isNurseryAllocAllowed());

  if (JS::IsIncrementalGCInProgress(cx)) {
    JS::PrepareForIncrementalGC(cx);
    JS::FinishIncrementalGC(cx, reason);
  }
}

void js::gc::WaitForBackgroundTasks(JSContext* cx) {
  cx->runtime()->gc.waitForBackgroundTasks();
}

void GCRuntime::waitForBackgroundTasks() {
  MOZ_ASSERT(!isIncrementalGCInProgress());
  MOZ_ASSERT(sweepTask.isIdle());
  MOZ_ASSERT(decommitTask.isIdle());
  MOZ_ASSERT(markTask.isIdle());

  allocTask.join();
  freeTask.join();
  nursery().joinDecommitTask();
}

Realm* js::NewRealm(JSContext* cx, JSPrincipals* principals,
                    const JS::RealmOptions& options) {
  JSRuntime* rt = cx->runtime();
  JS_AbortIfWrongThread(cx);

  UniquePtr<Zone> zoneHolder;
  UniquePtr<Compartment> compHolder;

  Compartment* comp = nullptr;
  Zone* zone = nullptr;
  JS::CompartmentSpecifier compSpec =
      options.creationOptions().compartmentSpecifier();
  switch (compSpec) {
    case JS::CompartmentSpecifier::NewCompartmentInSystemZone:
      // systemZone might be null here, in which case we'll make a zone and
      // set this field below.
      zone = rt->gc.systemZone;
      break;
    case JS::CompartmentSpecifier::NewCompartmentInExistingZone:
      zone = options.creationOptions().zone();
      MOZ_ASSERT(zone);
      break;
    case JS::CompartmentSpecifier::ExistingCompartment:
      comp = options.creationOptions().compartment();
      zone = comp->zone();
      break;
    case JS::CompartmentSpecifier::NewCompartmentAndZone:
      break;
  }

  if (!zone) {
    Zone::Kind kind = Zone::NormalZone;
    const JSPrincipals* trusted = rt->trustedPrincipals();
    if (compSpec == JS::CompartmentSpecifier::NewCompartmentInSystemZone ||
        (principals && principals == trusted)) {
      kind = Zone::SystemZone;
    }

    zoneHolder = MakeUnique<Zone>(cx->runtime(), kind);
    if (!zoneHolder || !zoneHolder->init()) {
      ReportOutOfMemory(cx);
      return nullptr;
    }

    zone = zoneHolder.get();
  }

  bool invisibleToDebugger = options.creationOptions().invisibleToDebugger();
  if (comp) {
    // Debugger visibility is per-compartment, not per-realm, so make sure the
    // new realm's visibility matches its compartment's.
    MOZ_ASSERT(comp->invisibleToDebugger() == invisibleToDebugger);
  } else {
    compHolder = cx->make_unique<JS::Compartment>(zone, invisibleToDebugger);
    if (!compHolder) {
      return nullptr;
    }

    comp = compHolder.get();
  }

  UniquePtr<Realm> realm(cx->new_<Realm>(comp, options));
  if (!realm) {
    return nullptr;
  }
  realm->init(cx, principals);

  // Make sure we don't put system and non-system realms in the same
  // compartment.
  if (!compHolder) {
    MOZ_RELEASE_ASSERT(realm->isSystem() == IsSystemCompartment(comp));
  }

  AutoLockGC lock(rt);

  // Reserve space in the Vectors before we start mutating them.
  if (!comp->realms().reserve(comp->realms().length() + 1) ||
      (compHolder &&
       !zone->compartments().reserve(zone->compartments().length() + 1)) ||
      (zoneHolder && !rt->gc.zones().reserve(rt->gc.zones().length() + 1))) {
    ReportOutOfMemory(cx);
    return nullptr;
  }

  // After this everything must be infallible.

  comp->realms().infallibleAppend(realm.get());

  if (compHolder) {
    zone->compartments().infallibleAppend(compHolder.release());
  }

  if (zoneHolder) {
    rt->gc.zones().infallibleAppend(zoneHolder.release());

    // Lazily set the runtime's system zone.
    if (compSpec == JS::CompartmentSpecifier::NewCompartmentInSystemZone) {
      MOZ_RELEASE_ASSERT(!rt->gc.systemZone);
      MOZ_ASSERT(zone->isSystemZone());
      rt->gc.systemZone = zone;
    }
  }

  return realm.release();
}

void GCRuntime::runDebugGC() {
#ifdef JS_GC_ZEAL
  if (rt->mainContextFromOwnThread()->suppressGC) {
    return;
  }

  if (hasZealMode(ZealMode::GenerationalGC)) {
    return minorGC(JS::GCReason::DEBUG_GC);
  }

  PrepareForDebugGC(rt);

  auto budget = SliceBudget::unlimited();
  if (hasZealMode(ZealMode::IncrementalMultipleSlices)) {
    /*
     * Start with a small slice limit and double it every slice. This
     * ensure that we get multiple slices, and collection runs to
     * completion.
     */
    if (!isIncrementalGCInProgress()) {
      zealSliceBudget = zealFrequency / 2;
    } else {
      zealSliceBudget *= 2;
    }
    budget = SliceBudget(WorkBudget(zealSliceBudget));

    js::gc::State initialState = incrementalState;
    if (!isIncrementalGCInProgress()) {
      setGCOptions(JS::GCOptions::Shrink);
    }
    collect(false, budget, JS::GCReason::DEBUG_GC);

    /* Reset the slice size when we get to the sweep or compact phases. */
    if ((initialState == State::Mark && incrementalState == State::Sweep) ||
        (initialState == State::Sweep && incrementalState == State::Compact)) {
      zealSliceBudget = zealFrequency / 2;
    }
  } else if (hasIncrementalTwoSliceZealMode()) {
    // These modes trigger incremental GC that happens in two slices and the
    // supplied budget is ignored by incrementalSlice.
    budget = SliceBudget(WorkBudget(1));

    if (!isIncrementalGCInProgress()) {
      setGCOptions(JS::GCOptions::Normal);
    }
    collect(false, budget, JS::GCReason::DEBUG_GC);
  } else if (hasZealMode(ZealMode::Compact)) {
    gc(JS::GCOptions::Shrink, JS::GCReason::DEBUG_GC);
  } else {
    gc(JS::GCOptions::Normal, JS::GCReason::DEBUG_GC);
  }

#endif
}

void GCRuntime::setFullCompartmentChecks(bool enabled) {
  MOZ_ASSERT(!JS::RuntimeHeapIsMajorCollecting());
  fullCompartmentChecks = enabled;
}

void GCRuntime::notifyRootsRemoved() {
  rootsRemoved = true;

#ifdef JS_GC_ZEAL
  /* Schedule a GC to happen "soon". */
  if (hasZealMode(ZealMode::RootsChange)) {
    nextScheduled = 1;
  }
#endif
}

#ifdef JS_GC_ZEAL
bool GCRuntime::selectForMarking(JSObject* object) {
  MOZ_ASSERT(!JS::RuntimeHeapIsMajorCollecting());
  return selectedForMarking.ref().get().append(object);
}

void GCRuntime::clearSelectedForMarking() {
  selectedForMarking.ref().get().clearAndFree();
}

void GCRuntime::setDeterministic(bool enabled) {
  MOZ_ASSERT(!JS::RuntimeHeapIsMajorCollecting());
  deterministicOnly = enabled;
}
#endif

#ifdef DEBUG

AutoAssertNoNurseryAlloc::AutoAssertNoNurseryAlloc() {
  TlsContext.get()->disallowNurseryAlloc();
}

AutoAssertNoNurseryAlloc::~AutoAssertNoNurseryAlloc() {
  TlsContext.get()->allowNurseryAlloc();
}

#endif  // DEBUG

#ifdef JSGC_HASH_TABLE_CHECKS
void GCRuntime::checkHashTablesAfterMovingGC() {
  /*
   * Check that internal hash tables no longer have any pointers to things
   * that have been moved.
   */
  rt->geckoProfiler().checkStringsMapAfterMovingGC();
  if (rt->hasJitRuntime() && rt->jitRuntime()->hasInterpreterEntryMap()) {
    rt->jitRuntime()->getInterpreterEntryMap()->checkScriptsAfterMovingGC();
  }
  for (ZonesIter zone(this, SkipAtoms); !zone.done(); zone.next()) {
    zone->checkUniqueIdTableAfterMovingGC();
    zone->shapeZone().checkTablesAfterMovingGC();
    zone->checkAllCrossCompartmentWrappersAfterMovingGC();
    zone->checkScriptMapsAfterMovingGC();

    // Note: CompactPropMaps never have a table.
    JS::AutoCheckCannotGC nogc;
    for (auto map = zone->cellIterUnsafe<NormalPropMap>(); !map.done();
         map.next()) {
      if (PropMapTable* table = map->asLinked()->maybeTable(nogc)) {
        table->checkAfterMovingGC();
      }
    }
    for (auto map = zone->cellIterUnsafe<DictionaryPropMap>(); !map.done();
         map.next()) {
      if (PropMapTable* table = map->asLinked()->maybeTable(nogc)) {
        table->checkAfterMovingGC();
      }
    }
  }

  for (CompartmentsIter c(this); !c.done(); c.next()) {
    for (RealmsInCompartmentIter r(c); !r.done(); r.next()) {
      r->dtoaCache.checkCacheAfterMovingGC();
      if (r->debugEnvs()) {
        r->debugEnvs()->checkHashTablesAfterMovingGC();
      }
    }
  }
}
#endif

#ifdef DEBUG
bool GCRuntime::hasZone(Zone* target) {
  for (AllZonesIter zone(this); !zone.done(); zone.next()) {
    if (zone == target) {
      return true;
    }
  }
  return false;
}
#endif

void AutoAssertEmptyNursery::checkCondition(JSContext* cx) {
  if (!noAlloc) {
    noAlloc.emplace();
  }
  this->cx = cx;
  MOZ_ASSERT(cx->nursery().isEmpty());
}

AutoEmptyNursery::AutoEmptyNursery(JSContext* cx) : AutoAssertEmptyNursery() {
  MOZ_ASSERT(!cx->suppressGC);
  cx->runtime()->gc.stats().suspendPhases();
  cx->runtime()->gc.evictNursery(JS::GCReason::EVICT_NURSERY);
  cx->runtime()->gc.stats().resumePhases();
  checkCondition(cx);
}

#ifdef DEBUG

namespace js {

// We don't want jsfriendapi.h to depend on GenericPrinter,
// so these functions are declared directly in the cpp.

extern JS_PUBLIC_API void DumpString(JSString* str, js::GenericPrinter& out);

}  // namespace js

void js::gc::Cell::dump(js::GenericPrinter& out) const {
  switch (getTraceKind()) {
    case JS::TraceKind::Object:
      reinterpret_cast<const JSObject*>(this)->dump(out);
      break;

    case JS::TraceKind::String:
      js::DumpString(reinterpret_cast<JSString*>(const_cast<Cell*>(this)), out);
      break;

    case JS::TraceKind::Shape:
      reinterpret_cast<const Shape*>(this)->dump(out);
      break;

    default:
      out.printf("%s(%p)\n", JS::GCTraceKindToAscii(getTraceKind()),
                 (void*)this);
  }
}

// For use in a debugger.
void js::gc::Cell::dump() const {
  js::Fprinter out(stderr);
  dump(out);
}
#endif

JS_PUBLIC_API bool js::gc::detail::CanCheckGrayBits(const TenuredCell* cell) {
  // We do not check the gray marking state of cells in the following cases:
  //
  // 1) When OOM has caused us to clear the gcGrayBitsValid_ flag.
  //
  // 2) When we are in an incremental GC and examine a cell that is in a zone
  // that is not being collected. Gray targets of CCWs that are marked black
  // by a barrier will eventually be marked black in a later GC slice.
  //
  // 3) When mark bits are being cleared concurrently by a helper thread.

  MOZ_ASSERT(cell);

  auto runtime = cell->runtimeFromAnyThread();
  MOZ_ASSERT(CurrentThreadCanAccessRuntime(runtime));

  if (!runtime->gc.areGrayBitsValid()) {
    return false;
  }

  JS::Zone* zone = cell->zone();

  if (runtime->gc.isIncrementalGCInProgress() && !zone->wasGCStarted()) {
    return false;
  }

  return !zone->isGCPreparing();
}

JS_PUBLIC_API bool js::gc::detail::CellIsMarkedGrayIfKnown(
    const TenuredCell* cell) {
  MOZ_ASSERT_IF(cell->isPermanentAndMayBeShared(), cell->isMarkedBlack());
  if (!cell->isMarkedGray()) {
    return false;
  }

  return CanCheckGrayBits(cell);
}

#ifdef DEBUG

JS_PUBLIC_API void js::gc::detail::AssertCellIsNotGray(const Cell* cell) {
  if (!cell->isTenured()) {
    return;
  }

  // Check that a cell is not marked gray.
  //
  // Since this is a debug-only check, take account of the eventual mark state
  // of cells that will be marked black by the next GC slice in an incremental
  // GC. For performance reasons we don't do this in CellIsMarkedGrayIfKnown.

  auto tc = &cell->asTenured();
  if (!tc->isMarkedGray() || !CanCheckGrayBits(tc)) {
    return;
  }

  // TODO: I'd like to AssertHeapIsIdle() here, but this ends up getting
  // called during GC and while iterating the heap for memory reporting.
  MOZ_ASSERT(!JS::RuntimeHeapIsCycleCollecting());

  if (tc->zone()->isGCMarkingBlackAndGray()) {
    // We are doing gray marking in the cell's zone. Even if the cell is
    // currently marked gray it may eventually be marked black. Delay checking
    // non-black cells until we finish gray marking.

    if (!tc->isMarkedBlack()) {
      JSRuntime* rt = tc->zone()->runtimeFromMainThread();
      AutoEnterOOMUnsafeRegion oomUnsafe;
      if (!rt->gc.cellsToAssertNotGray.ref().append(cell)) {
        oomUnsafe.crash("Can't append to delayed gray checks list");
      }
    }
    return;
  }

  MOZ_ASSERT(!tc->isMarkedGray());
}

extern JS_PUBLIC_API bool js::gc::detail::ObjectIsMarkedBlack(
    const JSObject* obj) {
  return obj->isMarkedBlack();
}

#endif

js::gc::ClearEdgesTracer::ClearEdgesTracer(JSRuntime* rt)
    : GenericTracerImpl(rt, JS::TracerKind::ClearEdges,
                        JS::WeakMapTraceAction::TraceKeysAndValues) {}

template <typename T>
void js::gc::ClearEdgesTracer::onEdge(T** thingp, const char* name) {
  // We don't handle removing pointers to nursery edges from the store buffer
  // with this tracer. Check that this doesn't happen.
  T* thing = *thingp;
  MOZ_ASSERT(!IsInsideNursery(thing));

  // Fire the pre-barrier since we're removing an edge from the graph.
  InternalBarrierMethods<T*>::preBarrier(thing);

  *thingp = nullptr;
}

void GCRuntime::setPerformanceHint(PerformanceHint hint) {
  if (hint == PerformanceHint::InPageLoad) {
    inPageLoadCount++;
  } else {
    MOZ_ASSERT(inPageLoadCount);
    inPageLoadCount--;
  }
}
