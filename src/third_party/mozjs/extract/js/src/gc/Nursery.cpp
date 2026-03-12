/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sw=2 et tw=80:
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "gc/Nursery-inl.h"

#include "mozilla/DebugOnly.h"
#include "mozilla/IntegerPrintfMacros.h"
#include "mozilla/Maybe.h"
#include "mozilla/ScopeExit.h"
#include "mozilla/Sprintf.h"
#include "mozilla/TimeStamp.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "builtin/MapObject.h"
#include "debugger/DebugAPI.h"
#include "gc/Allocator.h"
#include "gc/GCInternals.h"
#include "gc/GCLock.h"
#include "gc/GCParallelTask.h"
#include "gc/Memory.h"
#include "gc/PublicIterators.h"
#include "gc/Tenuring.h"
#include "jit/JitFrames.h"
#include "jit/JitZone.h"
#include "js/Printer.h"
#include "util/DifferentialTesting.h"
#include "util/GetPidProvider.h"  // getpid()
#include "util/Poison.h"
#include "vm/JSONPrinter.h"
#include "vm/Realm.h"
#include "vm/Time.h"

#include "gc/BufferAllocator-inl.h"
#include "gc/Heap-inl.h"
#include "gc/Marking-inl.h"
#include "gc/StableCellHasher-inl.h"
#include "gc/StoreBuffer-inl.h"
#include "vm/GeckoProfiler-inl.h"

using namespace js;
using namespace js::gc;

using mozilla::DebugOnly;
using mozilla::PodCopy;
using mozilla::TimeDuration;
using mozilla::TimeStamp;

namespace js {

static constexpr size_t NurseryChunkHeaderSize =
    RoundUp(sizeof(ChunkBase), CellAlignBytes);

// The amount of space in a nursery chunk available to allocations.
static constexpr size_t NurseryChunkUsableSize =
    ChunkSize - NurseryChunkHeaderSize;

struct NurseryChunk : public ChunkBase {
  alignas(CellAlignBytes) uint8_t data[NurseryChunkUsableSize];

  static NurseryChunk* fromChunk(ArenaChunk* chunk, ChunkKind kind,
                                 uint8_t index);

  explicit NurseryChunk(JSRuntime* runtime, ChunkKind kind, uint8_t chunkIndex)
      : ChunkBase(runtime, &runtime->gc.storeBuffer(), kind, chunkIndex) {}

  void poisonRange(size_t start, size_t end, uint8_t value,
                   MemCheckKind checkKind);
  void poisonAfterEvict(size_t extent = ChunkSize);

  // Mark pages from startOffset to the end of the chunk as unused. The start
  // offset must be after the first page, which contains the chunk header and is
  // not marked as unused.
  void markPagesUnusedHard(size_t startOffset);

  // Mark pages from the second page of the chunk to endOffset as in use,
  // following a call to markPagesUnusedHard.
  [[nodiscard]] bool markPagesInUseHard(size_t endOffset);

  uintptr_t start() const { return uintptr_t(&data); }
  uintptr_t end() const { return uintptr_t(this) + ChunkSize; }
};
static_assert(sizeof(NurseryChunk) == ChunkSize,
              "Nursery chunk size must match Chunk size.");
static_assert(offsetof(NurseryChunk, data) == NurseryChunkHeaderSize);

class NurserySweepTask : public GCParallelTask {
  SlimLinkedList<BufferAllocator> allocatorsToSweep;

 public:
  explicit NurserySweepTask(gc::GCRuntime* gc)
      : GCParallelTask(gc, gcstats::PhaseKind::NONE) {}

  bool isEmpty(AutoLockHelperThreadState& lock) const {
    return allocatorsToSweep.isEmpty();
  }

  void queueAllocatorToSweep(BufferAllocator& allocator) {
    MOZ_ASSERT(isIdle());
    allocatorsToSweep.pushBack(&allocator);
  }

 private:
  void run(AutoLockHelperThreadState& lock) override;
};

class NurseryDecommitTask : public GCParallelTask {
 public:
  explicit NurseryDecommitTask(gc::GCRuntime* gc);
  bool reserveSpaceForChunks(size_t nchunks);

  bool isEmpty(const AutoLockHelperThreadState& lock) const;

  void queueChunk(NurseryChunk* chunk, const AutoLockHelperThreadState& lock);
  void queueRange(size_t newCapacity, NurseryChunk* chunk,
                  const AutoLockHelperThreadState& lock);

 private:
  struct Region {
    NurseryChunk* chunk;
    size_t startOffset;
  };

  using NurseryChunkVector = Vector<NurseryChunk*, 0, SystemAllocPolicy>;
  using RegionVector = Vector<Region, 2, SystemAllocPolicy>;

  void run(AutoLockHelperThreadState& lock) override;

  NurseryChunkVector& chunksToDecommit() { return chunksToDecommit_.ref(); }
  const NurseryChunkVector& chunksToDecommit() const {
    return chunksToDecommit_.ref();
  }
  RegionVector& regionsToDecommit() { return regionsToDecommit_.ref(); }
  const RegionVector& regionsToDecommit() const {
    return regionsToDecommit_.ref();
  }

  MainThreadOrGCTaskData<NurseryChunkVector> chunksToDecommit_;
  MainThreadOrGCTaskData<RegionVector> regionsToDecommit_;
};

}  // namespace js

inline void js::NurseryChunk::poisonRange(size_t start, size_t end,
                                          uint8_t value,
                                          MemCheckKind checkKind) {
  MOZ_ASSERT(start >= NurseryChunkHeaderSize);
  MOZ_ASSERT((start % gc::CellAlignBytes) == 0);
  MOZ_ASSERT((end % gc::CellAlignBytes) == 0);
  MOZ_ASSERT(end >= start);
  MOZ_ASSERT(end <= ChunkSize);

  auto* ptr = reinterpret_cast<uint8_t*>(this) + start;
  size_t size = end - start;

  // We can poison the same chunk more than once, so first make sure memory
  // sanitizers will let us poison it.
  MOZ_MAKE_MEM_UNDEFINED(ptr, size);
  Poison(ptr, value, size, checkKind);
}

inline void js::NurseryChunk::poisonAfterEvict(size_t extent) {
  poisonRange(NurseryChunkHeaderSize, extent, JS_SWEPT_NURSERY_PATTERN,
              MemCheckKind::MakeNoAccess);
}

inline void js::NurseryChunk::markPagesUnusedHard(size_t startOffset) {
  MOZ_ASSERT(startOffset >= NurseryChunkHeaderSize);  // Don't touch the header.
  MOZ_ASSERT(startOffset >= SystemPageSize());
  MOZ_ASSERT(startOffset <= ChunkSize);
  uintptr_t start = uintptr_t(this) + startOffset;
  size_t length = ChunkSize - startOffset;
  MarkPagesUnusedHard(reinterpret_cast<void*>(start), length);
}

inline bool js::NurseryChunk::markPagesInUseHard(size_t endOffset) {
  MOZ_ASSERT(endOffset >= NurseryChunkHeaderSize);
  MOZ_ASSERT(endOffset >= SystemPageSize());
  MOZ_ASSERT(endOffset <= ChunkSize);
  uintptr_t start = uintptr_t(this) + SystemPageSize();
  size_t length = endOffset - SystemPageSize();
  return MarkPagesInUseHard(reinterpret_cast<void*>(start), length);
}

// static
inline js::NurseryChunk* js::NurseryChunk::fromChunk(ArenaChunk* chunk,
                                                     ChunkKind kind,
                                                     uint8_t index) {
  return new (chunk) NurseryChunk(chunk->runtime, kind, index);
}

void js::NurserySweepTask::run(AutoLockHelperThreadState& lock) {
  SlimLinkedList<BufferAllocator> allocators;
  std::swap(allocators, allocatorsToSweep);
  AutoUnlockHelperThreadState unlock(lock);

  while (!allocators.isEmpty()) {
    BufferAllocator* allocator = allocators.popFirst();
    allocator->sweepForMinorCollection();
  }
}

js::NurseryDecommitTask::NurseryDecommitTask(gc::GCRuntime* gc)
    : GCParallelTask(gc, gcstats::PhaseKind::NONE) {
  // This can occur outside GCs so doesn't have a stats phase.
  MOZ_ALWAYS_TRUE(regionsToDecommit().reserve(2));
}

bool js::NurseryDecommitTask::isEmpty(
    const AutoLockHelperThreadState& lock) const {
  return chunksToDecommit().empty() && regionsToDecommit().empty();
}

bool js::NurseryDecommitTask::reserveSpaceForChunks(size_t nchunks) {
  MOZ_ASSERT(isIdle());
  return chunksToDecommit().reserve(nchunks);
}

void js::NurseryDecommitTask::queueChunk(
    NurseryChunk* chunk, const AutoLockHelperThreadState& lock) {
  MOZ_ASSERT(isIdle(lock));
  MOZ_ALWAYS_TRUE(chunksToDecommit().append(chunk));
}

void js::NurseryDecommitTask::queueRange(
    size_t newCapacity, NurseryChunk* chunk,
    const AutoLockHelperThreadState& lock) {
  MOZ_ASSERT(isIdle(lock));
  MOZ_ASSERT(regionsToDecommit_.ref().length() < 2);
  MOZ_ASSERT(newCapacity < ChunkSize);
  MOZ_ASSERT(newCapacity % SystemPageSize() == 0);

  regionsToDecommit().infallibleAppend(Region{chunk, newCapacity});
}

void js::NurseryDecommitTask::run(AutoLockHelperThreadState& lock) {
  while (!chunksToDecommit().empty()) {
    NurseryChunk* nurseryChunk = chunksToDecommit().popCopy();
    AutoUnlockHelperThreadState unlock(lock);
    nurseryChunk->~NurseryChunk();
    ArenaChunk* tenuredChunk =
        ArenaChunk::emplace(nurseryChunk, gc, /* allMemoryCommitted = */ false);
    AutoLockGC lock(gc);
    gc->recycleChunk(tenuredChunk, lock);
  }

  while (!regionsToDecommit().empty()) {
    Region region = regionsToDecommit().popCopy();
    AutoUnlockHelperThreadState unlock(lock);
    region.chunk->markPagesUnusedHard(region.startOffset);
  }
}

js::Nursery::Nursery(GCRuntime* gc)
    : toSpace(ChunkKind::NurseryToSpace),
      fromSpace(ChunkKind::NurseryFromSpace),
      gc(gc),
      capacity_(0),
      enableProfiling_(false),
      semispaceEnabled_(gc::TuningDefaults::SemispaceNurseryEnabled),
      canAllocateStrings_(true),
      canAllocateBigInts_(true),
      reportDeduplications_(false),
      minorGCTriggerReason_(JS::GCReason::NO_REASON),
      prevPosition_(0),
      hasRecentGrowthData(false),
      smoothedTargetSize(0.0) {
  // Try to keep fields used by allocation fast path together at the start of
  // the nursery.
  static_assert(offsetof(Nursery, toSpace.position_) < TypicalCacheLineSize);
  static_assert(offsetof(Nursery, toSpace.currentEnd_) < TypicalCacheLineSize);

  const char* env = getenv("MOZ_NURSERY_STRINGS");
  if (env && *env) {
    canAllocateStrings_ = (*env == '1');
  }
  env = getenv("MOZ_NURSERY_BIGINTS");
  if (env && *env) {
    canAllocateBigInts_ = (*env == '1');
  }
}

static void PrintAndExit(const char* message) {
  fprintf(stderr, "%s", message);
  exit(0);
}

static const char* GetEnvVar(const char* name, const char* helpMessage) {
  const char* value = getenv(name);
  if (!value) {
    return nullptr;
  }

  if (strcmp(value, "help") == 0) {
    PrintAndExit(helpMessage);
  }

  return value;
}

static bool GetBoolEnvVar(const char* name, const char* helpMessage) {
  const char* env = GetEnvVar(name, helpMessage);
  return env && bool(atoi(env));
}

static void ReadReportPretenureEnv(const char* name, const char* helpMessage,
                                   AllocSiteFilter* filter) {
  const char* env = GetEnvVar(name, helpMessage);
  if (!env) {
    return;
  }

  if (!AllocSiteFilter::readFromString(env, filter)) {
    PrintAndExit(helpMessage);
  }
}

bool js::Nursery::init(AutoLockGCBgAlloc& lock) {
  ReadProfileEnv("JS_GC_PROFILE_NURSERY",
                 "Report minor GCs taking at least N microseconds.\n",
                 &enableProfiling_, &profileWorkers_, &profileThreshold_);

  reportDeduplications_ = GetBoolEnvVar(
      "JS_GC_REPORT_STATS",
      "JS_GC_REPORT_STATS=1\n"
      "\tAfter a minor GC, report how many strings were deduplicated.\n");

#ifdef JS_GC_ZEAL
  reportPromotion_ = GetBoolEnvVar(
      "JS_GC_REPORT_PROMOTE",
      "JS_GC_REPORT_PROMOTE=1\n"
      "\tAfter a minor GC, report what kinds of things were promoted.\n");
#endif

  ReadReportPretenureEnv(
      "JS_GC_REPORT_PRETENURE",
      "JS_GC_REPORT_PRETENURE=FILTER\n"
      "\tAfter a minor GC, report information about pretenuring, including\n"
      "\tallocation sites which match the filter specification. This is comma\n"
      "\tseparated list of one or more elements which can include:\n"
      "\t\tinteger N:    report sites with at least N allocations\n"
      "\t\t'normal':     report normal sites used for pretenuring\n"
      "\t\t'unknown':    report catch-all sites for allocations without a\n"
      "\t\t              specific site associated with them\n"
      "\t\t'optimized':  report catch-all sites for allocations from\n"
      "\t\t              optimized JIT code\n"
      "\t\t'missing':    report automatically generated missing sites\n"
      "\t\t'object':     report sites associated with JS objects\n"
      "\t\t'string':     report sites associated with JS strings\n"
      "\t\t'bigint':     report sites associated with JS big ints\n"
      "\t\t'longlived':  report sites in the LongLived state (ignored for\n"
      "\t\t              catch-all sites)\n"
      "\t\t'shortlived': report sites in the ShortLived state (ignored for\n"
      "\t\t              catch-all sites)\n"
      "\tFilters of the same kind are combined with OR and of different kinds\n"
      "\twith AND. Prefixes of the keywords above are accepted.\n",
      &pretenuringReportFilter_);

  sweepTask = MakeUnique<NurserySweepTask>(gc);
  if (!sweepTask) {
    return false;
  }

  decommitTask = MakeUnique<NurseryDecommitTask>(gc);
  if (!decommitTask) {
    return false;
  }

  if (!gc->storeBuffer().enable()) {
    return false;
  }

  return initFirstChunk(lock);
}

js::Nursery::~Nursery() { disable(); }

void js::Nursery::enable() {
  MOZ_ASSERT(TlsContext.get()->generationalDisabled == 0);

  if (isEnabled()) {
    return;
  }

  MOZ_ASSERT(isEmpty());
  MOZ_ASSERT(!gc->isVerifyPreBarriersEnabled());

  {
    AutoLockGCBgAlloc lock(gc);
    if (!initFirstChunk(lock)) {
      // If we fail to allocate memory, the nursery will not be enabled.
      return;
    }
  }

#ifdef JS_GC_ZEAL
  if (gc->hasZealMode(ZealMode::GenerationalGC)) {
    enterZealMode();
  }
#endif

  updateAllZoneAllocFlags();

  // This should always succeed after the first time it's called.
  MOZ_ALWAYS_TRUE(gc->storeBuffer().enable());
}

bool js::Nursery::initFirstChunk(AutoLockGCBgAlloc& lock) {
  MOZ_ASSERT(!isEnabled());
  MOZ_ASSERT(toSpace.chunks_.length() == 0);
  MOZ_ASSERT(fromSpace.chunks_.length() == 0);

  setCapacity(minSpaceSize());

  size_t nchunks = toSpace.maxChunkCount_ + fromSpace.maxChunkCount_;
  if (!decommitTask->reserveSpaceForChunks(nchunks) ||
      !allocateNextChunk(lock)) {
    setCapacity(0);
    MOZ_ASSERT(toSpace.isEmpty());
    MOZ_ASSERT(fromSpace.isEmpty());
    return false;
  }

  toSpace.moveToStartOfChunk(this, 0);
  toSpace.setStartToCurrentPosition();

  if (semispaceEnabled_) {
    fromSpace.moveToStartOfChunk(this, 0);
    fromSpace.setStartToCurrentPosition();
  }

  MOZ_ASSERT(toSpace.isEmpty());
  MOZ_ASSERT(fromSpace.isEmpty());

  poisonAndInitCurrentChunk();

  // Clear any information about previous collections.
  clearRecentGrowthData();

  tenureThreshold_ = 0;

#ifdef DEBUG
  toSpace.checkKind(ChunkKind::NurseryToSpace);
  fromSpace.checkKind(ChunkKind::NurseryFromSpace);
#endif

  return true;
}

size_t RequiredChunkCount(size_t nbytes) {
  return nbytes <= ChunkSize ? 1 : nbytes / ChunkSize;
}

void js::Nursery::setCapacity(size_t newCapacity) {
  MOZ_ASSERT(newCapacity == roundSize(newCapacity));
  capacity_ = newCapacity;
  size_t count = RequiredChunkCount(newCapacity);
  toSpace.maxChunkCount_ = count;
  if (semispaceEnabled_) {
    fromSpace.maxChunkCount_ = count;
  }
}

void js::Nursery::disable() {
  MOZ_ASSERT(isEmpty());
  if (!isEnabled()) {
    return;
  }

  // Wait for any background tasks.
  sweepTask->join();
  decommitTask->join();

  // Free all chunks.
  freeChunksFrom(toSpace, 0);
  freeChunksFrom(fromSpace, 0);
  decommitTask->runFromMainThread();

  setCapacity(0);

  // We must reset currentEnd_ so that there is no space for anything in the
  // nursery. JIT'd code uses this even if the nursery is disabled.
  toSpace = Space(ChunkKind::NurseryToSpace);
  fromSpace = Space(ChunkKind::NurseryFromSpace);
  MOZ_ASSERT(toSpace.isEmpty());
  MOZ_ASSERT(fromSpace.isEmpty());

  gc->storeBuffer().disable();

  if (gc->wasInitialized()) {
    // This assumes there is an atoms zone.
    updateAllZoneAllocFlags();
  }
}

void js::Nursery::enableStrings() {
  MOZ_ASSERT(isEmpty());
  canAllocateStrings_ = true;
  updateAllZoneAllocFlags();
}

void js::Nursery::disableStrings() {
  MOZ_ASSERT(isEmpty());
  canAllocateStrings_ = false;
  updateAllZoneAllocFlags();
}

void js::Nursery::enableBigInts() {
  MOZ_ASSERT(isEmpty());
  canAllocateBigInts_ = true;
  updateAllZoneAllocFlags();
}

void js::Nursery::disableBigInts() {
  MOZ_ASSERT(isEmpty());
  canAllocateBigInts_ = false;
  updateAllZoneAllocFlags();
}

void js::Nursery::updateAllZoneAllocFlags() {
  // The alloc flags are not relevant for the atoms zone, and flushing
  // jit-related information can be problematic for the atoms zone.
  for (ZonesIter zone(gc, SkipAtoms); !zone.done(); zone.next()) {
    updateAllocFlagsForZone(zone);
  }
}

void js::Nursery::getAllocFlagsForZone(JS::Zone* zone, bool* allocObjectsOut,
                                       bool* allocStringsOut,
                                       bool* allocBigIntsOut) {
  *allocObjectsOut = isEnabled();
  *allocStringsOut =
      isEnabled() && canAllocateStrings() && !zone->nurseryStringsDisabled;
  *allocBigIntsOut =
      isEnabled() && canAllocateBigInts() && !zone->nurseryBigIntsDisabled;
}

void js::Nursery::setAllocFlagsForZone(JS::Zone* zone) {
  bool allocObjects;
  bool allocStrings;
  bool allocBigInts;

  getAllocFlagsForZone(zone, &allocObjects, &allocStrings, &allocBigInts);
  zone->setNurseryAllocFlags(allocObjects, allocStrings, allocBigInts);
}

void js::Nursery::updateAllocFlagsForZone(JS::Zone* zone) {
  bool allocObjects;
  bool allocStrings;
  bool allocBigInts;

  getAllocFlagsForZone(zone, &allocObjects, &allocStrings, &allocBigInts);

  if (allocObjects != zone->allocNurseryObjects() ||
      allocStrings != zone->allocNurseryStrings() ||
      allocBigInts != zone->allocNurseryBigInts()) {
    CancelOffThreadIonCompile(zone);
    zone->setNurseryAllocFlags(allocObjects, allocStrings, allocBigInts);
    discardCodeAndSetJitFlagsForZone(zone);
  }
}

void js::Nursery::discardCodeAndSetJitFlagsForZone(JS::Zone* zone) {
  zone->forceDiscardJitCode(runtime()->gcContext());

  if (jit::JitZone* jitZone = zone->jitZone()) {
    jitZone->discardStubs();
    jitZone->setStringsCanBeInNursery(zone->allocNurseryStrings());
  }
}

void js::Nursery::setSemispaceEnabled(bool enabled) {
  if (semispaceEnabled() == enabled) {
    return;
  }

  bool wasEnabled = isEnabled();
  if (wasEnabled) {
    if (!isEmpty()) {
      gc->minorGC(JS::GCReason::EVICT_NURSERY);
    }
    disable();
  }

  semispaceEnabled_ = enabled;

  if (wasEnabled) {
    enable();
  }
}

bool js::Nursery::isEmpty() const {
  MOZ_ASSERT(fromSpace.isEmpty());

  if (!isEnabled()) {
    return true;
  }

  if (!gc->hasZealMode(ZealMode::GenerationalGC)) {
    MOZ_ASSERT(startChunk() == 0);
    MOZ_ASSERT(startPosition() == chunk(0).start());
  }

  return toSpace.isEmpty();
}

bool js::Nursery::Space::isEmpty() const { return position_ == startPosition_; }

static size_t AdjustSizeForSemispace(size_t size, bool semispaceEnabled) {
  if (!semispaceEnabled) {
    return size;
  }

  return Nursery::roundSize(size / 2);
}

size_t js::Nursery::maxSpaceSize() const {
  return AdjustSizeForSemispace(tunables().gcMaxNurseryBytes(),
                                semispaceEnabled_);
}

size_t js::Nursery::minSpaceSize() const {
  return AdjustSizeForSemispace(tunables().gcMinNurseryBytes(),
                                semispaceEnabled_);
}

#ifdef JS_GC_ZEAL
void js::Nursery::enterZealMode() {
  if (!isEnabled()) {
    return;
  }

  MOZ_ASSERT(isEmpty());

  decommitTask->join();

  AutoEnterOOMUnsafeRegion oomUnsafe;

  if (isSubChunkMode()) {
    {
      if (!chunk(0).markPagesInUseHard(ChunkSize)) {
        oomUnsafe.crash("Out of memory trying to extend chunk for zeal mode");
      }
    }

    // It'd be simpler to poison the whole chunk, but we can't do that
    // because the nursery might be partially used.
    chunk(0).poisonRange(capacity_, ChunkSize, JS_FRESH_NURSERY_PATTERN,
                         MemCheckKind::MakeUndefined);
  }

  setCapacity(maxSpaceSize());

  size_t nchunks = toSpace.maxChunkCount_ + fromSpace.maxChunkCount_;
  if (!decommitTask->reserveSpaceForChunks(nchunks)) {
    oomUnsafe.crash("Nursery::enterZealMode");
  }

  setCurrentEnd();
}

void js::Nursery::leaveZealMode() {
  if (!isEnabled()) {
    return;
  }

  MOZ_ASSERT(isEmpty());

  // Reset the nursery size.
  setCapacity(minSpaceSize());

  toSpace.moveToStartOfChunk(this, 0);
  toSpace.setStartToCurrentPosition();

  if (semispaceEnabled_) {
    fromSpace.moveToStartOfChunk(this, 0);
    fromSpace.setStartToCurrentPosition();
  }

  poisonAndInitCurrentChunk();
}
#endif  // JS_GC_ZEAL

void* js::Nursery::allocateCell(gc::AllocSite* site, size_t size,
                                JS::TraceKind kind) {
  MOZ_ASSERT(CurrentThreadCanAccessRuntime(runtime()));

  void* ptr = tryAllocateCell(site, size, kind);
  if (MOZ_LIKELY(ptr)) {
    return ptr;
  }

  if (handleAllocationFailure() != JS::GCReason::NO_REASON) {
    return nullptr;
  }

  ptr = tryAllocateCell(site, size, kind);
  MOZ_ASSERT(ptr);
  return ptr;
}

inline void* js::Nursery::allocate(size_t size) {
  MOZ_ASSERT(CurrentThreadCanAccessRuntime(runtime()));

  void* ptr = tryAllocate(size);
  if (MOZ_LIKELY(ptr)) {
    return ptr;
  }

  if (handleAllocationFailure() != JS::GCReason::NO_REASON) {
    return nullptr;
  }

  ptr = tryAllocate(size);
  MOZ_ASSERT(ptr);
  return ptr;
}

MOZ_NEVER_INLINE JS::GCReason Nursery::handleAllocationFailure() {
  if (minorGCRequested()) {
    // If a minor GC was requested then fail the allocation. The collection is
    // then run in GCRuntime::tryNewNurseryCell.
    return minorGCTriggerReason_;
  }

  if (!moveToNextChunk()) {
    return JS::GCReason::OUT_OF_NURSERY;
  }

  return JS::GCReason::NO_REASON;
}

bool Nursery::moveToNextChunk() {
  unsigned chunkno = currentChunk() + 1;
  MOZ_ASSERT(chunkno <= maxChunkCount());
  MOZ_ASSERT(chunkno <= allocatedChunkCount());
  if (chunkno == maxChunkCount()) {
    return false;
  }

  if (chunkno == allocatedChunkCount()) {
    TimeStamp start = TimeStamp::Now();
    {
      AutoLockGCBgAlloc lock(gc);
      if (!allocateNextChunk(lock)) {
        return false;
      }
    }
    timeInChunkAlloc_ += TimeStamp::Now() - start;
    MOZ_ASSERT(chunkno < allocatedChunkCount());
  }

  moveToStartOfChunk(chunkno);
  poisonAndInitCurrentChunk();
  return true;
}

std::tuple<void*, bool> js::Nursery::allocNurseryOrMallocBuffer(
    Zone* zone, size_t nbytes, arena_id_t arenaId) {
  MOZ_ASSERT(nbytes > 0);
  MOZ_ASSERT(nbytes <= SIZE_MAX - gc::CellAlignBytes);
  nbytes = RoundUp(nbytes, gc::CellAlignBytes);

  if (nbytes <= MaxNurseryBufferSize) {
    void* buffer = allocate(nbytes);
    if (buffer) {
      return {buffer, false};
    }
  }

  void* buffer = zone->pod_arena_malloc<uint8_t>(arenaId, nbytes);
  return {buffer, bool(buffer)};
}

std::tuple<void*, bool> js::Nursery::allocateBuffer(Zone* zone, size_t nbytes) {
  MOZ_ASSERT(nbytes > 0);
  MOZ_ASSERT(nbytes <= SIZE_MAX - gc::CellAlignBytes);
  nbytes = RoundUp(nbytes, gc::CellAlignBytes);

  if (nbytes <= MaxNurseryBufferSize) {
    void* buffer = allocate(nbytes);
    if (buffer) {
      return {buffer, false};
    }
  }

  void* buffer = AllocBuffer(zone, nbytes, true);
  return {buffer, bool(buffer)};
}

void* js::Nursery::tryAllocateNurseryBuffer(JS::Zone* zone, size_t nbytes,
                                            arena_id_t arenaId) {
  MOZ_ASSERT(nbytes > 0);
  MOZ_ASSERT(nbytes <= SIZE_MAX - gc::CellAlignBytes);
  nbytes = RoundUp(nbytes, gc::CellAlignBytes);

  if (nbytes <= MaxNurseryBufferSize) {
    return allocate(nbytes);
  }

  return nullptr;
}

void* js::Nursery::allocNurseryOrMallocBuffer(Zone* zone, Cell* owner,
                                              size_t nbytes,
                                              arena_id_t arenaId) {
  MOZ_ASSERT(owner);
  MOZ_ASSERT(nbytes > 0);

  if (!IsInsideNursery(owner)) {
    return zone->pod_arena_malloc<uint8_t>(arenaId, nbytes);
  }

  auto [buffer, isMalloced] = allocNurseryOrMallocBuffer(zone, nbytes, arenaId);
  if (isMalloced && !registerMallocedBuffer(buffer, nbytes)) {
    js_free(buffer);
    return nullptr;
  }
  return buffer;
}

void* js::Nursery::allocateBuffer(Zone* zone, Cell* owner, size_t nbytes) {
  MOZ_ASSERT(owner);
  MOZ_ASSERT(zone == owner->zone());
  MOZ_ASSERT(nbytes > 0);

  if (!IsInsideNursery(owner)) {
    return AllocBuffer(zone, nbytes, false);
  }

  auto [buffer, isExternal] = allocateBuffer(zone, nbytes);
  if (isExternal) {
    registerBuffer(buffer, nbytes);
  }
  return buffer;
}

std::tuple<void*, bool> js::Nursery::allocateZeroedBuffer(Zone* zone,
                                                          size_t nbytes,
                                                          arena_id_t arena) {
  MOZ_ASSERT(nbytes > 0);

  if (nbytes <= MaxNurseryBufferSize) {
    void* buffer = allocate(nbytes);
    if (buffer) {
      memset(buffer, 0, nbytes);
      return {buffer, false};
    }
  }

  void* buffer = zone->pod_arena_calloc<uint8_t>(arena, nbytes);
  return {buffer, bool(buffer)};
}

void* js::Nursery::allocateZeroedBuffer(Cell* owner, size_t nbytes,
                                        arena_id_t arena) {
  MOZ_ASSERT(owner);
  MOZ_ASSERT(nbytes > 0);

  if (!IsInsideNursery(owner)) {
    return owner->asTenured().zone()->pod_arena_calloc<uint8_t>(arena, nbytes);
  }
  auto [buffer, isMalloced] =
      allocateZeroedBuffer(owner->nurseryZone(), nbytes, arena);
  if (isMalloced && !registerMallocedBuffer(buffer, nbytes)) {
    js_free(buffer);
    return nullptr;
  }
  return buffer;
}

void* js::Nursery::reallocNurseryOrMallocBuffer(Zone* zone, Cell* cell,
                                                void* oldBuffer,
                                                size_t oldBytes,
                                                size_t newBytes,
                                                arena_id_t arena) {
  if (!IsInsideNursery(cell)) {
    MOZ_ASSERT(!isInside(oldBuffer));
    return zone->pod_realloc<uint8_t>((uint8_t*)oldBuffer, oldBytes, newBytes);
  }

  if (!isInside(oldBuffer)) {
    MOZ_ASSERT(toSpace.mallocedBufferBytes >= oldBytes);
    void* newBuffer =
        zone->pod_realloc<uint8_t>((uint8_t*)oldBuffer, oldBytes, newBytes);
    if (newBuffer) {
      if (oldBuffer != newBuffer) {
        MOZ_ALWAYS_TRUE(
            toSpace.mallocedBuffers.rekeyAs(oldBuffer, newBuffer, newBuffer));
      }
      toSpace.mallocedBufferBytes -= oldBytes;
      toSpace.mallocedBufferBytes += newBytes;
    }
    return newBuffer;
  }

  // The nursery cannot make use of the returned slots data.
  if (newBytes < oldBytes) {
    return oldBuffer;
  }

  auto newBuffer =
      allocNurseryOrMallocBuffer(zone, cell, newBytes, js::MallocArena);
  if (newBuffer) {
    PodCopy((uint8_t*)newBuffer, (uint8_t*)oldBuffer, oldBytes);
  }
  return newBuffer;
}

void* js::Nursery::reallocateBuffer(Zone* zone, Cell* cell, void* oldBuffer,
                                    size_t oldBytes, size_t newBytes) {
  if (!IsInsideNursery(cell)) {
    MOZ_ASSERT(IsBufferAlloc(oldBuffer));
    MOZ_ASSERT(!IsNurseryOwned(zone, oldBuffer));
    return ReallocBuffer(zone, oldBuffer, newBytes, false);
  }

  if (IsBufferAlloc(oldBuffer)) {
    MOZ_ASSERT(IsNurseryOwned(zone, oldBuffer));
    MOZ_ASSERT(toSpace.mallocedBufferBytes >= oldBytes);

    void* newBuffer = ReallocBuffer(zone, oldBuffer, newBytes, true);
    if (!newBuffer) {
      return nullptr;
    }

    toSpace.mallocedBufferBytes -= oldBytes;
    toSpace.mallocedBufferBytes += newBytes;
    return newBuffer;
  }

  // The nursery cannot make use of the returned slots data.
  if (newBytes < oldBytes) {
    return oldBuffer;
  }

  auto newBuffer = allocateBuffer(zone, cell, newBytes);
  if (newBuffer) {
    PodCopy((uint8_t*)newBuffer, (uint8_t*)oldBuffer, oldBytes);
  }
  return newBuffer;
}

void js::Nursery::freeBuffer(void* buffer, size_t nbytes) {
  if (!isInside(buffer)) {
    removeMallocedBuffer(buffer, nbytes);
    js_free(buffer);
  }
}

#ifdef DEBUG
/* static */
inline bool Nursery::checkForwardingPointerInsideNursery(void* ptr) {
  // If a zero-capacity elements header lands right at the end of a chunk then
  // elements data will appear to be in the next chunk. If we have a pointer to
  // the very start of a chunk, check the previous chunk.
  if ((uintptr_t(ptr) & ChunkMask) == 0) {
    return isInside(reinterpret_cast<uint8_t*>(ptr) - 1);
  }

  return isInside(ptr);
}
#endif

void Nursery::setIndirectForwardingPointer(void* oldData, void* newData) {
  MOZ_ASSERT(checkForwardingPointerInsideNursery(oldData));
  // |newData| may be either in the nursery or in the malloc heap.

  AutoEnterOOMUnsafeRegion oomUnsafe;
#ifdef DEBUG
  if (ForwardedBufferMap::Ptr p = forwardedBuffers.lookup(oldData)) {
    MOZ_ASSERT(p->value() == newData);
  }
#endif
  if (!forwardedBuffers.put(oldData, newData)) {
    oomUnsafe.crash("Nursery::setForwardingPointer");
  }
}

#ifdef DEBUG
static bool IsWriteableAddress(void* ptr) {
  auto* vPtr = reinterpret_cast<volatile uint64_t*>(ptr);
  *vPtr = *vPtr;
  return true;
}
#endif

void js::Nursery::forwardBufferPointer(uintptr_t* pSlotsElems) {
  // Read the current pointer value which may be one of:
  //  - Non-nursery pointer
  //  - Nursery-allocated buffer
  //  - A BufferRelocationOverlay inside the nursery
  //
  // Note: The buffer has already be relocated. We are just patching stale
  //       pointers now.
  auto* buffer = reinterpret_cast<void*>(*pSlotsElems);

  if (!isInside(buffer)) {
    return;
  }

  // The new location for this buffer is either stored inline with it or in
  // the forwardedBuffers table.
  if (ForwardedBufferMap::Ptr p = forwardedBuffers.lookup(buffer)) {
    buffer = p->value();
    // It's not valid to assert IsWriteableAddress for indirect forwarding
    // pointers because the size of the allocation could be less than a word.
  } else {
    BufferRelocationOverlay* reloc =
        static_cast<BufferRelocationOverlay*>(buffer);
    buffer = *reloc;
    MOZ_ASSERT(IsWriteableAddress(buffer));
  }

  MOZ_ASSERT_IF(isInside(buffer), !inCollectedRegion(buffer));
  *pSlotsElems = reinterpret_cast<uintptr_t>(buffer);
}

inline double js::Nursery::calcPromotionRate(bool* validForTenuring) const {
  MOZ_ASSERT(validForTenuring);

  if (previousGC.nurseryUsedBytes == 0) {
    *validForTenuring = false;
    return 0.0;
  }

  double used = double(previousGC.nurseryUsedBytes);
  double capacity = double(previousGC.nurseryCapacity);
  double tenured = double(previousGC.tenuredBytes);

  // We should only use the promotion rate to make tenuring decisions if it's
  // likely to be valid. The criterion we use is that the nursery was at least
  // 90% full.
  *validForTenuring = used > capacity * 0.9;

  MOZ_ASSERT(tenured <= used);
  return tenured / used;
}

void js::Nursery::renderProfileJSON(JSONPrinter& json) const {
  if (!isEnabled()) {
    json.beginObject();
    json.property("status", "nursery disabled");
    json.endObject();
    return;
  }

  if (previousGC.reason == JS::GCReason::NO_REASON) {
    // If the nursery was empty when the last minorGC was requested, then
    // no nursery collection will have been performed but JSON may still be
    // requested. (And as a public API, this function should not crash in
    // such a case.)
    json.beginObject();
    json.property("status", "nursery empty");
    json.endObject();
    return;
  }

  // The profiler data uses the term 'tenured' for compatibility with the
  // existing data format, although 'promoted' would be more accurate given
  // support for semispace nursery.

  json.beginObject();

  json.property("status", "complete");

  json.property("reason", JS::ExplainGCReason(previousGC.reason));
  json.property("bytes_tenured", previousGC.tenuredBytes);
  json.property("cells_tenured", previousGC.tenuredCells);
  json.property("strings_tenured",
                stats().getStat(gcstats::STAT_STRINGS_PROMOTED));
  json.property("strings_deduplicated",
                stats().getStat(gcstats::STAT_STRINGS_DEDUPLICATED));
  json.property("bigints_tenured",
                stats().getStat(gcstats::STAT_BIGINTS_PROMOTED));
  json.property("bytes_used", previousGC.nurseryUsedBytes);
  json.property("cur_capacity", previousGC.nurseryCapacity);
  const size_t newCapacity = capacity();
  if (newCapacity != previousGC.nurseryCapacity) {
    json.property("new_capacity", newCapacity);
  }
  if (previousGC.nurseryCommitted != previousGC.nurseryCapacity) {
    json.property("lazy_capacity", previousGC.nurseryCommitted);
  }
  if (!timeInChunkAlloc_.IsZero()) {
    json.property("chunk_alloc_us", timeInChunkAlloc_, json.MICROSECONDS);
  }

  // This calculation includes the whole collection time, not just the time
  // spent promoting.
  double totalTime = profileDurations_[ProfileKey::Total].ToSeconds();
  if (totalTime > 0.0) {
    double tenuredAllocRate = double(previousGC.tenuredBytes) / totalTime;
    json.property("tenured_allocation_rate", size_t(tenuredAllocRate));
  }

  // These counters only contain consistent data if the profiler is enabled,
  // and then there's no guarentee.
  if (runtime()->geckoProfiler().enabled()) {
    json.property("cells_allocated_nursery",
                  pretenuringNursery.totalAllocCount());
    json.property("cells_allocated_tenured",
                  stats().allocsSinceMinorGCTenured());
  }

  json.beginObjectProperty("phase_times");

#define EXTRACT_NAME(name, text) #name,
  static const char* const names[] = {
      FOR_EACH_NURSERY_PROFILE_TIME(EXTRACT_NAME)
#undef EXTRACT_NAME
          ""};

  size_t i = 0;
  for (auto time : profileDurations_) {
    json.property(names[i++], time, json.MICROSECONDS);
  }

  json.endObject();  // timings value

  json.endObject();
}

// The following macros define nursery GC profile metadata fields that are
// printed before the timing information defined by
// FOR_EACH_NURSERY_PROFILE_TIME.

#define FOR_EACH_NURSERY_PROFILE_COMMON_METADATA(_) \
  _("PID", 7, "%7zu", pid)                          \
  _("Runtime", 14, "0x%12p", runtime)

#define FOR_EACH_NURSERY_PROFILE_SLICE_METADATA(_)    \
  _("Timestamp", 10, "%10.6f", timestamp.ToSeconds()) \
  _("Reason", 20, "%-20.20s", reasonStr)              \
  _("PRate", 6, "%5.1f%%", promotionRatePercent)      \
  _("OldKB", 6, "%6zu", oldSizeKB)                    \
  _("NewKB", 6, "%6zu", newSizeKB)                    \
  _("Dedup", 6, "%6zu", dedupCount)

#define FOR_EACH_NURSERY_PROFILE_METADATA(_)  \
  FOR_EACH_NURSERY_PROFILE_COMMON_METADATA(_) \
  FOR_EACH_NURSERY_PROFILE_SLICE_METADATA(_)

void js::Nursery::printCollectionProfile(JS::GCReason reason,
                                         double promotionRate) {
  stats().maybePrintProfileHeaders();

  Sprinter sprinter;
  if (!sprinter.init()) {
    return;
  }
  sprinter.put(gcstats::MinorGCProfilePrefix);

  size_t pid = getpid();
  JSRuntime* runtime = gc->rt;
  TimeDuration timestamp = collectionStartTime() - stats().creationTime();
  const char* reasonStr = ExplainGCReason(reason);
  double promotionRatePercent = promotionRate * 100;
  size_t oldSizeKB = previousGC.nurseryCapacity / 1024;
  size_t newSizeKB = capacity() / 1024;
  size_t dedupCount = stats().getStat(gcstats::STAT_STRINGS_DEDUPLICATED);

#define PRINT_FIELD_VALUE(_1, _2, format, value) \
  sprinter.printf(" " format, value);

  FOR_EACH_NURSERY_PROFILE_METADATA(PRINT_FIELD_VALUE)
#undef PRINT_FIELD_VALUE

  printProfileDurations(profileDurations_, sprinter);

  JS::UniqueChars str = sprinter.release();
  if (!str) {
    return;
  }
  fputs(str.get(), stats().profileFile());
}

void js::Nursery::printProfileHeader() {
  Sprinter sprinter;
  if (!sprinter.init()) {
    return;
  }
  sprinter.put(gcstats::MinorGCProfilePrefix);

#define PRINT_FIELD_NAME(name, width, _1, _2) \
  sprinter.printf(" %-*s", width, name);

  FOR_EACH_NURSERY_PROFILE_METADATA(PRINT_FIELD_NAME)
#undef PRINT_FIELD_NAME

#define PRINT_PROFILE_NAME(_1, text) sprinter.printf(" %-6.6s", text);

  FOR_EACH_NURSERY_PROFILE_TIME(PRINT_PROFILE_NAME)
#undef PRINT_PROFILE_NAME

  sprinter.put("\n");

  JS::UniqueChars str = sprinter.release();
  if (!str) {
    return;
  }
  fputs(str.get(), stats().profileFile());
}

// static
void js::Nursery::printProfileDurations(const ProfileDurations& times,
                                        Sprinter& sprinter) {
  for (auto time : times) {
    int64_t micros = int64_t(time.ToMicroseconds());
    sprinter.printf(" %6" PRIi64, micros);
  }

  sprinter.put("\n");
}

static constexpr size_t NurserySliceMetadataFormatWidth() {
  size_t fieldCount = 0;
  size_t totalWidth = 0;

#define UPDATE_COUNT_AND_WIDTH(_1, width, _2, _3) \
  fieldCount++;                                   \
  totalWidth += width;
  FOR_EACH_NURSERY_PROFILE_SLICE_METADATA(UPDATE_COUNT_AND_WIDTH)
#undef UPDATE_COUNT_AND_WIDTH

  // Add padding between fields.
  totalWidth += fieldCount - 1;

  return totalWidth;
}

void js::Nursery::printTotalProfileTimes() {
  if (!enableProfiling_) {
    return;
  }

  Sprinter sprinter;
  if (!sprinter.init()) {
    return;
  }
  sprinter.put(gcstats::MinorGCProfilePrefix);

  size_t pid = getpid();
  JSRuntime* runtime = gc->rt;

  char collections[32];
  DebugOnly<int> r = SprintfLiteral(
      collections, "TOTALS: %7" PRIu64 " collections:", gc->minorGCCount());
  MOZ_ASSERT(r > 0 && r < int(sizeof(collections)));

#define PRINT_FIELD_VALUE(_1, _2, format, value) \
  sprinter.printf(" " format, value);

  FOR_EACH_NURSERY_PROFILE_COMMON_METADATA(PRINT_FIELD_VALUE)
#undef PRINT_FIELD_VALUE

  // Use whole width of per-slice metadata to print total slices so the profile
  // totals that follow line up.
  size_t width = NurserySliceMetadataFormatWidth();
  sprinter.printf(" %-*s", int(width), collections);

  printProfileDurations(totalDurations_, sprinter);

  JS::UniqueChars str = sprinter.release();
  if (!str) {
    return;
  }
  fputs(str.get(), stats().profileFile());
}

void js::Nursery::maybeClearProfileDurations() {
  for (auto& duration : profileDurations_) {
    duration = mozilla::TimeDuration::Zero();
  }
}

inline void js::Nursery::startProfile(ProfileKey key) {
  startTimes_[key] = TimeStamp::Now();
}

inline void js::Nursery::endProfile(ProfileKey key) {
  profileDurations_[key] = TimeStamp::Now() - startTimes_[key];
  totalDurations_[key] += profileDurations_[key];
}

inline TimeStamp js::Nursery::collectionStartTime() const {
  return startTimes_[ProfileKey::Total];
}

TimeStamp js::Nursery::lastCollectionEndTime() const {
  return previousGC.endTime;
}

bool js::Nursery::wantEagerCollection() const {
  if (!isEnabled()) {
    return false;
  }

  if (isEmpty() && capacity() == minSpaceSize()) {
    return false;
  }

  if (minorGCRequested()) {
    return true;
  }

  if (freeSpaceIsBelowEagerThreshold()) {
    return true;
  }

  // If the nursery is not being collected often then it may be taking up more
  // space than necessary.
  return isUnderused();
}

inline bool js::Nursery::freeSpaceIsBelowEagerThreshold() const {
  // The threshold is specified in terms of free space so that it doesn't depend
  // on the size of the nursery.
  //
  // There two thresholds, an absolute free bytes threshold and a free space
  // fraction threshold. Two thresholds are used so that we don't collect too
  // eagerly for small nurseries (or even all the time if nursery size is less
  // than the free bytes threshold) or too eagerly for large nurseries (where a
  // fractional threshold may leave a significant amount of nursery unused).
  //
  // Since the aim is making this less eager we require both thresholds to be
  // met.

  size_t freeBytes = freeSpace();
  double freeFraction = double(freeBytes) / double(capacity());

  size_t bytesThreshold = tunables().nurseryEagerCollectionThresholdBytes();
  double fractionThreshold =
      tunables().nurseryEagerCollectionThresholdPercent();

  return freeBytes < bytesThreshold && freeFraction < fractionThreshold;
}

inline bool js::Nursery::isUnderused() const {
  if (js::SupportDifferentialTesting() || !previousGC.endTime) {
    return false;
  }

  if (capacity() == minSpaceSize()) {
    return false;
  }

  // If the nursery is above its minimum size, collect it every so often if we
  // have idle time. This allows the nursery to shrink when it's not being
  // used. There are other heuristics we could use for this, but this is the
  // simplest.
  TimeDuration timeSinceLastCollection =
      TimeStamp::NowLoRes() - previousGC.endTime;
  return timeSinceLastCollection > tunables().nurseryEagerCollectionTimeout();
}

void js::Nursery::collect(JS::GCOptions options, JS::GCReason reason) {
  JSRuntime* rt = runtime();
  MOZ_ASSERT(!rt->mainContextFromOwnThread()->suppressGC);

  if (minorGCRequested()) {
    MOZ_ASSERT(position() == chunk(currentChunk()).end());
    toSpace.position_ = prevPosition_;
    prevPosition_ = 0;
    minorGCTriggerReason_ = JS::GCReason::NO_REASON;
    rt->mainContextFromOwnThread()->clearPendingInterrupt(
        InterruptReason::MinorGC);
  }

  if (!isEnabled() || isEmpty()) {
    // Our barriers are not always exact, and there may be entries in the
    // storebuffer even when the nursery is disabled or empty. It's not safe
    // to keep these entries as they may refer to tenured cells which may be
    // freed after this point.
    gc->storeBuffer().clear();

    MOZ_ASSERT_IF(!semispaceEnabled_, !pretenuringNursery.hasAllocatedSites());
  }

  if (!isEnabled()) {
    return;
  }

  AutoGCSession session(gc, JS::HeapState::MinorCollecting);

  stats().beginNurseryCollection();
  gcprobes::MinorGCStart();

  if (stats().bufferAllocStatsEnabled() && runtime()->isMainRuntime()) {
    stats().maybePrintProfileHeaders();
    BufferAllocator::printStats(gc, gc->stats().creationTime(), false,
                                gc->stats().profileFile());
  }

  gc->callNurseryCollectionCallbacks(
      JS::GCNurseryProgress::GC_NURSERY_COLLECTION_START, reason);

  maybeClearProfileDurations();
  startProfile(ProfileKey::Total);

  previousGC.reason = JS::GCReason::NO_REASON;
  previousGC.nurseryUsedBytes = usedSpace();
  previousGC.nurseryCapacity = capacity();
  previousGC.nurseryCommitted = totalCommitted();
  previousGC.nurseryUsedChunkCount = currentChunk() + 1;
  previousGC.tenuredBytes = 0;
  previousGC.tenuredCells = 0;
  tenuredEverything = true;

  // Wait for any previous buffer sweeping to finish. This happens even if the
  // nursery is empty because we track whether this has happened by checking the
  // minor GC number, which is incremented regardless. See the call to
  // joinSweepTask in GCRuntime::endSweepingSweepGroup.
  joinSweepTask();

  // If it isn't empty, it will call doCollection, and possibly after that
  // isEmpty() will become true, so use another variable to keep track of the
  // old empty state.
  bool wasEmpty = isEmpty();
  if (!wasEmpty) {
    CollectionResult result = doCollection(session, options, reason);
    // Don't include chunk headers when calculating nursery space, since this
    // space does not represent data that can be tenured
    MOZ_ASSERT(result.tenuredBytes <=
               (previousGC.nurseryUsedBytes -
                (NurseryChunkHeaderSize * previousGC.nurseryUsedChunkCount)));

    previousGC.reason = reason;
    previousGC.tenuredBytes = result.tenuredBytes;
    previousGC.tenuredCells = result.tenuredCells;
    previousGC.nurseryUsedChunkCount = currentChunk() + 1;
  }

  // Resize the nursery.
  maybeResizeNursery(options, reason);

  if (!semispaceEnabled()) {
    poisonAndInitCurrentChunk();
  }

  bool validPromotionRate;
  const double promotionRate = calcPromotionRate(&validPromotionRate);

  startProfile(ProfileKey::Pretenure);
  size_t sitesPretenured = 0;
  sitesPretenured =
      doPretenuring(rt, reason, validPromotionRate, promotionRate);
  endProfile(ProfileKey::Pretenure);

  previousGC.endTime =
      TimeStamp::Now();  // Must happen after maybeResizeNursery.
  endProfile(ProfileKey::Total);
  gc->incMinorGcNumber();

  TimeDuration totalTime = profileDurations_[ProfileKey::Total];
  sendTelemetry(reason, totalTime, wasEmpty, promotionRate, sitesPretenured);

  gc->callNurseryCollectionCallbacks(
      JS::GCNurseryProgress::GC_NURSERY_COLLECTION_END, reason);

  stats().endNurseryCollection();
  gcprobes::MinorGCEnd();

  timeInChunkAlloc_ = mozilla::TimeDuration::Zero();

  js::StringStats prevStats = gc->stringStats;
  js::StringStats& currStats = gc->stringStats;
  currStats = js::StringStats();
  for (ZonesIter zone(gc, WithAtoms); !zone.done(); zone.next()) {
    currStats += zone->stringStats;
    zone->previousGCStringStats = zone->stringStats;
  }
  stats().setStat(
      gcstats::STAT_STRINGS_DEDUPLICATED,
      currStats.deduplicatedStrings - prevStats.deduplicatedStrings);
  if (ShouldPrintProfile(runtime(), enableProfiling_, profileWorkers_,
                         profileThreshold_, totalTime)) {
    printCollectionProfile(reason, promotionRate);
  }

  if (reportDeduplications_) {
    printDeduplicationData(prevStats, currStats);
  }
}

void js::Nursery::sendTelemetry(JS::GCReason reason, TimeDuration totalTime,
                                bool wasEmpty, double promotionRate,
                                size_t sitesPretenured) {
  JSRuntime* rt = runtime();
  rt->metrics().GC_MINOR_REASON(uint32_t(reason));

  // Long minor GCs are those that take more than 1ms.
  bool wasLongMinorGC = totalTime.ToMilliseconds() > 1.0;
  if (wasLongMinorGC) {
    rt->metrics().GC_MINOR_REASON_LONG(uint32_t(reason));
  }
  rt->metrics().GC_MINOR_US(totalTime);
  rt->metrics().GC_NURSERY_BYTES_2(totalCommitted());

  if (!wasEmpty) {
    rt->metrics().GC_PRETENURE_COUNT_2(sitesPretenured);
    rt->metrics().GC_NURSERY_PROMOTION_RATE(promotionRate * 100);
  }
}

void js::Nursery::printDeduplicationData(js::StringStats& prev,
                                         js::StringStats& curr) {
  if (curr.deduplicatedStrings > prev.deduplicatedStrings) {
    fprintf(stderr,
            "pid %zu: deduplicated %" PRIi64 " strings, %" PRIu64
            " chars, %" PRIu64 " malloc bytes\n",
            size_t(getpid()),
            curr.deduplicatedStrings - prev.deduplicatedStrings,
            curr.deduplicatedChars - prev.deduplicatedChars,
            curr.deduplicatedBytes - prev.deduplicatedBytes);
  }
}

void js::Nursery::freeTrailerBlocks(JS::GCOptions options,
                                    JS::GCReason reason) {
  fromSpace.freeTrailerBlocks(mallocedBlockCache_);

  if (options == JS::GCOptions::Shrink || gc::IsOOMReason(reason)) {
    mallocedBlockCache_.clear();
    return;
  }

  // Discard blocks from the cache at 0.05% per megabyte of nursery capacity,
  // that is, 0.8% of blocks for a 16-megabyte nursery.  This allows the cache
  // to gradually discard unneeded blocks in long running applications.
  mallocedBlockCache_.preen(0.05 * double(capacity()) / (1024.0 * 1024.0));
}

void js::Nursery::Space::freeTrailerBlocks(
    MallocedBlockCache& mallocedBlockCache) {
  // This routine frees those blocks denoted by the set
  //
  //  trailersAdded_ (all of it)
  //    - trailersRemoved_ (entries with index below trailersRemovedUsed_)
  //
  // For each block, places it back on the nursery's small-malloced-block pool
  // by calling mallocedBlockCache.free.

  MOZ_ASSERT(trailersAdded_.length() == trailersRemoved_.length());
  MOZ_ASSERT(trailersRemovedUsed_ <= trailersRemoved_.length());

  // Sort the removed entries.
  std::sort(trailersRemoved_.begin(),
            trailersRemoved_.begin() + trailersRemovedUsed_,
            [](const void* block1, const void* block2) {
              return uintptr_t(block1) < uintptr_t(block2);
            });

  // Use one of two schemes to enumerate the set subtraction.
  if (trailersRemovedUsed_ < 1000) {
    // If the number of removed items is relatively small, it isn't worth the
    // cost of sorting `trailersAdded_`.  Instead, walk through the vector in
    // whatever order it is and use binary search to establish whether each
    // item is present in trailersRemoved_[0 .. trailersRemovedUsed_ - 1].
    const size_t nAdded = trailersAdded_.length();
    for (size_t i = 0; i < nAdded; i++) {
      const PointerAndUint7 block = trailersAdded_[i];
      const void* blockPointer = block.pointer();
      if (!std::binary_search(trailersRemoved_.begin(),
                              trailersRemoved_.begin() + trailersRemovedUsed_,
                              blockPointer)) {
        mallocedBlockCache.free(block);
      }
    }
  } else {
    // The general case, which is algorithmically safer for large inputs.
    // Sort the added entries, and then walk through both them and the removed
    // entries in lockstep.
    std::sort(trailersAdded_.begin(), trailersAdded_.end(),
              [](const PointerAndUint7& block1, const PointerAndUint7& block2) {
                return uintptr_t(block1.pointer()) <
                       uintptr_t(block2.pointer());
              });
    // Enumerate the set subtraction.  This is somewhat simplified by the fact
    // that all elements of the removed set must also be present in the added
    // set. (the "inclusion property").
    const size_t nAdded = trailersAdded_.length();
    const size_t nRemoved = trailersRemovedUsed_;
    size_t iAdded;
    size_t iRemoved = 0;
    for (iAdded = 0; iAdded < nAdded; iAdded++) {
      if (iRemoved == nRemoved) {
        // We've run out of items to skip, so move on to the next loop.
        break;
      }
      const PointerAndUint7 blockAdded = trailersAdded_[iAdded];
      const void* blockRemoved = trailersRemoved_[iRemoved];
      if (blockAdded.pointer() < blockRemoved) {
        mallocedBlockCache.free(blockAdded);
        continue;
      }
      // If this doesn't hold
      // (that is, if `blockAdded.pointer() > blockRemoved`),
      // then the abovementioned inclusion property doesn't hold.
      MOZ_RELEASE_ASSERT(blockAdded.pointer() == blockRemoved);
      iRemoved++;
    }
    MOZ_ASSERT(iRemoved == nRemoved);
    // We've used up the removed set, so now finish up the remainder of the
    // added set.
    for (/*keep going*/; iAdded < nAdded; iAdded++) {
      const PointerAndUint7 block = trailersAdded_[iAdded];
      mallocedBlockCache.free(block);
    }
  }

  // And empty out both sets, but preserve the underlying storage.
  trailersAdded_.clear();
  trailersRemoved_.clear();
  trailersRemovedUsed_ = 0;
  trailerBytes_ = 0;
}

size_t Nursery::sizeOfTrailerBlockSets(
    mozilla::MallocSizeOf mallocSizeOf) const {
  MOZ_ASSERT(fromSpace.trailersAdded_.empty());
  MOZ_ASSERT(fromSpace.trailersRemoved_.empty());
  return toSpace.trailersAdded_.sizeOfExcludingThis(mallocSizeOf) +
         toSpace.trailersRemoved_.sizeOfExcludingThis(mallocSizeOf);
}

js::Nursery::CollectionResult js::Nursery::doCollection(AutoGCSession& session,
                                                        JS::GCOptions options,
                                                        JS::GCReason reason) {
  JSRuntime* rt = runtime();
  AutoSetThreadIsPerformingGC performingGC(rt->gcContext());
  AutoStopVerifyingBarriers av(rt, false);
  AutoDisableProxyCheck disableStrictProxyChecking;
  mozilla::DebugOnly<AutoEnterOOMUnsafeRegion> oomUnsafeRegion;

#ifdef JS_GC_ZEAL
  if (gc->hasZealMode(ZealMode::CheckHeapBeforeMinorGC)) {
    gc->checkHeapBeforeMinorGC(session);
  }
#endif

  // Swap nursery spaces.
  swapSpaces();
  MOZ_ASSERT(toSpace.isEmpty());
  MOZ_ASSERT(toSpace.mallocedBuffers.empty());
  if (semispaceEnabled_) {
    poisonAndInitCurrentChunk();
  }

  clearMapAndSetNurseryIterators();

  MOZ_ASSERT(sweepTask->isIdle());
  {
    BufferAllocator::MaybeLock lock;
    for (ZonesIter zone(runtime(), WithAtoms); !zone.done(); zone.next()) {
      zone->bufferAllocator.startMinorCollection(lock);
    }
  }

  // Move objects pointed to by roots from the nursery to the major heap.
  tenuredEverything = shouldTenureEverything(reason);
  TenuringTracer mover(rt, this, tenuredEverything);
#ifdef JS_GC_ZEAL
  if (reportPromotion_) {
    mover.initPromotionReport();
  }
#endif

  // Trace everything considered as a root by a minor GC.
  traceRoots(session, mover);

  startProfile(ProfileKey::SweepCaches);
  gc->purgeRuntimeForMinorGC();
  endProfile(ProfileKey::SweepCaches);

  // Most of the work is done here. This loop iterates over objects that have
  // been moved to the major heap. If these objects have any outgoing pointers
  // to the nursery, then those nursery objects get moved as well, until no
  // objects are left to move. That is, we iterate to a fixed point.
  startProfile(ProfileKey::CollectToObjFP);
  mover.collectToObjectFixedPoint();
  endProfile(ProfileKey::CollectToObjFP);

  startProfile(ProfileKey::CollectToStrFP);
  mover.collectToStringFixedPoint();
  endProfile(ProfileKey::CollectToStrFP);

#ifdef JS_GC_ZEAL
  if (reportPromotion_ && options != JS::GCOptions::Shutdown) {
    JSContext* cx = runtime()->mainContextFromOwnThread();
    JS::AutoAssertNoGC nogc(cx);
    mover.printPromotionReport(cx, reason, nogc);
  }
#endif

  // Sweep to update any pointers to nursery objects that have now been
  // tenured.
  startProfile(ProfileKey::Sweep);
  sweep();
  endProfile(ProfileKey::Sweep);

  // Update any slot or element pointers whose destination has been tenured.
  startProfile(ProfileKey::UpdateJitActivations);
  js::jit::UpdateJitActivationsForMinorGC(rt);
  forwardedBuffers.clearAndCompact();
  endProfile(ProfileKey::UpdateJitActivations);

  startProfile(ProfileKey::ObjectsTenuredCallback);
  gc->callObjectsTenuredCallback();
  endProfile(ProfileKey::ObjectsTenuredCallback);

  // Sweep malloced buffers.
  startProfile(ProfileKey::FreeMallocedBuffers);
  gc->queueBuffersForFreeAfterMinorGC(fromSpace.mallocedBuffers,
                                      stringBuffersToReleaseAfterMinorGC_);
  fromSpace.mallocedBufferBytes = 0;
  endProfile(ProfileKey::FreeMallocedBuffers);

  // Give trailer blocks associated with non-tenured Wasm{Struct,Array}Objects
  // back to our `mallocedBlockCache_`.
  startProfile(ProfileKey::FreeTrailerBlocks);
  freeTrailerBlocks(options, reason);
  endProfile(ProfileKey::FreeTrailerBlocks);

  startProfile(ProfileKey::ClearNursery);
  clear();
  endProfile(ProfileKey::ClearNursery);

  // Purge the StringToAtomCache. This has to happen at the end because the
  // cache is used when tenuring strings.
  startProfile(ProfileKey::PurgeStringToAtomCache);
  runtime()->caches().stringToAtomCache.purge();
  endProfile(ProfileKey::PurgeStringToAtomCache);

#ifdef JS_GC_ZEAL
  // Make sure hashtables have been updated after the collection.
  startProfile(ProfileKey::CheckHashTables);
  if (gc->hasZealMode(ZealMode::CheckHashTablesOnMinorGC)) {
    runtime()->caches().checkEvalCacheAfterMinorGC();
    gc->checkHashTablesAfterMovingGC();
  }
  endProfile(ProfileKey::CheckHashTables);

  // Check for missing post barriers.
  if (gc->hasZealMode(ZealMode::VerifierPost)) {
    gc->verifyPostBarriers(session);
  }
#endif

  if (semispaceEnabled_) {
    // On the next collection, tenure everything before |tenureThreshold_|.
    tenureThreshold_ = toSpace.offsetFromExclusiveAddress(position());
  } else {
    // Swap nursery spaces back because we only use one.
    swapSpaces();
    MOZ_ASSERT(toSpace.isEmpty());
  }

  MOZ_ASSERT(fromSpace.isEmpty());

  if (semispaceEnabled_) {
    poisonAndInitCurrentChunk();
  }

  return {mover.getPromotedSize(), mover.getPromotedCells()};
}

void js::Nursery::swapSpaces() {
  std::swap(toSpace, fromSpace);
  toSpace.setKind(ChunkKind::NurseryToSpace);
  fromSpace.setKind(ChunkKind::NurseryFromSpace);
}

void js::Nursery::traceRoots(AutoGCSession& session, TenuringTracer& mover) {
  {
    // Suppress the sampling profiler to prevent it observing moved functions.
    AutoSuppressProfilerSampling suppressProfiler(
        runtime()->mainContextFromOwnThread());

    // Trace the store buffer, which must happen first.

    // Create an empty store buffer on the stack and swap it with the main store
    // buffer, clearing it.
    StoreBuffer sb(runtime());
    {
      AutoEnterOOMUnsafeRegion oomUnsafe;
      if (!sb.enable()) {
        oomUnsafe.crash("Nursery::traceRoots");
      }
    }
    std::swap(sb, gc->storeBuffer());
    MOZ_ASSERT(gc->storeBuffer().isEnabled());
    MOZ_ASSERT(gc->storeBuffer().isEmpty());

    startProfile(ProfileKey::TraceWholeCells);
    sb.traceWholeCells(mover);
    endProfile(ProfileKey::TraceWholeCells);

    startProfile(ProfileKey::TraceValues);
    sb.traceValues(mover);
    endProfile(ProfileKey::TraceValues);

    startProfile(ProfileKey::TraceWasmAnyRefs);
    sb.traceWasmAnyRefs(mover);
    endProfile(ProfileKey::TraceWasmAnyRefs);

    startProfile(ProfileKey::TraceCells);
    sb.traceCells(mover);
    endProfile(ProfileKey::TraceCells);

    startProfile(ProfileKey::TraceSlots);
    sb.traceSlots(mover);
    endProfile(ProfileKey::TraceSlots);

    startProfile(ProfileKey::TraceGenericEntries);
    sb.traceGenericEntries(&mover);
    endProfile(ProfileKey::TraceGenericEntries);

    startProfile(ProfileKey::MarkRuntime);
    gc->traceRuntimeForMinorGC(&mover, session);
    endProfile(ProfileKey::MarkRuntime);
  }

  startProfile(ProfileKey::MarkDebugger);
  {
    gcstats::AutoPhase ap(stats(), gcstats::PhaseKind::MARK_ROOTS);
    DebugAPI::traceAllForMovingGC(&mover);
  }
  endProfile(ProfileKey::MarkDebugger);
}

bool js::Nursery::shouldTenureEverything(JS::GCReason reason) {
  if (!semispaceEnabled()) {
    return true;
  }

  return reason == JS::GCReason::EVICT_NURSERY ||
         reason == JS::GCReason::DISABLE_GENERATIONAL_GC;
}

size_t js::Nursery::doPretenuring(JSRuntime* rt, JS::GCReason reason,
                                  bool validPromotionRate,
                                  double promotionRate) {
  size_t sitesPretenured = pretenuringNursery.doPretenuring(
      gc, reason, validPromotionRate, promotionRate, pretenuringReportFilter_);

  size_t zonesWhereStringsDisabled = 0;
  size_t zonesWhereBigIntsDisabled = 0;

  uint32_t numStringsPromoted = 0;
  uint32_t numBigIntsPromoted = 0;
  for (ZonesIter zone(gc, SkipAtoms); !zone.done(); zone.next()) {
    bool disableNurseryStrings =
        zone->allocNurseryStrings() &&
        zone->unknownAllocSite(JS::TraceKind::String)->state() ==
            AllocSite::State::LongLived;

    bool disableNurseryBigInts =
        zone->allocNurseryBigInts() &&
        zone->unknownAllocSite(JS::TraceKind::BigInt)->state() ==
            AllocSite::State::LongLived;

    if (disableNurseryStrings || disableNurseryBigInts) {
      if (disableNurseryStrings) {
        zone->nurseryStringsDisabled = true;
        zonesWhereStringsDisabled++;
      }
      if (disableNurseryBigInts) {
        zone->nurseryBigIntsDisabled = true;
        zonesWhereStringsDisabled++;
      }
      updateAllocFlagsForZone(zone);
    }

    numStringsPromoted += zone->nurseryPromotedCount(JS::TraceKind::String);
    numBigIntsPromoted += zone->nurseryPromotedCount(JS::TraceKind::BigInt);
  }

  stats().setStat(gcstats::STAT_STRINGS_PROMOTED, numStringsPromoted);
  stats().setStat(gcstats::STAT_BIGINTS_PROMOTED, numBigIntsPromoted);

  if (reportPretenuring() && zonesWhereStringsDisabled) {
    fprintf(stderr,
            "Pretenuring disabled nursery string allocation in %zu zones\n",
            zonesWhereStringsDisabled);
  }
  if (reportPretenuring() && zonesWhereBigIntsDisabled) {
    fprintf(stderr,
            "Pretenuring disabled nursery big int allocation in %zu zones\n",
            zonesWhereBigIntsDisabled);
  }

  return sitesPretenured;
}

bool js::Nursery::registerMallocedBuffer(void* buffer, size_t nbytes) {
  MOZ_ASSERT(buffer);
  MOZ_ASSERT(nbytes > 0);
  MOZ_ASSERT(!isInside(buffer));

  if (!toSpace.mallocedBuffers.putNew(buffer)) {
    return false;
  }

  addMallocedBufferBytes(nbytes);
  return true;
}

void js::Nursery::registerBuffer(void* buffer, size_t nbytes) {
  MOZ_ASSERT(buffer);
  MOZ_ASSERT(nbytes > 0);
  MOZ_ASSERT(!isInside(buffer));

  addMallocedBufferBytes(nbytes);
}

/*
 * Several things may need to happen when a nursery allocated cell with an
 * external buffer is promoted:
 *  - the buffer may need to be moved if it is currently in the nursery
 *  - the buffer may need to be removed from the list of buffers that will be
 *    freed after nursery collection if it is malloced
 *  - memory accounting for the buffer needs to be updated
 */
Nursery::WasBufferMoved js::Nursery::maybeMoveRawBufferOnPromotion(
    void** bufferp, gc::Cell* owner, size_t bytesUsed, size_t bytesCapacity,
    MemoryUse use, arena_id_t arena) {
  MOZ_ASSERT(bytesUsed <= bytesCapacity);

  void* buffer = *bufferp;
  if (!isInside(buffer)) {
    // This is a malloced buffer. Remove it from the nursery's previous list of
    // buffers so we don't free it.
    removeMallocedBufferDuringMinorGC(buffer);
    trackMallocedBufferOnPromotion(buffer, owner, bytesCapacity, use);
    return BufferNotMoved;
  }

  // Copy the nursery-allocated buffer into a new malloc allocation.

  AutoEnterOOMUnsafeRegion oomUnsafe;
  Zone* zone = owner->zone();
  void* movedBuffer = zone->pod_arena_malloc<uint8_t>(arena, bytesCapacity);
  if (!movedBuffer) {
    oomUnsafe.crash("Nursery::maybeMoveRawNurseryOrMallocBufferOnPromotion");
  }

  memcpy(movedBuffer, buffer, bytesUsed);

  trackMallocedBufferOnPromotion(movedBuffer, owner, bytesCapacity, use);

  *bufferp = movedBuffer;
  return BufferMoved;
}

void js::Nursery::trackMallocedBufferOnPromotion(void* buffer, gc::Cell* owner,
                                                 size_t nbytes, MemoryUse use) {
  if (owner->isTenured()) {
    // If we tenured the owner then account for the memory.
    AddCellMemory(owner, nbytes, use);
    return;
  }

  // Otherwise add it to the nursery's new buffer list.
  AutoEnterOOMUnsafeRegion oomUnsafe;
  if (!registerMallocedBuffer(buffer, nbytes)) {
    oomUnsafe.crash("Nursery::trackMallocedBufferOnPromotion");
  }
}

void js::Nursery::trackTrailerOnPromotion(void* buffer, gc::Cell* owner,
                                          size_t nbytes, size_t overhead,
                                          MemoryUse use) {
  MOZ_ASSERT(!isInside(buffer));
  unregisterTrailer(buffer);

  if (owner->isTenured()) {
    // If we tenured the owner then account for the memory.
    AddCellMemory(owner, nbytes + overhead, use);
    return;
  }

  // Otherwise add it to the nursery's new buffer list.
  PointerAndUint7 blockAndListID(buffer,
                                 MallocedBlockCache::listIDForSize(nbytes));
  AutoEnterOOMUnsafeRegion oomUnsafe;
  if (!registerTrailer(blockAndListID, nbytes)) {
    oomUnsafe.crash("Nursery::trackTrailerOnPromotion");
  }
}

Nursery::WasBufferMoved js::Nursery::maybeMoveRawBufferOnPromotion(
    void** bufferp, gc::Cell* owner, size_t nbytes) {
  bool nurseryOwned = IsInsideNursery(owner);

  void* buffer = *bufferp;
  if (IsBufferAlloc(buffer)) {
    // This is an external buffer allocation owned by a nursery GC thing.
    Zone* zone = owner->zone();
    MOZ_ASSERT(IsNurseryOwned(zone, buffer));
    bool ownerWasTenured = !nurseryOwned;
    zone->bufferAllocator.markNurseryOwnedAlloc(buffer, ownerWasTenured);
    if (nurseryOwned) {
      registerBuffer(buffer, nbytes);
    }
    return BufferNotMoved;
  }

  // Copy the nursery-allocated buffer into a new allocation.

  // todo: only necessary for copying inline elements data where we didn't
  // calculate this on allocation.
  size_t dstBytes = GetGoodAllocSize(nbytes);

  AutoEnterOOMUnsafeRegion oomUnsafe;
  void* movedBuffer = AllocBufferInGC(owner->zone(), dstBytes, nurseryOwned);
  if (!movedBuffer) {
    oomUnsafe.crash("Nursery::maybeMoveRawBufferOnPromotion");
  }

  memcpy(movedBuffer, buffer, nbytes);

  if (nurseryOwned) {
    registerBuffer(movedBuffer, nbytes);
  }

  *bufferp = movedBuffer;
  return BufferMoved;
}

void js::Nursery::sweepBuffers() {
  for (ZonesIter zone(runtime(), WithAtoms); !zone.done(); zone.next()) {
    if (zone->bufferAllocator.startMinorSweeping()) {
      sweepTask->queueAllocatorToSweep(zone->bufferAllocator);
    }
  }

  AutoLockHelperThreadState lock;
  if (!sweepTask->isEmpty(lock)) {
    sweepTask->startOrRunIfIdle(lock);
  }
}

void Nursery::requestMinorGC(JS::GCReason reason) {
  JS::HeapState heapState = runtime()->heapState();
#ifdef DEBUG
  if (heapState == JS::HeapState::Idle ||
      heapState == JS::HeapState::MinorCollecting) {
    MOZ_ASSERT(CurrentThreadCanAccessRuntime(runtime()));
  } else if (heapState == JS::HeapState::MajorCollecting) {
    // The GC runs sweeping tasks that may access the storebuffer in parallel
    // and these require taking the store buffer lock.
    MOZ_ASSERT(!CurrentThreadIsGCMarking());
    runtime()->gc.assertCurrentThreadHasLockedStoreBuffer();
  } else {
    MOZ_CRASH("Unexpected heap state");
  }
#endif

  MOZ_ASSERT(reason != JS::GCReason::NO_REASON);
  MOZ_ASSERT(isEnabled());

  if (minorGCRequested()) {
    return;
  }

  if (heapState == JS::HeapState::MinorCollecting) {
    // This can happen when we promote a lot of data to the second generation in
    // a semispace collection. This can trigger a GC due to the amount of store
    // buffer entries added.
    return;
  }

  // Set position to end of chunk to block further allocation.
  MOZ_ASSERT(prevPosition_ == 0);
  prevPosition_ = position();
  toSpace.position_ = chunk(currentChunk()).end();

  minorGCTriggerReason_ = reason;
  runtime()->mainContextFromAnyThread()->requestInterrupt(
      InterruptReason::MinorGC);
}

size_t SemispaceSizeFactor(bool semispaceEnabled) {
  return semispaceEnabled ? 2 : 1;
}

size_t js::Nursery::totalCapacity() const {
  return capacity() * SemispaceSizeFactor(semispaceEnabled_);
}

size_t js::Nursery::totalCommitted() const {
  size_t size = std::min(capacity_, allocatedChunkCount() * gc::ChunkSize);
  return size * SemispaceSizeFactor(semispaceEnabled_);
}

size_t Nursery::sizeOfMallocedBuffers(
    mozilla::MallocSizeOf mallocSizeOf) const {
  MOZ_ASSERT(fromSpace.mallocedBuffers.empty());

  size_t total = 0;
  for (BufferSet::Range r = toSpace.mallocedBuffers.all(); !r.empty();
       r.popFront()) {
    total += mallocSizeOf(r.front());
  }
  total += toSpace.mallocedBuffers.shallowSizeOfExcludingThis(mallocSizeOf);

  for (AllZonesIter zone(runtime()); !zone.done(); zone.next()) {
    total += zone->bufferAllocator.getSizeOfNurseryBuffers();
  }

  return total;
}

void js::Nursery::sweepStringsWithBuffer() {
  // Add StringBuffers to stringBuffersToReleaseAfterMinorGC_. Strings we
  // tenured must have an additional refcount at this point.

  MOZ_ASSERT(stringBuffersToReleaseAfterMinorGC_.empty());

  auto sweep = [&](JSLinearString* str,
                   mozilla::StringBuffer* buffer) -> JSLinearString* {
    MOZ_ASSERT(inCollectedRegion(str));

    if (!IsForwarded(str)) {
      MOZ_ASSERT(str->hasStringBuffer() || str->isAtomRef());
      MOZ_ASSERT_IF(str->hasStringBuffer(), str->stringBuffer() == buffer);
      if (!stringBuffersToReleaseAfterMinorGC_.append(buffer)) {
        // Release on the main thread on OOM.
        buffer->Release();
      }
      return nullptr;
    }

    JSLinearString* dst = Forwarded(str);
    if (!IsInsideNursery(dst)) {
      MOZ_ASSERT_IF(dst->hasStringBuffer() && dst->stringBuffer() == buffer,
                    buffer->RefCount() > 1);
      if (!stringBuffersToReleaseAfterMinorGC_.append(buffer)) {
        // Release on the main thread on OOM.
        buffer->Release();
      }
      return nullptr;
    }

    return dst;
  };

  stringBuffers_.mutableEraseIf([&](StringAndBuffer& entry) {
    if (JSLinearString* dst = sweep(entry.first, entry.second)) {
      entry.first = dst;
      // See comment in Nursery::addStringBuffer.
      if (!entry.second->HasMultipleReferences()) {
        addMallocedBufferBytes(entry.second->AllocationSize());
      }
      return false;
    }
    return true;
  });

  AutoEnterOOMUnsafeRegion oomUnsafe;

  ExtensibleStringBuffers buffers(std::move(extensibleStringBuffers_));
  MOZ_ASSERT(extensibleStringBuffers_.empty());

  for (ExtensibleStringBuffers::Enum e(buffers); !e.empty(); e.popFront()) {
    if (JSLinearString* dst = sweep(e.front().key(), e.front().value())) {
      if (!extensibleStringBuffers_.putNew(dst, e.front().value())) {
        oomUnsafe.crash("sweepStringsWithBuffer");
      }
      // Ensure mallocedBufferBytes includes the buffer size for
      // removeExtensibleStringBuffer.
      addMallocedBufferBytes(e.front().value()->AllocationSize());
    }
  }
}

void js::Nursery::sweep() {
  // It's important that the context's GCUse is not Finalizing at this point,
  // otherwise we will miscount memory attached to nursery objects with
  // CellAllocPolicy.
  AutoSetThreadIsSweeping setThreadSweeping(runtime()->gcContext());

  // Start sweeping buffers off-thread as soon as possible.
  sweepBuffers();

  MinorSweepingTracer trc(runtime());

  // Sweep unique IDs first before we sweep any tables that may be keyed based
  // on them.
  cellsWithUid_.mutableEraseIf([](Cell*& cell) {
    auto* obj = static_cast<JSObject*>(cell);
    if (!IsForwarded(obj)) {
      gc::RemoveUniqueId(obj);
      return true;
    }

    JSObject* dst = Forwarded(obj);
    gc::TransferUniqueId(dst, obj);

    if (!IsInsideNursery(dst)) {
      return true;
    }

    cell = dst;
    return false;
  });

  sweepStringsWithBuffer();

  for (ZonesIter zone(runtime(), SkipAtoms); !zone.done(); zone.next()) {
    zone->sweepAfterMinorGC(&trc);
  }

  sweepMapAndSetObjects();

  runtime()->caches().sweepAfterMinorGC(&trc);
}

void js::Nursery::clear() {
  fromSpace.clear(this);
  MOZ_ASSERT(fromSpace.isEmpty());
}

void js::Nursery::Space::clear(Nursery* nursery) {
  GCRuntime* gc = nursery->gc;

  // Poison the nursery contents so touching a freed object will crash.
  unsigned firstClearChunk;
  if (gc->hasZealMode(ZealMode::GenerationalGC) || nursery->semispaceEnabled_) {
    // Poison all the chunks used in this cycle.
    firstClearChunk = startChunk_;
  } else {
    // Poison from the second chunk onwards as the first one will be used
    // in the next cycle and poisoned in Nusery::collect();
    MOZ_ASSERT(startChunk_ == 0);
    firstClearChunk = 1;
  }
  for (unsigned i = firstClearChunk; i < currentChunk_; ++i) {
    chunks_[i]->poisonAfterEvict();
  }
  // Clear only the used part of the chunk because that's the part we touched,
  // but only if it's not going to be re-used immediately (>= firstClearChunk).
  if (currentChunk_ >= firstClearChunk) {
    size_t usedBytes = position_ - chunks_[currentChunk_]->start();
    chunks_[currentChunk_]->poisonAfterEvict(NurseryChunkHeaderSize +
                                             usedBytes);
  }

  // Reset the start chunk & position if we're not in this zeal mode, or we're
  // in it and close to the end of the nursery.
  MOZ_ASSERT(maxChunkCount_ > 0);
  if (!gc->hasZealMode(ZealMode::GenerationalGC) ||
      currentChunk_ + 1 == maxChunkCount_) {
    moveToStartOfChunk(nursery, 0);
  }

  // Set current start position for isEmpty checks.
  setStartToCurrentPosition();
}

void js::Nursery::moveToStartOfChunk(unsigned chunkno) {
  toSpace.moveToStartOfChunk(this, chunkno);
}

void js::Nursery::Space::moveToStartOfChunk(Nursery* nursery,
                                            unsigned chunkno) {
  MOZ_ASSERT(chunkno < chunks_.length());

  currentChunk_ = chunkno;
  position_ = chunks_[chunkno]->start();
  setCurrentEnd(nursery);

  MOZ_ASSERT(position_ != 0);
  MOZ_ASSERT(currentEnd_ > position_);  // Check this cannot wrap.
}

void js::Nursery::poisonAndInitCurrentChunk() {
  NurseryChunk& chunk = this->chunk(currentChunk());
  size_t start = position() - uintptr_t(&chunk);
  size_t end = isSubChunkMode() ? capacity_ : ChunkSize;
  chunk.poisonRange(start, end, JS_FRESH_NURSERY_PATTERN,
                    MemCheckKind::MakeUndefined);
  new (&chunk)
      NurseryChunk(runtime(), ChunkKind::NurseryToSpace, currentChunk());
}

void js::Nursery::setCurrentEnd() { toSpace.setCurrentEnd(this); }

void js::Nursery::Space::setCurrentEnd(Nursery* nursery) {
  currentEnd_ = uintptr_t(chunks_[currentChunk_]) +
                std::min(nursery->capacity(), ChunkSize);
}

bool js::Nursery::allocateNextChunk(AutoLockGCBgAlloc& lock) {
  // Allocate a new nursery chunk. If semispace collection is enabled, we have
  // to allocate one for both spaces.

  const unsigned priorCount = toSpace.chunks_.length();
  const unsigned newCount = priorCount + 1;

  MOZ_ASSERT(newCount <= maxChunkCount());
  MOZ_ASSERT(fromSpace.chunks_.length() ==
             (semispaceEnabled_ ? priorCount : 0));

  if (!toSpace.chunks_.reserve(newCount) ||
      (semispaceEnabled_ && !fromSpace.chunks_.reserve(newCount))) {
    return false;
  }

  ArenaChunk* toSpaceChunk = gc->takeOrAllocChunk(StallAndRetry::No, lock);
  if (!toSpaceChunk) {
    return false;
  }

  ArenaChunk* fromSpaceChunk = nullptr;
  if (semispaceEnabled_ &&
      !(fromSpaceChunk = gc->takeOrAllocChunk(StallAndRetry::No, lock))) {
    gc->recycleChunk(toSpaceChunk, lock);
    return false;
  }

  uint8_t index = toSpace.chunks_.length();
  NurseryChunk* nurseryChunk =
      NurseryChunk::fromChunk(toSpaceChunk, ChunkKind::NurseryToSpace, index);
  toSpace.chunks_.infallibleAppend(nurseryChunk);

  if (semispaceEnabled_) {
    MOZ_ASSERT(index == fromSpace.chunks_.length());
    nurseryChunk = NurseryChunk::fromChunk(fromSpaceChunk,
                                           ChunkKind::NurseryFromSpace, index);
    fromSpace.chunks_.infallibleAppend(nurseryChunk);
  }

  return true;
}

void js::Nursery::setStartToCurrentPosition() {
  toSpace.setStartToCurrentPosition();
}

void js::Nursery::Space::setStartToCurrentPosition() {
  startChunk_ = currentChunk_;
  startPosition_ = position_;
  MOZ_ASSERT(isEmpty());
}

void js::Nursery::maybeResizeNursery(JS::GCOptions options,
                                     JS::GCReason reason) {
#ifdef JS_GC_ZEAL
  // This zeal mode disabled nursery resizing.
  if (gc->hasZealMode(ZealMode::GenerationalGC)) {
    return;
  }
#endif

  decommitTask->join();

  size_t newCapacity =
      std::clamp(targetSize(options, reason), minSpaceSize(), maxSpaceSize());

  MOZ_ASSERT(roundSize(newCapacity) == newCapacity);
  MOZ_ASSERT(newCapacity >= SystemPageSize());

  if (newCapacity > capacity()) {
    growAllocableSpace(newCapacity);
  } else if (newCapacity < capacity()) {
    shrinkAllocableSpace(newCapacity);
  }

  AutoLockHelperThreadState lock;
  if (!decommitTask->isEmpty(lock)) {
    decommitTask->startOrRunIfIdle(lock);
  }
}

static inline bool ClampDouble(double* value, double min, double max) {
  MOZ_ASSERT(!std::isnan(*value) && !std::isnan(min) && !std::isnan(max));
  MOZ_ASSERT(max >= min);

  if (*value <= min) {
    *value = min;
    return true;
  }

  if (*value >= max) {
    *value = max;
    return true;
  }

  return false;
}

size_t js::Nursery::targetSize(JS::GCOptions options, JS::GCReason reason) {
  // Shrink the nursery as much as possible if purging was requested or in low
  // memory situations.
  if (options == JS::GCOptions::Shrink || gc::IsOOMReason(reason) ||
      gc->systemHasLowMemory()) {
    clearRecentGrowthData();
    return 0;
  }

  // Don't resize the nursery during shutdown.
  if (options == JS::GCOptions::Shutdown) {
    clearRecentGrowthData();
    return capacity();
  }

  TimeStamp now = TimeStamp::Now();

  if (reason == JS::GCReason::PREPARE_FOR_PAGELOAD) {
    return roundSize(maxSpaceSize());
  }

  // If the nursery is completely unused then minimise it.
  if (hasRecentGrowthData && previousGC.nurseryUsedBytes == 0 &&
      now - lastCollectionEndTime() >
          tunables().nurseryEagerCollectionTimeout() &&
      !js::SupportDifferentialTesting()) {
    clearRecentGrowthData();
    return 0;
  }

  // Calculate the fraction of the nursery promoted out of its entire
  // capacity. This gives better results than using the promotion rate (based on
  // the amount of nursery used) in cases where we collect before the nursery is
  // full.
  double fractionPromoted =
      double(previousGC.tenuredBytes) / double(previousGC.nurseryCapacity);

  // Calculate the duty factor, the fraction of time spent collecting the
  // nursery.
  double dutyFactor = 0.0;
  TimeDuration collectorTime = now - collectionStartTime();
  if (hasRecentGrowthData && !js::SupportDifferentialTesting()) {
    TimeDuration totalTime = now - lastCollectionEndTime();
    dutyFactor = collectorTime.ToSeconds() / totalTime.ToSeconds();
  }

  // Calculate a growth factor to try to achieve target promotion rate and duty
  // factor goals.
  static const double PromotionGoal = 0.02;
  static const double DutyFactorGoal = 0.01;
  double promotionGrowth = fractionPromoted / PromotionGoal;
  double dutyGrowth = dutyFactor / DutyFactorGoal;
  double growthFactor = std::max(promotionGrowth, dutyGrowth);

#ifndef DEBUG
  // In optimized builds, decrease the growth factor to try to keep collections
  // shorter than a target maximum time. Don't do this during page load.
  //
  // Debug builds are so much slower and more unpredictable that doing this
  // would cause very different nursery behaviour to an equivalent release
  // build.
  double maxTimeGoalMS = tunables().nurseryMaxTimeGoalMS().ToMilliseconds();
  if (!gc->isInPageLoad() && maxTimeGoalMS != 0.0 &&
      !js::SupportDifferentialTesting()) {
    double timeGrowth = maxTimeGoalMS / collectorTime.ToMilliseconds();
    growthFactor = std::min(growthFactor, timeGrowth);
  }
#endif

  // Limit the range of the growth factor to prevent transient high promotion
  // rates from affecting the nursery size too far into the future.
  static const double GrowthRange = 2.0;
  bool wasClamped = ClampDouble(&growthFactor, 1.0 / GrowthRange, GrowthRange);

  // Calculate the target size based on data from this collection.
  double target = double(capacity()) * growthFactor;

  // Use exponential smoothing on the target size to take into account data from
  // recent previous collections.
  if (hasRecentGrowthData &&
      now - lastCollectionEndTime() < TimeDuration::FromMilliseconds(200) &&
      !js::SupportDifferentialTesting()) {
    // Pay more attention to large changes.
    double fraction = wasClamped ? 0.5 : 0.25;
    smoothedTargetSize =
        (1 - fraction) * smoothedTargetSize + fraction * target;
  } else {
    smoothedTargetSize = target;
  }
  hasRecentGrowthData = true;

  // Leave size untouched if we are close to the target.
  static const double GoalWidth = 1.5;
  growthFactor = smoothedTargetSize / double(capacity());
  if (growthFactor > (1.0 / GoalWidth) && growthFactor < GoalWidth) {
    return capacity();
  }

  return roundSize(size_t(smoothedTargetSize));
}

void js::Nursery::clearRecentGrowthData() {
  if (js::SupportDifferentialTesting()) {
    return;
  }

  hasRecentGrowthData = false;
  smoothedTargetSize = 0.0;
}

/* static */
size_t js::Nursery::roundSize(size_t size) {
  size_t step = size >= ChunkSize ? ChunkSize : SystemPageSize();
  return Round(size, step);
}

void js::Nursery::growAllocableSpace(size_t newCapacity) {
  MOZ_ASSERT_IF(!isSubChunkMode(), newCapacity > currentChunk() * ChunkSize);
  MOZ_ASSERT(newCapacity <= maxSpaceSize());
  MOZ_ASSERT(newCapacity > capacity());

  size_t nchunks =
      RequiredChunkCount(newCapacity) * SemispaceSizeFactor(semispaceEnabled_);
  if (!decommitTask->reserveSpaceForChunks(nchunks)) {
    return;
  }

  if (isSubChunkMode()) {
    if (!toSpace.commitSubChunkRegion(capacity(), newCapacity) ||
        (semispaceEnabled_ &&
         !fromSpace.commitSubChunkRegion(capacity(), newCapacity))) {
      return;
    }
  }

  setCapacity(newCapacity);

  toSpace.setCurrentEnd(this);
  if (semispaceEnabled_) {
    fromSpace.setCurrentEnd(this);
  }
}

bool js::Nursery::Space::commitSubChunkRegion(size_t oldCapacity,
                                              size_t newCapacity) {
  MOZ_ASSERT(currentChunk_ == 0);
  MOZ_ASSERT(oldCapacity < ChunkSize);
  MOZ_ASSERT(newCapacity > oldCapacity);

  size_t newChunkEnd = std::min(newCapacity, ChunkSize);

  // The remainder of the chunk may have been decommitted.
  if (!chunks_[0]->markPagesInUseHard(newChunkEnd)) {
    // The OS won't give us the memory we need, we can't grow.
    return false;
  }

  // The capacity has changed and since we were in sub-chunk mode we need to
  // update the poison values / asan information for the now-valid region of
  // this chunk.
  chunks_[0]->poisonRange(oldCapacity, newChunkEnd, JS_FRESH_NURSERY_PATTERN,
                          MemCheckKind::MakeUndefined);
  return true;
}

void js::Nursery::freeChunksFrom(Space& space, const unsigned firstFreeChunk) {
  if (firstFreeChunk >= space.chunks_.length()) {
    return;
  }

  // The loop below may need to skip the first chunk, so we may use this so we
  // can modify it.
  unsigned firstChunkToDecommit = firstFreeChunk;

  if ((firstChunkToDecommit == 0) && isSubChunkMode()) {
    // Part of the first chunk may be hard-decommitted, un-decommit it so that
    // the GC's normal chunk-handling doesn't segfault.
    MOZ_ASSERT(space.currentChunk_ == 0);
    if (!space.chunks_[0]->markPagesInUseHard(ChunkSize)) {
      // Free the chunk if we can't allocate its pages.
      UnmapPages(space.chunks_[0], ChunkSize);
      firstChunkToDecommit = 1;
    }
  }

  {
    AutoLockHelperThreadState lock;
    for (size_t i = firstChunkToDecommit; i < space.chunks_.length(); i++) {
      decommitTask->queueChunk(space.chunks_[i], lock);
    }
  }

  space.chunks_.shrinkTo(firstFreeChunk);
}

void js::Nursery::shrinkAllocableSpace(size_t newCapacity) {
  MOZ_ASSERT(!gc->hasZealMode(ZealMode::GenerationalGC));
  MOZ_ASSERT(newCapacity < capacity_);

  if (semispaceEnabled() && usedSpace() >= newCapacity) {
    // Can't shrink below what we've already used.
    return;
  }

  unsigned newCount = HowMany(newCapacity, ChunkSize);
  if (newCount < allocatedChunkCount()) {
    freeChunksFrom(toSpace, newCount);
    freeChunksFrom(fromSpace, newCount);
  }

  size_t oldCapacity = capacity_;
  setCapacity(newCapacity);

  toSpace.setCurrentEnd(this);
  if (semispaceEnabled_) {
    fromSpace.setCurrentEnd(this);
  }

  if (isSubChunkMode()) {
    toSpace.decommitSubChunkRegion(this, oldCapacity, newCapacity);
    if (semispaceEnabled_) {
      fromSpace.decommitSubChunkRegion(this, oldCapacity, newCapacity);
    }
  }
}

void js::Nursery::Space::decommitSubChunkRegion(Nursery* nursery,
                                                size_t oldCapacity,
                                                size_t newCapacity) {
  MOZ_ASSERT(currentChunk_ == 0);
  MOZ_ASSERT(newCapacity < ChunkSize);
  MOZ_ASSERT(newCapacity < oldCapacity);

  size_t oldChunkEnd = std::min(oldCapacity, ChunkSize);
  chunks_[0]->poisonRange(newCapacity, oldChunkEnd, JS_SWEPT_NURSERY_PATTERN,
                          MemCheckKind::MakeNoAccess);

  AutoLockHelperThreadState lock;
  nursery->decommitTask->queueRange(newCapacity, chunks_[0], lock);
}

js::Nursery::Space::Space(gc::ChunkKind kind) : kind(kind) {
  MOZ_ASSERT(kind == ChunkKind::NurseryFromSpace ||
             kind == ChunkKind::NurseryToSpace);
}

void js::Nursery::Space::setKind(ChunkKind newKind) {
#ifdef DEBUG
  MOZ_ASSERT(newKind == ChunkKind::NurseryFromSpace ||
             newKind == ChunkKind::NurseryToSpace);
  checkKind(kind);
#endif

  kind = newKind;
  for (NurseryChunk* chunk : chunks_) {
    chunk->kind = newKind;
  }

#ifdef DEBUG
  checkKind(newKind);
#endif
}

#ifdef DEBUG
void js::Nursery::Space::checkKind(ChunkKind expected) const {
  MOZ_ASSERT(kind == expected);
  for (NurseryChunk* chunk : chunks_) {
    MOZ_ASSERT(chunk->getKind() == expected);
  }
}
#endif

#ifdef DEBUG
size_t js::Nursery::Space::findChunkIndex(uintptr_t chunkAddr) const {
  for (size_t i = 0; i < chunks_.length(); i++) {
    if (uintptr_t(chunks_[i]) == chunkAddr) {
      return i;
    }
  }

  MOZ_CRASH("Nursery chunk not found");
}
#endif

gcstats::Statistics& js::Nursery::stats() const { return gc->stats(); }

MOZ_ALWAYS_INLINE const js::gc::GCSchedulingTunables& js::Nursery::tunables()
    const {
  return gc->tunables;
}

bool js::Nursery::isSubChunkMode() const {
  return capacity() <= NurseryChunkUsableSize;
}

void js::Nursery::clearMapAndSetNurseryIterators() {
  // Clears the lists of nursery iterators for all Map/Set objects. These lists
  // are cleared at the start of minor GC and rebuilt when iterators are
  // promoted during minor GC.
  for (auto* map : mapsWithNurseryIterators_) {
    map->clearNurseryIteratorsBeforeMinorGC();
  }
  for (auto* set : setsWithNurseryIterators_) {
    set->clearNurseryIteratorsBeforeMinorGC();
  }
}

void js::Nursery::sweepMapAndSetObjects() {
  // This processes all Map and Set objects that are known to have associated
  // iterator objects that are nursery allocated.
  //
  // These objects may die and be finalized or if not their internal state and
  // memory tracking are updated.
  //
  // Finally the lists themselves are rebuilt so as to remove objects that are
  // no longer associated with nursery iterators (because all iterators died or
  // were promoted to the tenured heap).

  auto* gcx = runtime()->gcContext();

  AutoEnterOOMUnsafeRegion oomUnsafe;

  MapObjectVector maps;
  std::swap(mapsWithNurseryIterators_, maps);
  for (auto* mapobj : maps) {
    mapobj = MapObject::sweepAfterMinorGC(gcx, mapobj);
    if (mapobj) {
      if (!mapsWithNurseryIterators_.append(mapobj)) {
        oomUnsafe.crash("sweepAfterMinorGC");
      }
    }
  }

  SetObjectVector sets;
  std::swap(setsWithNurseryIterators_, sets);
  for (auto* setobj : sets) {
    setobj = SetObject::sweepAfterMinorGC(gcx, setobj);
    if (setobj) {
      if (!setsWithNurseryIterators_.append(setobj)) {
        oomUnsafe.crash("sweepAfterMinorGC");
      }
    }
  }
}

void js::Nursery::joinSweepTask() { sweepTask->join(); }
void js::Nursery::joinDecommitTask() { decommitTask->join(); }

#ifdef DEBUG
bool js::Nursery::sweepTaskIsIdle() { return sweepTask->isIdle(); }
#endif
