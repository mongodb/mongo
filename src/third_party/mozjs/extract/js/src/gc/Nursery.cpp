/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sw=2 et tw=80:
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "gc/Nursery-inl.h"

#include "mozilla/DebugOnly.h"
#include "mozilla/IntegerPrintfMacros.h"
#include "mozilla/ScopeExit.h"
#include "mozilla/Sprintf.h"
#include "mozilla/TimeStamp.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "builtin/MapObject.h"
#include "debugger/DebugAPI.h"
#include "gc/GCInternals.h"
#include "gc/GCLock.h"
#include "gc/GCParallelTask.h"
#include "gc/GCProbes.h"
#include "gc/Memory.h"
#include "gc/PublicIterators.h"
#include "gc/Tenuring.h"
#include "jit/JitFrames.h"
#include "jit/JitRealm.h"
#include "js/Printer.h"
#include "util/DifferentialTesting.h"
#include "util/GetPidProvider.h"  // getpid()
#include "util/Poison.h"
#include "vm/JSONPrinter.h"
#include "vm/Realm.h"
#include "vm/Time.h"

#include "gc/Heap-inl.h"
#include "gc/Marking-inl.h"
#include "gc/StableCellHasher-inl.h"
#include "vm/GeckoProfiler-inl.h"

using namespace js;
using namespace js::gc;

using mozilla::DebugOnly;
using mozilla::PodCopy;
using mozilla::TimeDuration;
using mozilla::TimeStamp;

namespace js {

struct NurseryChunk : public ChunkBase {
  char data[Nursery::NurseryChunkUsableSize];

  static NurseryChunk* fromChunk(gc::TenuredChunk* chunk);

  explicit NurseryChunk(JSRuntime* runtime)
      : ChunkBase(runtime, &runtime->gc.storeBuffer()) {}

  void poisonAndInit(JSRuntime* rt, size_t size = ChunkSize);
  void poisonRange(size_t from, size_t size, uint8_t value,
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
static_assert(sizeof(js::NurseryChunk) == gc::ChunkSize,
              "Nursery chunk size must match gc::Chunk size.");

class NurseryDecommitTask : public GCParallelTask {
 public:
  explicit NurseryDecommitTask(gc::GCRuntime* gc);
  bool reserveSpaceForBytes(size_t nbytes);

  bool isEmpty(const AutoLockHelperThreadState& lock) const;

  void queueChunk(NurseryChunk* chunk, const AutoLockHelperThreadState& lock);
  void queueRange(size_t newCapacity, NurseryChunk& chunk,
                  const AutoLockHelperThreadState& lock);

 private:
  using NurseryChunkVector = Vector<NurseryChunk*, 0, SystemAllocPolicy>;

  void run(AutoLockHelperThreadState& lock) override;

  NurseryChunkVector& chunksToDecommit() { return chunksToDecommit_.ref(); }
  const NurseryChunkVector& chunksToDecommit() const {
    return chunksToDecommit_.ref();
  }

  MainThreadOrGCTaskData<NurseryChunkVector> chunksToDecommit_;

  MainThreadOrGCTaskData<NurseryChunk*> partialChunk;
  MainThreadOrGCTaskData<size_t> partialCapacity;
};

}  // namespace js

inline void js::NurseryChunk::poisonAndInit(JSRuntime* rt, size_t size) {
  MOZ_ASSERT(size >= sizeof(ChunkBase));
  MOZ_ASSERT(size <= ChunkSize);
  poisonRange(0, size, JS_FRESH_NURSERY_PATTERN, MemCheckKind::MakeUndefined);
  new (this) NurseryChunk(rt);
}

inline void js::NurseryChunk::poisonRange(size_t from, size_t size,
                                          uint8_t value,
                                          MemCheckKind checkKind) {
  MOZ_ASSERT(from + size <= ChunkSize);

  auto* start = reinterpret_cast<uint8_t*>(this) + from;

  // We can poison the same chunk more than once, so first make sure memory
  // sanitizers will let us poison it.
  MOZ_MAKE_MEM_UNDEFINED(start, size);
  Poison(start, value, size, checkKind);
}

inline void js::NurseryChunk::poisonAfterEvict(size_t extent) {
  MOZ_ASSERT(extent <= ChunkSize);
  poisonRange(sizeof(ChunkBase), extent - sizeof(ChunkBase),
              JS_SWEPT_NURSERY_PATTERN, MemCheckKind::MakeNoAccess);
}

inline void js::NurseryChunk::markPagesUnusedHard(size_t startOffset) {
  MOZ_ASSERT(startOffset >= sizeof(ChunkBase));  // Don't touch the header.
  MOZ_ASSERT(startOffset >= SystemPageSize());
  MOZ_ASSERT(startOffset <= ChunkSize);
  uintptr_t start = uintptr_t(this) + startOffset;
  size_t length = ChunkSize - startOffset;
  MarkPagesUnusedHard(reinterpret_cast<void*>(start), length);
}

inline bool js::NurseryChunk::markPagesInUseHard(size_t endOffset) {
  MOZ_ASSERT(endOffset >= sizeof(ChunkBase));
  MOZ_ASSERT(endOffset >= SystemPageSize());
  MOZ_ASSERT(endOffset <= ChunkSize);
  uintptr_t start = uintptr_t(this) + SystemPageSize();
  size_t length = endOffset - SystemPageSize();
  return MarkPagesInUseHard(reinterpret_cast<void*>(start), length);
}

// static
inline js::NurseryChunk* js::NurseryChunk::fromChunk(TenuredChunk* chunk) {
  return reinterpret_cast<NurseryChunk*>(chunk);
}

js::NurseryDecommitTask::NurseryDecommitTask(gc::GCRuntime* gc)
    : GCParallelTask(gc, gcstats::PhaseKind::NONE) {
  // This can occur outside GCs so doesn't have a stats phase.
}

bool js::NurseryDecommitTask::isEmpty(
    const AutoLockHelperThreadState& lock) const {
  return chunksToDecommit().empty() && !partialChunk;
}

bool js::NurseryDecommitTask::reserveSpaceForBytes(size_t nbytes) {
  MOZ_ASSERT(isIdle());
  size_t nchunks = HowMany(nbytes, ChunkSize);
  return chunksToDecommit().reserve(nchunks);
}

void js::NurseryDecommitTask::queueChunk(
    NurseryChunk* chunk, const AutoLockHelperThreadState& lock) {
  MOZ_ASSERT(isIdle(lock));
  MOZ_ALWAYS_TRUE(chunksToDecommit().append(chunk));
}

void js::NurseryDecommitTask::queueRange(
    size_t newCapacity, NurseryChunk& newChunk,
    const AutoLockHelperThreadState& lock) {
  MOZ_ASSERT(isIdle(lock));
  MOZ_ASSERT(!partialChunk);
  MOZ_ASSERT(newCapacity < ChunkSize);
  MOZ_ASSERT(newCapacity % SystemPageSize() == 0);

  partialChunk = &newChunk;
  partialCapacity = newCapacity;
}

void js::NurseryDecommitTask::run(AutoLockHelperThreadState& lock) {
  while (!chunksToDecommit().empty()) {
    NurseryChunk* nurseryChunk = chunksToDecommit().popCopy();
    AutoUnlockHelperThreadState unlock(lock);
    nurseryChunk->~NurseryChunk();
    TenuredChunk* tenuredChunk = TenuredChunk::emplace(
        nurseryChunk, gc, /* allMemoryCommitted = */ false);
    AutoLockGC lock(gc);
    gc->recycleChunk(tenuredChunk, lock);
  }

  if (partialChunk) {
    {
      AutoUnlockHelperThreadState unlock(lock);
      partialChunk->markPagesUnusedHard(partialCapacity);
    }
    partialChunk = nullptr;
    partialCapacity = 0;
  }
}

js::Nursery::Nursery(GCRuntime* gc)
    : position_(0),
      currentEnd_(0),
      gc(gc),
      currentChunk_(0),
      currentStartChunk_(0),
      currentStartPosition_(0),
      capacity_(0),
      timeInChunkAlloc_(0),
      enableProfiling_(false),
      profileThreshold_(0),
      canAllocateStrings_(true),
      canAllocateBigInts_(true),
      reportDeduplications_(false),
      reportPretenuring_(false),
      reportPretenuringThreshold_(0),
      minorGCTriggerReason_(JS::GCReason::NO_REASON),
      hasRecentGrowthData(false),
      smoothedTargetSize(0.0) {
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
                                   bool* enabled, size_t* threshold) {
  *enabled = false;
  *threshold = 0;

  const char* env = GetEnvVar(name, helpMessage);
  if (!env) {
    return;
  }

  char* end;
  *threshold = strtol(env, &end, 10);
  if (end == env || *end) {
    PrintAndExit(helpMessage);
  }

  *enabled = true;
}

bool js::Nursery::init(AutoLockGCBgAlloc& lock) {
  ReadProfileEnv("JS_GC_PROFILE_NURSERY",
                 "Report minor GCs taking at least N microseconds.\n",
                 &enableProfiling_, &profileWorkers_, &profileThreshold_);

  reportDeduplications_ = GetBoolEnvVar(
      "JS_GC_REPORT_STATS",
      "JS_GC_REPORT_STATS=1\n"
      "\tAfter a minor GC, report how many strings were deduplicated.\n");

  ReadReportPretenureEnv(
      "JS_GC_REPORT_PRETENURE",
      "JS_GC_REPORT_PRETENURE=N\n"
      "\tAfter a minor GC, report information about pretenuring, including\n"
      "\tallocation sites with at least N allocations.\n",
      &reportPretenuring_, &reportPretenuringThreshold_);

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
  MOZ_ASSERT(isEmpty());
  MOZ_ASSERT(!gc->isVerifyPreBarriersEnabled());
  if (isEnabled()) {
    return;
  }

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

  capacity_ = tunables().gcMinNurseryBytes();

  if (!decommitTask->reserveSpaceForBytes(capacity_) ||
      !allocateNextChunk(0, lock)) {
    capacity_ = 0;
    return false;
  }

  setCurrentChunk(0);
  setStartPosition();
  poisonAndInitCurrentChunk();

  // Clear any information about previous collections.
  clearRecentGrowthData();

  return true;
}

void js::Nursery::disable() {
  MOZ_ASSERT(isEmpty());
  if (!isEnabled()) {
    return;
  }

  // Free all chunks.
  decommitTask->join();
  freeChunksFrom(0);
  decommitTask->runFromMainThread();

  capacity_ = 0;

  // We must reset currentEnd_ so that there is no space for anything in the
  // nursery. JIT'd code uses this even if the nursery is disabled.
  currentEnd_ = 0;
  position_ = 0;
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

  for (RealmsInZoneIter r(zone); !r.done(); r.next()) {
    if (jit::JitRealm* jitRealm = r->jitRealm()) {
      jitRealm->discardStubs();
      jitRealm->setStringsCanBeInNursery(zone->allocNurseryStrings());
    }
  }
}

bool js::Nursery::isEmpty() const {
  if (!isEnabled()) {
    return true;
  }

  if (!gc->hasZealMode(ZealMode::GenerationalGC)) {
    MOZ_ASSERT(currentStartChunk_ == 0);
    MOZ_ASSERT(currentStartPosition_ == chunk(0).start());
  }
  return position() == currentStartPosition_;
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
    chunk(0).poisonRange(capacity_, ChunkSize - capacity_,
                         JS_FRESH_NURSERY_PATTERN, MemCheckKind::MakeUndefined);
  }

  capacity_ = RoundUp(tunables().gcMaxNurseryBytes(), ChunkSize);

  if (!decommitTask->reserveSpaceForBytes(capacity_)) {
    oomUnsafe.crash("Nursery::enterZealMode");
  }

  setCurrentEnd();
}

void js::Nursery::leaveZealMode() {
  if (!isEnabled()) {
    return;
  }

  MOZ_ASSERT(isEmpty());

  setCurrentChunk(0);
  setStartPosition();
  poisonAndInitCurrentChunk();
}
#endif  // JS_GC_ZEAL

void* js::Nursery::allocateCell(gc::AllocSite* site, size_t size,
                                JS::TraceKind kind) {
  // Ensure there's enough space to replace the contents with a
  // RelocationOverlay.
  MOZ_ASSERT(size >= sizeof(RelocationOverlay));
  MOZ_ASSERT(size % CellAlignBytes == 0);
  MOZ_ASSERT(size_t(kind) < NurseryTraceKinds);
  MOZ_ASSERT_IF(kind == JS::TraceKind::String, canAllocateStrings());
  MOZ_ASSERT_IF(kind == JS::TraceKind::BigInt, canAllocateBigInts());

  void* ptr = allocate(sizeof(NurseryCellHeader) + size);
  if (!ptr) {
    return nullptr;
  }

  new (ptr) NurseryCellHeader(site, kind);

  void* cell =
      reinterpret_cast<void*>(uintptr_t(ptr) + sizeof(NurseryCellHeader));

  // Update the allocation site. This code is also inlined in
  // MacroAssembler::updateAllocSite.
  uint32_t allocCount = site->incAllocCount();
  if (allocCount == 1) {
    pretenuringNursery.insertIntoAllocatedList(site);
  } else {
    MOZ_ASSERT_IF(site->isNormal(), site->isInAllocatedList());
  }

  gcprobes::NurseryAlloc(cell, kind);
  return cell;
}

inline void* js::Nursery::allocate(size_t size) {
  MOZ_ASSERT(isEnabled());
  MOZ_ASSERT(!JS::RuntimeHeapIsBusy());
  MOZ_ASSERT(CurrentThreadCanAccessRuntime(runtime()));
  MOZ_ASSERT_IF(currentChunk_ == currentStartChunk_,
                position() >= currentStartPosition_);
  MOZ_ASSERT(position() % CellAlignBytes == 0);
  MOZ_ASSERT(size % CellAlignBytes == 0);

  if (MOZ_UNLIKELY(currentEnd() < position() + size)) {
    return moveToNextChunkAndAllocate(size);
  }

  void* thing = (void*)position();
  position_ = position() + size;

  DebugOnlyPoison(thing, JS_ALLOCATED_NURSERY_PATTERN, size,
                  MemCheckKind::MakeUndefined);

  return thing;
}

void* Nursery::moveToNextChunkAndAllocate(size_t size) {
  MOZ_ASSERT(currentEnd() < position() + size);

  unsigned chunkno = currentChunk_ + 1;
  MOZ_ASSERT(chunkno <= maxChunkCount());
  MOZ_ASSERT(chunkno <= allocatedChunkCount());
  if (chunkno == maxChunkCount()) {
    return nullptr;
  }
  if (chunkno == allocatedChunkCount()) {
    TimeStamp start = TimeStamp::Now();
    {
      AutoLockGCBgAlloc lock(gc);
      if (!allocateNextChunk(chunkno, lock)) {
        return nullptr;
      }
    }
    timeInChunkAlloc_ += TimeStamp::Now() - start;
    MOZ_ASSERT(chunkno < allocatedChunkCount());
  }
  setCurrentChunk(chunkno);
  poisonAndInitCurrentChunk();

  // We know there's enough space to allocate now so we can call allocate()
  // recursively.
  MOZ_ASSERT(currentEnd() >= position() + size);
  return allocate(size);
}
void* js::Nursery::allocateBuffer(Zone* zone, size_t nbytes) {
  MOZ_ASSERT(nbytes > 0);

  if (nbytes <= MaxNurseryBufferSize) {
    void* buffer = allocate(nbytes);
    if (buffer) {
      return buffer;
    }
  }

  void* buffer = zone->pod_malloc<uint8_t>(nbytes);
  if (buffer && !registerMallocedBuffer(buffer, nbytes)) {
    js_free(buffer);
    return nullptr;
  }
  return buffer;
}

void* js::Nursery::allocateBuffer(Zone* zone, JSObject* obj, size_t nbytes) {
  MOZ_ASSERT(obj);
  MOZ_ASSERT(nbytes > 0);

  if (!IsInsideNursery(obj)) {
    return zone->pod_malloc<uint8_t>(nbytes);
  }

  return allocateBuffer(zone, nbytes);
}

void* js::Nursery::allocateBufferSameLocation(JSObject* obj, size_t nbytes) {
  MOZ_ASSERT(obj);
  MOZ_ASSERT(nbytes > 0);
  MOZ_ASSERT(nbytes <= MaxNurseryBufferSize);

  if (!IsInsideNursery(obj)) {
    return obj->zone()->pod_malloc<uint8_t>(nbytes);
  }

  return allocate(nbytes);
}

void* js::Nursery::allocateZeroedBuffer(
    Zone* zone, size_t nbytes, arena_id_t arena /*= js::MallocArena*/) {
  MOZ_ASSERT(nbytes > 0);

  if (nbytes <= MaxNurseryBufferSize) {
    void* buffer = allocate(nbytes);
    if (buffer) {
      memset(buffer, 0, nbytes);
      return buffer;
    }
  }

  void* buffer = zone->pod_arena_calloc<uint8_t>(arena, nbytes);
  if (buffer && !registerMallocedBuffer(buffer, nbytes)) {
    js_free(buffer);
    return nullptr;
  }
  return buffer;
}

void* js::Nursery::allocateZeroedBuffer(
    JSObject* obj, size_t nbytes, arena_id_t arena /*= js::MallocArena*/) {
  MOZ_ASSERT(obj);
  MOZ_ASSERT(nbytes > 0);

  if (!IsInsideNursery(obj)) {
    return obj->zone()->pod_arena_calloc<uint8_t>(arena, nbytes);
  }
  return allocateZeroedBuffer(obj->zone(), nbytes, arena);
}

void* js::Nursery::reallocateBuffer(Zone* zone, Cell* cell, void* oldBuffer,
                                    size_t oldBytes, size_t newBytes) {
  if (!IsInsideNursery(cell)) {
    MOZ_ASSERT(!isInside(oldBuffer));
    return zone->pod_realloc<uint8_t>((uint8_t*)oldBuffer, oldBytes, newBytes);
  }

  if (!isInside(oldBuffer)) {
    MOZ_ASSERT(mallocedBufferBytes >= oldBytes);
    void* newBuffer =
        zone->pod_realloc<uint8_t>((uint8_t*)oldBuffer, oldBytes, newBytes);
    if (newBuffer) {
      if (oldBuffer != newBuffer) {
        MOZ_ALWAYS_TRUE(
            mallocedBuffers.rekeyAs(oldBuffer, newBuffer, newBuffer));
      }
      mallocedBufferBytes -= oldBytes;
      mallocedBufferBytes += newBytes;
    }
    return newBuffer;
  }

  // The nursery cannot make use of the returned slots data.
  if (newBytes < oldBytes) {
    return oldBuffer;
  }

  void* newBuffer = allocateBuffer(zone, newBytes);
  if (newBuffer) {
    PodCopy((uint8_t*)newBuffer, (uint8_t*)oldBuffer, oldBytes);
  }
  return newBuffer;
}

void* js::Nursery::allocateBuffer(JS::BigInt* bi, size_t nbytes) {
  MOZ_ASSERT(bi);
  MOZ_ASSERT(nbytes > 0);

  if (!IsInsideNursery(bi)) {
    return bi->zone()->pod_malloc<uint8_t>(nbytes);
  }
  return allocateBuffer(bi->zone(), nbytes);
}

void js::Nursery::freeBuffer(void* buffer, size_t nbytes) {
  if (!isInside(buffer)) {
    removeMallocedBuffer(buffer, nbytes);
    js_free(buffer);
  }
}

#ifdef DEBUG
/* static */
inline bool Nursery::checkForwardingPointerLocation(void* ptr,
                                                    bool expectedInside) {
  if (isInside(ptr) == expectedInside) {
    return true;
  }

  // If a zero-capacity elements header lands right at the end of a chunk then
  // elements data will appear to be in the next chunk. If we have a pointer to
  // the very start of a chunk, check the previous chunk.
  if ((uintptr_t(ptr) & ChunkMask) == 0 &&
      isInside(reinterpret_cast<uint8_t*>(ptr) - 1) == expectedInside) {
    return true;
  }

  return false;
}
#endif

void Nursery::setIndirectForwardingPointer(void* oldData, void* newData) {
  MOZ_ASSERT(checkForwardingPointerLocation(oldData, true));
  MOZ_ASSERT(checkForwardingPointerLocation(newData, false));

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

  MOZ_ASSERT(!isInside(buffer));
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

  json.beginObject();

  json.property("status", "complete");

  json.property("reason", JS::ExplainGCReason(previousGC.reason));
  json.property("bytes_tenured", previousGC.tenuredBytes);
  json.property("cells_tenured", previousGC.tenuredCells);
  json.property("strings_tenured",
                stats().getStat(gcstats::STAT_STRINGS_TENURED));
  json.property("strings_deduplicated",
                stats().getStat(gcstats::STAT_STRINGS_DEDUPLICATED));
  json.property("bigints_tenured",
                stats().getStat(gcstats::STAT_BIGINTS_TENURED));
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
  if (!sprinter.init() || !sprinter.put(gcstats::MinorGCProfilePrefix)) {
    return;
  }

  size_t pid = getpid();
  JSRuntime* runtime = gc->rt;
  TimeDuration timestamp = collectionStartTime() - stats().creationTime();
  const char* reasonStr = ExplainGCReason(reason);
  double promotionRatePercent = promotionRate * 100;
  size_t oldSizeKB = previousGC.nurseryCapacity / 1024;
  size_t newSizeKB = capacity() / 1024;
  size_t dedupCount = stats().getStat(gcstats::STAT_STRINGS_DEDUPLICATED);

#define PRINT_FIELD_VALUE(_1, _2, format, value) \
  if (!sprinter.jsprintf(" " format, value)) {   \
    return;                                      \
  }
  FOR_EACH_NURSERY_PROFILE_METADATA(PRINT_FIELD_VALUE)
#undef PRINT_FIELD_VALUE

  printProfileDurations(profileDurations_, sprinter);

  fputs(sprinter.string(), stats().profileFile());
}

void js::Nursery::printProfileHeader() {
  Sprinter sprinter;
  if (!sprinter.init() || !sprinter.put(gcstats::MinorGCProfilePrefix)) {
    return;
  }

#define PRINT_FIELD_NAME(name, width, _1, _2)     \
  if (!sprinter.jsprintf(" %-*s", width, name)) { \
    return;                                       \
  }
  FOR_EACH_NURSERY_PROFILE_METADATA(PRINT_FIELD_NAME)
#undef PRINT_FIELD_NAME

#define PRINT_PROFILE_NAME(_1, text)         \
  if (!sprinter.jsprintf(" %-6.6s", text)) { \
    return;                                  \
  }
  FOR_EACH_NURSERY_PROFILE_TIME(PRINT_PROFILE_NAME)
#undef PRINT_PROFILE_NAME

  if (!sprinter.put("\n")) {
    return;
  }

  fputs(sprinter.string(), stats().profileFile());
}

// static
bool js::Nursery::printProfileDurations(const ProfileDurations& times,
                                        Sprinter& sprinter) {
  for (auto time : times) {
    int64_t micros = int64_t(time.ToMicroseconds());
    if (!sprinter.jsprintf(" %6" PRIi64, micros)) {
      return false;
    }
  }

  return sprinter.put("\n");
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
  if (!sprinter.init() || !sprinter.put(gcstats::MinorGCProfilePrefix)) {
    return;
  }

  size_t pid = getpid();
  JSRuntime* runtime = gc->rt;

  char collections[32];
  DebugOnly<int> r = SprintfLiteral(
      collections, "TOTALS: %7" PRIu64 " collections:", gc->minorGCCount());
  MOZ_ASSERT(r > 0 && r < int(sizeof(collections)));

#define PRINT_FIELD_VALUE(_1, _2, format, value) \
  if (!sprinter.jsprintf(" " format, value)) {   \
    return;                                      \
  }
  FOR_EACH_NURSERY_PROFILE_COMMON_METADATA(PRINT_FIELD_VALUE)
#undef PRINT_FIELD_VALUE

  // Use whole width of per-slice metadata to print total slices so the profile
  // totals that follow line up.
  size_t width = NurserySliceMetadataFormatWidth();
  if (!sprinter.jsprintf(" %-*s", int(width), collections)) {
    return;
  }

  if (!printProfileDurations(totalDurations_, sprinter)) {
    return;
  }

  fputs(sprinter.string(), stats().profileFile());
}

void js::Nursery::maybeClearProfileDurations() {
  for (auto& duration : profileDurations_) {
    duration = mozilla::TimeDuration();
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

inline TimeStamp js::Nursery::lastCollectionEndTime() const {
  return previousGC.endTime;
}

bool js::Nursery::shouldCollect() const {
  if (!isEnabled()) {
    return false;
  }

  if (isEmpty() && capacity() == tunables().gcMinNurseryBytes()) {
    return false;
  }

  if (minorGCRequested()) {
    return true;
  }

  // Eagerly collect the nursery in idle time if it's nearly full.
  if (isNearlyFull()) {
    return true;
  }

  // If the nursery is not being collected often then it may be taking up more
  // space than necessary.
  return isUnderused();
}

inline bool js::Nursery::isNearlyFull() const {
  bool belowBytesThreshold =
      freeSpace() < tunables().nurseryFreeThresholdForIdleCollection();
  bool belowFractionThreshold =
      double(freeSpace()) / double(capacity()) <
      tunables().nurseryFreeThresholdForIdleCollectionFraction();

  // We want to use belowBytesThreshold when the nursery is sufficiently large,
  // and belowFractionThreshold when it's small.
  //
  // When the nursery is small then belowBytesThreshold is a lower threshold
  // (triggered earlier) than belowFractionThreshold. So if the fraction
  // threshold is true, the bytes one will be true also. The opposite is true
  // when the nursery is large.
  //
  // Therefore, by the time we cross the threshold we care about, we've already
  // crossed the other one, and we can boolean AND to use either condition
  // without encoding any "is the nursery big/small" test/threshold. The point
  // at which they cross is when the nursery is: BytesThreshold /
  // FractionThreshold large.
  //
  // With defaults that's:
  //
  //   1MB = 256KB / 0.25
  //
  return belowBytesThreshold && belowFractionThreshold;
}

inline bool js::Nursery::isUnderused() const {
  if (js::SupportDifferentialTesting() || !previousGC.endTime) {
    return false;
  }

  if (capacity() == tunables().gcMinNurseryBytes()) {
    return false;
  }

  // If the nursery is above its minimum size, collect it every so often if we
  // have idle time. This allows the nursery to shrink when it's not being
  // used. There are other heuristics we could use for this, but this is the
  // simplest.
  TimeDuration timeSinceLastCollection = TimeStamp::Now() - previousGC.endTime;
  return timeSinceLastCollection > tunables().nurseryTimeoutForIdleCollection();
}

// typeReason is the gcReason for specified type, for example,
// FULL_CELL_PTR_OBJ_BUFFER is the gcReason for JSObject.
static inline bool IsFullStoreBufferReason(JS::GCReason reason,
                                           JS::GCReason typeReason) {
  return reason == typeReason ||
         reason == JS::GCReason::FULL_WHOLE_CELL_BUFFER ||
         reason == JS::GCReason::FULL_GENERIC_BUFFER ||
         reason == JS::GCReason::FULL_VALUE_BUFFER ||
         reason == JS::GCReason::FULL_SLOT_BUFFER ||
         reason == JS::GCReason::FULL_SHAPE_BUFFER;
}

void js::Nursery::collect(JS::GCOptions options, JS::GCReason reason) {
  JSRuntime* rt = runtime();
  MOZ_ASSERT(!rt->mainContextFromOwnThread()->suppressGC);

  if (!isEnabled() || isEmpty()) {
    // Our barriers are not always exact, and there may be entries in the
    // storebuffer even when the nursery is disabled or empty. It's not safe
    // to keep these entries as they may refer to tenured cells which may be
    // freed after this point.
    gc->storeBuffer().clear();

    MOZ_ASSERT(!pretenuringNursery.hasAllocatedSites());
  }

  if (!isEnabled()) {
    return;
  }

  AutoGCSession session(gc, JS::HeapState::MinorCollecting);

  stats().beginNurseryCollection(reason);
  gcprobes::MinorGCStart();

  maybeClearProfileDurations();
  startProfile(ProfileKey::Total);

  previousGC.reason = JS::GCReason::NO_REASON;
  previousGC.nurseryUsedBytes = usedSpace();
  previousGC.nurseryCapacity = capacity();
  previousGC.nurseryCommitted = committed();
  previousGC.nurseryUsedChunkCount = currentChunk_ + 1;
  previousGC.tenuredBytes = 0;
  previousGC.tenuredCells = 0;

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
                (sizeof(ChunkBase) * previousGC.nurseryUsedChunkCount)));

    previousGC.reason = reason;
    previousGC.tenuredBytes = result.tenuredBytes;
    previousGC.tenuredCells = result.tenuredCells;
    previousGC.nurseryUsedChunkCount = currentChunk_ + 1;
  }

  // Resize the nursery.
  maybeResizeNursery(options, reason);

  // Poison/initialise the first chunk.
  if (previousGC.nurseryUsedBytes) {
    // In most cases Nursery::clear() has not poisoned this chunk or marked it
    // as NoAccess; so we only need to poison the region used during the last
    // cycle.  Also, if the heap was recently expanded we don't want to
    // re-poison the new memory.  In both cases we only need to poison until
    // previousGC.nurseryUsedBytes.
    //
    // In cases where this is not true, like generational zeal mode or subchunk
    // mode, poisonAndInitCurrentChunk() will ignore its parameter.  It will
    // also clamp the parameter.
    poisonAndInitCurrentChunk(previousGC.nurseryUsedBytes);
  }

  bool validPromotionRate;
  const double promotionRate = calcPromotionRate(&validPromotionRate);

  startProfile(ProfileKey::Pretenure);
  size_t sitesPretenured = 0;
  if (!wasEmpty) {
    sitesPretenured =
        doPretenuring(rt, reason, validPromotionRate, promotionRate);
  }
  endProfile(ProfileKey::Pretenure);

  // We ignore gcMaxBytes when allocating for minor collection. However, if we
  // overflowed, we disable the nursery. The next time we allocate, we'll fail
  // because bytes >= gcMaxBytes.
  if (gc->heapSize.bytes() >= tunables().gcMaxBytes()) {
    disable();
  }

  previousGC.endTime =
      TimeStamp::Now();  // Must happen after maybeResizeNursery.
  endProfile(ProfileKey::Total);
  gc->incMinorGcNumber();

  TimeDuration totalTime = profileDurations_[ProfileKey::Total];
  sendTelemetry(reason, totalTime, wasEmpty, promotionRate, sitesPretenured);

  stats().endNurseryCollection(reason);  // Calls GCNurseryCollectionCallback.
  gcprobes::MinorGCEnd();

  timeInChunkAlloc_ = mozilla::TimeDuration();

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
  rt->metrics().GC_NURSERY_BYTES_2(committed());

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

void js::Nursery::freeTrailerBlocks(void) {
  // This routine frees those blocks denoted by the set
  //
  //  trailersAdded_ (all of it)
  //    - trailersRemoved_ (entries with index below trailersRemovedUsed_)
  //
  // For each block, places it back on the nursery's small-malloced-block pool
  // by calling mallocedBlockCache_.free.

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
        mallocedBlockCache_.free(block);
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
        mallocedBlockCache_.free(blockAdded);
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
      mallocedBlockCache_.free(block);
    }
  }

  // And empty out both sets, but preserve the underlying storage.
  trailersAdded_.clear();
  trailersRemoved_.clear();
  trailersRemovedUsed_ = 0;
  trailerBytes_ = 0;

  // Discard blocks from the cache at 0.05% per megabyte of nursery capacity,
  // that is, 0.8% of blocks for a 16-megabyte nursery.  This allows the cache
  // to gradually discard unneeded blocks in long running applications.
  mallocedBlockCache_.preen(0.05 * float(capacity() / (1024 * 1024)));
}

size_t Nursery::sizeOfTrailerBlockSets(
    mozilla::MallocSizeOf mallocSizeOf) const {
  return trailersAdded_.sizeOfExcludingThis(mallocSizeOf) +
         trailersRemoved_.sizeOfExcludingThis(mallocSizeOf);
}

js::Nursery::CollectionResult js::Nursery::doCollection(AutoGCSession& session,
                                                        JS::GCOptions options,
                                                        JS::GCReason reason) {
  JSRuntime* rt = runtime();
  AutoSetThreadIsPerformingGC performingGC(rt->gcContext());
  AutoStopVerifyingBarriers av(rt, false);
  AutoDisableProxyCheck disableStrictProxyChecking;
  mozilla::DebugOnly<AutoEnterOOMUnsafeRegion> oomUnsafeRegion;

  // Move objects pointed to by roots from the nursery to the major heap.
  TenuringTracer mover(rt, this);

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

  // Sweep.
  startProfile(ProfileKey::FreeMallocedBuffers);
  gc->queueBuffersForFreeAfterMinorGC(mallocedBuffers);
  mallocedBufferBytes = 0;
  endProfile(ProfileKey::FreeMallocedBuffers);

  // Give trailer blocks associated with non-tenured Wasm{Struct,Array}Objects
  // back to our `mallocedBlockCache_`.
  startProfile(ProfileKey::FreeTrailerBlocks);
  freeTrailerBlocks();
  if (options == JS::GCOptions::Shrink || gc::IsOOMReason(reason)) {
    mallocedBlockCache_.clear();
  }
  endProfile(ProfileKey::FreeTrailerBlocks);

  startProfile(ProfileKey::ClearNursery);
  clear();
  endProfile(ProfileKey::ClearNursery);

  startProfile(ProfileKey::ClearStoreBuffer);
  gc->storeBuffer().clear();
  endProfile(ProfileKey::ClearStoreBuffer);

  // Purge the StringToAtomCache. This has to happen at the end because the
  // cache is used when tenuring strings.
  startProfile(ProfileKey::PurgeStringToAtomCache);
  runtime()->caches().stringToAtomCache.purge();
  endProfile(ProfileKey::PurgeStringToAtomCache);

  // Make sure hashtables have been updated after the collection.
  startProfile(ProfileKey::CheckHashTables);
#ifdef JS_GC_ZEAL
  if (gc->hasZealMode(ZealMode::CheckHashTablesOnMinorGC)) {
    runtime()->caches().checkEvalCacheAfterMinorGC();
    gc->checkHashTablesAfterMovingGC();
  }
#endif
  endProfile(ProfileKey::CheckHashTables);

  return {mover.getTenuredSize(), mover.getTenuredCells()};
}

void js::Nursery::traceRoots(AutoGCSession& session, TenuringTracer& mover) {
  {
    // Suppress the sampling profiler to prevent it observing moved functions.
    AutoSuppressProfilerSampling suppressProfiler(
        runtime()->mainContextFromOwnThread());

    // Trace the store buffer. This must happen first.
    StoreBuffer& sb = gc->storeBuffer();

    // Strings in the whole cell buffer must be traced first, in order to mark
    // tenured dependent strings' bases as non-deduplicatable. The rest of
    // nursery collection (whole non-string cells, edges, etc.) can happen
    // later.
    startProfile(ProfileKey::TraceWholeCells);
    sb.traceWholeCells(mover);
    endProfile(ProfileKey::TraceWholeCells);

    startProfile(ProfileKey::TraceValues);
    sb.traceValues(mover);
    endProfile(ProfileKey::TraceValues);

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

size_t js::Nursery::doPretenuring(JSRuntime* rt, JS::GCReason reason,
                                  bool validPromotionRate,
                                  double promotionRate) {
  size_t sitesPretenured = pretenuringNursery.doPretenuring(
      gc, reason, validPromotionRate, promotionRate, reportPretenuring_,
      reportPretenuringThreshold_);

  bool highPromotionRate =
      validPromotionRate && promotionRate > tunables().pretenureThreshold();

  bool pretenureStr = false;
  bool pretenureBigInt = false;
  if (tunables().attemptPretenuring()) {
    // Should we check for pretenuring regardless of GCReason?
    // Use 3MB as the threshold so the pretenuring can be applied on Android.
    bool pretenureAll =
        highPromotionRate && previousGC.nurseryUsedBytes >= 3 * 1024 * 1024;

    pretenureStr =
        pretenureAll ||
        IsFullStoreBufferReason(reason, JS::GCReason::FULL_CELL_PTR_STR_BUFFER);
    pretenureBigInt =
        pretenureAll || IsFullStoreBufferReason(
                            reason, JS::GCReason::FULL_CELL_PTR_BIGINT_BUFFER);
  }

  uint32_t numStringsTenured = 0;
  uint32_t numBigIntsTenured = 0;
  for (ZonesIter zone(gc, SkipAtoms); !zone.done(); zone.next()) {
    // For some tests in JetStream2 and Kraken, the tenuredRate is high but the
    // number of allocated strings is low. So we calculate the tenuredRate only
    // if the number of string allocations is enough.
    uint32_t zoneNurseryStrings =
        zone->nurseryAllocCount(JS::TraceKind::String);
    bool allocThreshold = zoneNurseryStrings > 30000;
    uint64_t zoneTenuredStrings =
        zone->stringStats.ref().liveNurseryStrings -
        zone->previousGCStringStats.ref().liveNurseryStrings;
    double tenuredRate =
        allocThreshold ? double(zoneTenuredStrings) / double(zoneNurseryStrings)
                       : 0.0;
    bool disableNurseryStrings =
        pretenureStr && zone->allocNurseryStrings() &&
        tenuredRate > tunables().pretenureStringThreshold();
    bool disableNurseryBigInts = pretenureBigInt &&
                                 zone->allocNurseryBigInts() &&
                                 zone->tenuredBigInts >= 30 * 1000;
    if (disableNurseryStrings || disableNurseryBigInts) {
      if (disableNurseryStrings) {
        zone->nurseryStringsDisabled = true;
      }
      if (disableNurseryBigInts) {
        zone->nurseryBigIntsDisabled = true;
      }
      updateAllocFlagsForZone(zone);
    }
    numStringsTenured += zoneTenuredStrings;
    numBigIntsTenured += zone->tenuredBigInts;
    zone->tenuredBigInts = 0;
  }
  stats().setStat(gcstats::STAT_STRINGS_TENURED, numStringsTenured);
  stats().setStat(gcstats::STAT_BIGINTS_TENURED, numBigIntsTenured);

  return sitesPretenured;
}

bool js::Nursery::registerMallocedBuffer(void* buffer, size_t nbytes) {
  MOZ_ASSERT(buffer);
  MOZ_ASSERT(nbytes > 0);
  if (!mallocedBuffers.putNew(buffer)) {
    return false;
  }

  mallocedBufferBytes += nbytes;
  if (MOZ_UNLIKELY(mallocedBufferBytes > capacity() * 8)) {
    requestMinorGC(JS::GCReason::NURSERY_MALLOC_BUFFERS);
  }

  return true;
}

size_t Nursery::sizeOfMallocedBuffers(
    mozilla::MallocSizeOf mallocSizeOf) const {
  size_t total = 0;
  for (BufferSet::Range r = mallocedBuffers.all(); !r.empty(); r.popFront()) {
    total += mallocSizeOf(r.front());
  }
  total += mallocedBuffers.shallowSizeOfExcludingThis(mallocSizeOf);
  return total;
}

void js::Nursery::sweep() {
  // It's important that the context's GCUse is not Finalizing at this point,
  // otherwise we will miscount memory attached to nursery objects with
  // CellAllocPolicy.
  AutoSetThreadIsSweeping setThreadSweeping(runtime()->gcContext());

  MinorSweepingTracer trc(runtime());

  // Sweep unique IDs first before we sweep any tables that may be keyed based
  // on them.
  for (Cell* cell : cellsWithUid_) {
    auto* obj = static_cast<JSObject*>(cell);
    if (!IsForwarded(obj)) {
      gc::RemoveUniqueId(obj);
    } else {
      JSObject* dst = Forwarded(obj);
      gc::TransferUniqueId(dst, obj);
    }
  }
  cellsWithUid_.clear();

  for (ZonesIter zone(runtime(), SkipAtoms); !zone.done(); zone.next()) {
    zone->sweepAfterMinorGC(&trc);
  }

  sweepMapAndSetObjects();

  runtime()->caches().sweepAfterMinorGC(&trc);
}

void js::Nursery::clear() {
  // Poison the nursery contents so touching a freed object will crash.
  unsigned firstClearChunk;
  if (gc->hasZealMode(ZealMode::GenerationalGC)) {
    // Poison all the chunks used in this cycle. The new start chunk is
    // reposioned in Nursery::collect() but there's no point optimising that in
    // this case.
    firstClearChunk = currentStartChunk_;
  } else {
    // In normal mode we start at the second chunk, the first one will be used
    // in the next cycle and poisoned in Nusery::collect();
    MOZ_ASSERT(currentStartChunk_ == 0);
    firstClearChunk = 1;
  }
  for (unsigned i = firstClearChunk; i < currentChunk_; ++i) {
    chunk(i).poisonAfterEvict();
  }
  // Clear only the used part of the chunk because that's the part we touched,
  // but only if it's not going to be re-used immediately (>= firstClearChunk).
  if (currentChunk_ >= firstClearChunk) {
    chunk(currentChunk_)
        .poisonAfterEvict(position() - chunk(currentChunk_).start());
  }

  // Reset the start chunk & position if we're not in this zeal mode, or we're
  // in it and close to the end of the nursery.
  MOZ_ASSERT(maxChunkCount() > 0);
  if (!gc->hasZealMode(ZealMode::GenerationalGC) ||
      (gc->hasZealMode(ZealMode::GenerationalGC) &&
       currentChunk_ + 1 == maxChunkCount())) {
    setCurrentChunk(0);
  }

  // Set current start position for isEmpty checks.
  setStartPosition();
}

size_t js::Nursery::spaceToEnd(unsigned chunkCount) const {
  if (chunkCount == 0) {
    return 0;
  }

  unsigned lastChunk = chunkCount - 1;

  MOZ_ASSERT(lastChunk >= currentStartChunk_);
  MOZ_ASSERT(currentStartPosition_ - chunk(currentStartChunk_).start() <=
             NurseryChunkUsableSize);

  size_t bytes;

  if (chunkCount != 1) {
    // In the general case we have to add:
    //  + the bytes used in the first
    //    chunk which may be less than the total size of a chunk since in some
    //    zeal modes we start the first chunk at some later position
    //    (currentStartPosition_).
    //  + the size of all the other chunks.
    bytes = (chunk(currentStartChunk_).end() - currentStartPosition_) +
            ((lastChunk - currentStartChunk_) * ChunkSize);
  } else {
    // In sub-chunk mode, but it also works whenever chunkCount == 1, we need to
    // use currentEnd_ since it may not refer to a full chunk.
    bytes = currentEnd_ - currentStartPosition_;
  }

  MOZ_ASSERT(bytes <= maxChunkCount() * ChunkSize);

  return bytes;
}

MOZ_ALWAYS_INLINE void js::Nursery::setCurrentChunk(unsigned chunkno) {
  MOZ_ASSERT(chunkno < allocatedChunkCount());

  currentChunk_ = chunkno;
  position_ = chunk(chunkno).start();
  setCurrentEnd();
}

void js::Nursery::poisonAndInitCurrentChunk(size_t extent) {
  if (gc->hasZealMode(ZealMode::GenerationalGC) || !isSubChunkMode()) {
    chunk(currentChunk_).poisonAndInit(runtime());
  } else {
    extent = std::min(capacity_, extent);
    chunk(currentChunk_).poisonAndInit(runtime(), extent);
  }
}

MOZ_ALWAYS_INLINE void js::Nursery::setCurrentEnd() {
  MOZ_ASSERT_IF(isSubChunkMode(),
                currentChunk_ == 0 && currentEnd_ <= chunk(0).end());
  currentEnd_ =
      uintptr_t(&chunk(currentChunk_)) + std::min(capacity_, ChunkSize);
}

bool js::Nursery::allocateNextChunk(const unsigned chunkno,
                                    AutoLockGCBgAlloc& lock) {
  const unsigned priorCount = allocatedChunkCount();
  const unsigned newCount = priorCount + 1;

  MOZ_ASSERT((chunkno == currentChunk_ + 1) ||
             (chunkno == 0 && allocatedChunkCount() == 0));
  MOZ_ASSERT(chunkno == allocatedChunkCount());
  MOZ_ASSERT(chunkno < HowMany(capacity(), ChunkSize));

  if (!chunks_.resize(newCount)) {
    return false;
  }

  TenuredChunk* newChunk;
  newChunk = gc->getOrAllocChunk(lock);
  if (!newChunk) {
    chunks_.shrinkTo(priorCount);
    return false;
  }

  chunks_[chunkno] = NurseryChunk::fromChunk(newChunk);
  return true;
}

MOZ_ALWAYS_INLINE void js::Nursery::setStartPosition() {
  currentStartChunk_ = currentChunk_;
  currentStartPosition_ = position();
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

  size_t newCapacity = mozilla::Clamp(targetSize(options, reason),
                                      tunables().gcMinNurseryBytes(),
                                      tunables().gcMaxNurseryBytes());

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

  // If the nursery is completely unused then minimise it.
  if (hasRecentGrowthData && previousGC.nurseryUsedBytes == 0 &&
      now - lastCollectionEndTime() >
          tunables().nurseryTimeoutForIdleCollection() &&
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

  // Decrease the growth factor to try to keep collections shorter than a target
  // maximum time. Don't do this during page load.
  static const double MaxTimeGoalMs = 4.0;
  if (!gc->isInPageLoad() && !js::SupportDifferentialTesting()) {
    double timeGrowth = MaxTimeGoalMs / collectorTime.ToMilliseconds();
    growthFactor = std::min(growthFactor, timeGrowth);
  }

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
  MOZ_ASSERT_IF(!isSubChunkMode(), newCapacity > currentChunk_ * ChunkSize);
  MOZ_ASSERT(newCapacity <= tunables().gcMaxNurseryBytes());
  MOZ_ASSERT(newCapacity > capacity());

  if (!decommitTask->reserveSpaceForBytes(newCapacity)) {
    return;
  }

  if (isSubChunkMode()) {
    MOZ_ASSERT(currentChunk_ == 0);

    // The remainder of the chunk may have been decommitted.
    if (!chunk(0).markPagesInUseHard(std::min(newCapacity, ChunkSize))) {
      // The OS won't give us the memory we need, we can't grow.
      return;
    }

    // The capacity has changed and since we were in sub-chunk mode we need to
    // update the poison values / asan information for the now-valid region of
    // this chunk.
    size_t size = std::min(newCapacity, ChunkSize) - capacity();
    chunk(0).poisonRange(capacity(), size, JS_FRESH_NURSERY_PATTERN,
                         MemCheckKind::MakeUndefined);
  }

  capacity_ = newCapacity;

  setCurrentEnd();
}

void js::Nursery::freeChunksFrom(const unsigned firstFreeChunk) {
  MOZ_ASSERT(firstFreeChunk < chunks_.length());

  // The loop below may need to skip the first chunk, so we may use this so we
  // can modify it.
  unsigned firstChunkToDecommit = firstFreeChunk;

  if ((firstChunkToDecommit == 0) && isSubChunkMode()) {
    // Part of the first chunk may be hard-decommitted, un-decommit it so that
    // the GC's normal chunk-handling doesn't segfault.
    MOZ_ASSERT(currentChunk_ == 0);
    if (!chunk(0).markPagesInUseHard(ChunkSize)) {
      // Free the chunk if we can't allocate its pages.
      UnmapPages(static_cast<void*>(&chunk(0)), ChunkSize);
      firstChunkToDecommit = 1;
    }
  }

  {
    AutoLockHelperThreadState lock;
    for (size_t i = firstChunkToDecommit; i < chunks_.length(); i++) {
      decommitTask->queueChunk(chunks_[i], lock);
    }
  }

  chunks_.shrinkTo(firstFreeChunk);
}

void js::Nursery::shrinkAllocableSpace(size_t newCapacity) {
#ifdef JS_GC_ZEAL
  if (gc->hasZealMode(ZealMode::GenerationalGC)) {
    return;
  }
#endif

  // Don't shrink the nursery to zero (use Nursery::disable() instead)
  // This can't happen due to the rounding-down performed above because of the
  // clamping in maybeResizeNursery().
  MOZ_ASSERT(newCapacity != 0);
  // Don't attempt to shrink it to the same size.
  if (newCapacity == capacity_) {
    return;
  }
  MOZ_ASSERT(newCapacity < capacity_);

  unsigned newCount = HowMany(newCapacity, ChunkSize);
  if (newCount < allocatedChunkCount()) {
    freeChunksFrom(newCount);
  }

  size_t oldCapacity = capacity_;
  capacity_ = newCapacity;

  setCurrentEnd();

  if (isSubChunkMode()) {
    MOZ_ASSERT(currentChunk_ == 0);
    size_t size = std::min(oldCapacity, ChunkSize) - newCapacity;
    chunk(0).poisonRange(newCapacity, size, JS_SWEPT_NURSERY_PATTERN,
                         MemCheckKind::MakeNoAccess);

    AutoLockHelperThreadState lock;
    decommitTask->queueRange(capacity_, chunk(0), lock);
  }
}

uintptr_t js::Nursery::currentEnd() const {
  // These are separate asserts because it can be useful to see which one
  // failed.
  MOZ_ASSERT_IF(isSubChunkMode(), currentChunk_ == 0);
  MOZ_ASSERT_IF(isSubChunkMode(), currentEnd_ <= chunk(currentChunk_).end());
  MOZ_ASSERT_IF(!isSubChunkMode(), currentEnd_ == chunk(currentChunk_).end());
  MOZ_ASSERT(currentEnd_ != chunk(currentChunk_).start());
  return currentEnd_;
}

gcstats::Statistics& js::Nursery::stats() const { return gc->stats(); }

MOZ_ALWAYS_INLINE const js::gc::GCSchedulingTunables& js::Nursery::tunables()
    const {
  return gc->tunables;
}

bool js::Nursery::isSubChunkMode() const {
  return capacity() <= NurseryChunkUsableSize;
}

void js::Nursery::sweepMapAndSetObjects() {
  auto gcx = runtime()->gcContext();

  for (auto mapobj : mapsWithNurseryMemory_) {
    MapObject::sweepAfterMinorGC(gcx, mapobj);
  }
  mapsWithNurseryMemory_.clearAndFree();

  for (auto setobj : setsWithNurseryMemory_) {
    SetObject::sweepAfterMinorGC(gcx, setobj);
  }
  setsWithNurseryMemory_.clearAndFree();
}

void js::Nursery::joinDecommitTask() { decommitTask->join(); }

JS_PUBLIC_API void JS::EnableNurseryStrings(JSContext* cx) {
  AutoEmptyNursery empty(cx);
  ReleaseAllJITCode(cx->gcContext());
  cx->runtime()->gc.nursery().enableStrings();
}

JS_PUBLIC_API void JS::DisableNurseryStrings(JSContext* cx) {
  AutoEmptyNursery empty(cx);
  ReleaseAllJITCode(cx->gcContext());
  cx->runtime()->gc.nursery().disableStrings();
}

JS_PUBLIC_API void JS::EnableNurseryBigInts(JSContext* cx) {
  AutoEmptyNursery empty(cx);
  ReleaseAllJITCode(cx->gcContext());
  cx->runtime()->gc.nursery().enableBigInts();
}

JS_PUBLIC_API void JS::DisableNurseryBigInts(JSContext* cx) {
  AutoEmptyNursery empty(cx);
  ReleaseAllJITCode(cx->gcContext());
  cx->runtime()->gc.nursery().disableBigInts();
}
