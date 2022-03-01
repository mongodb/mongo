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

#include <algorithm>
#include <cmath>
#include <utility>

#include "builtin/MapObject.h"
#include "debugger/DebugAPI.h"
#include "gc/FreeOp.h"
#include "gc/GCInternals.h"
#include "gc/GCLock.h"
#include "gc/Memory.h"
#include "gc/PublicIterators.h"
#include "jit/JitFrames.h"
#include "jit/JitRealm.h"
#include "util/DifferentialTesting.h"
#include "util/GetPidProvider.h"  // getpid()
#include "util/Poison.h"
#include "vm/ArrayObject.h"
#include "vm/JSONPrinter.h"
#include "vm/Realm.h"
#include "vm/Time.h"
#include "vm/TypedArrayObject.h"

#include "gc/Marking-inl.h"
#include "gc/Zone-inl.h"
#include "vm/NativeObject-inl.h"

using namespace js;
using namespace gc;

using mozilla::DebugOnly;
using mozilla::PodCopy;
using mozilla::TimeDuration;
using mozilla::TimeStamp;

#ifdef JS_GC_ZEAL
constexpr uint32_t CanaryMagicValue = 0xDEADB15D;

struct alignas(gc::CellAlignBytes) js::Nursery::Canary {
  uint32_t magicValue;
  Canary* next;
};
#endif

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

  // The end of the range is always ChunkSize.
  void markPagesUnusedHard(size_t from);
  // The start of the range is always the beginning of the chunk.
  [[nodiscard]] bool markPagesInUseHard(size_t to);

  uintptr_t start() const { return uintptr_t(&data); }
  uintptr_t end() const { return uintptr_t(this) + ChunkSize; }
};
static_assert(sizeof(js::NurseryChunk) == gc::ChunkSize,
              "Nursery chunk size must match gc::Chunk size.");

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

inline void js::NurseryChunk::markPagesUnusedHard(size_t from) {
  MOZ_ASSERT(from >= sizeof(ChunkBase));  // Don't touch the header.
  MOZ_ASSERT(from <= ChunkSize);
  uintptr_t start = uintptr_t(this) + from;
  MarkPagesUnusedHard(reinterpret_cast<void*>(start), ChunkSize - from);
}

inline bool js::NurseryChunk::markPagesInUseHard(size_t to) {
  MOZ_ASSERT(to >= sizeof(ChunkBase));
  MOZ_ASSERT(to <= ChunkSize);
  return MarkPagesInUseHard(this, to);
}

// static
inline js::NurseryChunk* js::NurseryChunk::fromChunk(TenuredChunk* chunk) {
  return reinterpret_cast<NurseryChunk*>(chunk);
}

js::NurseryDecommitTask::NurseryDecommitTask(gc::GCRuntime* gc)
    : GCParallelTask(gc) {}

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
    auto* tenuredChunk = reinterpret_cast<TenuredChunk*>(nurseryChunk);
    tenuredChunk->init(gc);
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
    : gc(gc),
      position_(0),
      currentStartChunk_(0),
      currentStartPosition_(0),
      currentEnd_(0),
      currentStringEnd_(0),
      currentBigIntEnd_(0),
      currentChunk_(0),
      capacity_(0),
      timeInChunkAlloc_(0),
      enableProfiling_(false),
      profileThreshold_(0),
      canAllocateStrings_(true),
      canAllocateBigInts_(true),
      reportDeduplications_(false),
      reportPretenuring_(false),
      minorGCTriggerReason_(JS::GCReason::NO_REASON),
      hasRecentGrowthData(false),
      smoothedGrowthFactor(1.0),
      decommitTask(gc)
#ifdef JS_GC_ZEAL
      ,
      lastCanary_(nullptr)
#endif
{
  const char* env = getenv("MOZ_NURSERY_STRINGS");
  if (env && *env) {
    canAllocateStrings_ = (*env == '1');
  }
  env = getenv("MOZ_NURSERY_BIGINTS");
  if (env && *env) {
    canAllocateBigInts_ = (*env == '1');
  }
}

static const char* GetEnvVar(const char* name, const char* helpMessage) {
  const char* value = getenv(name);
  if (!value) {
    return nullptr;
  }

  if (strcmp(value, "help") == 0) {
    fprintf(stderr, "%s", helpMessage);
    exit(0);
  }

  return value;
}

static bool GetBoolEnvVar(const char* name, const char* helpMessage) {
  const char* env = GetEnvVar(name, helpMessage);
  return env && bool(atoi(env));
}

bool js::Nursery::init(AutoLockGCBgAlloc& lock) {
  ReadProfileEnv("JS_GC_PROFILE_NURSERY",
                 "Report minor GCs taking at least N microseconds.\n",
                 &enableProfiling_, &profileWorkers_, &profileThreshold_);

  reportDeduplications_ = GetBoolEnvVar(
      "JS_GC_REPORT_STATS",
      "JS_GC_REPORT_STATS=1\n"
      "\tAfter a minor GC, report how many strings were deduplicated.\n");

  reportPretenuring_ = GetBoolEnvVar(
      "JS_GC_REPORT_PRETENURE",
      "JS_GC_REPORT_PRETENURE=1\n"
      "\tAfter a minor GC, report information about pretenuring.\n");

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

  // This should always succeed after the first time it's called.
  MOZ_ALWAYS_TRUE(gc->storeBuffer().enable());
}

bool js::Nursery::initFirstChunk(AutoLockGCBgAlloc& lock) {
  MOZ_ASSERT(!isEnabled());

  capacity_ = tunables().gcMinNurseryBytes();

  if (!decommitTask.reserveSpaceForBytes(capacity_) ||
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
  stringDeDupSet.reset();
  MOZ_ASSERT(isEmpty());
  if (!isEnabled()) {
    return;
  }

  // Free all chunks.
  decommitTask.join();
  freeChunksFrom(0);
  decommitTask.runFromMainThread();

  capacity_ = 0;

  // We must reset currentEnd_ so that there is no space for anything in the
  // nursery. JIT'd code uses this even if the nursery is disabled.
  currentEnd_ = 0;
  currentStringEnd_ = 0;
  currentBigIntEnd_ = 0;
  position_ = 0;
  gc->storeBuffer().disable();
}

void js::Nursery::enableStrings() {
  MOZ_ASSERT(isEmpty());
  canAllocateStrings_ = true;
  currentStringEnd_ = currentEnd_;
}

void js::Nursery::disableStrings() {
  MOZ_ASSERT(isEmpty());
  canAllocateStrings_ = false;
  currentStringEnd_ = 0;
}

void js::Nursery::enableBigInts() {
  MOZ_ASSERT(isEmpty());
  canAllocateBigInts_ = true;
  currentBigIntEnd_ = currentEnd_;
}

void js::Nursery::disableBigInts() {
  MOZ_ASSERT(isEmpty());
  canAllocateBigInts_ = false;
  currentBigIntEnd_ = 0;
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

  decommitTask.join();

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

  if (!decommitTask.reserveSpaceForBytes(capacity_)) {
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

JSObject* js::Nursery::allocateObject(gc::AllocSite* site, size_t size,
                                      size_t nDynamicSlots,
                                      const JSClass* clasp) {
  // Ensure there's enough space to replace the contents with a
  // RelocationOverlay.
  MOZ_ASSERT(size >= sizeof(RelocationOverlay));

  // Sanity check the finalizer.
  MOZ_ASSERT_IF(clasp->hasFinalize(), CanNurseryAllocateFinalizedClass(clasp) ||
                                          clasp->isProxyObject());

  auto* obj = reinterpret_cast<JSObject*>(
      allocateCell(site, size, JS::TraceKind::Object));
  if (!obj) {
    return nullptr;
  }

  // If we want external slots, add them.
  ObjectSlots* slotsHeader = nullptr;
  if (nDynamicSlots) {
    MOZ_ASSERT(clasp->isNativeObject());
    void* allocation =
        allocateBuffer(site->zone(), ObjectSlots::allocSize(nDynamicSlots));
    if (!allocation) {
      // It is safe to leave the allocated object uninitialized, since we
      // do not visit unallocated things in the nursery.
      return nullptr;
    }
    slotsHeader = new (allocation) ObjectSlots(nDynamicSlots, 0);
  }

  // Store slots pointer directly in new object. If no dynamic slots were
  // requested, caller must initialize slots_ field itself as needed. We
  // don't know if the caller was a native object or not.
  if (nDynamicSlots) {
    static_cast<NativeObject*>(obj)->initSlots(slotsHeader->slots());
  }

  gcprobes::NurseryAlloc(obj, size);
  return obj;
}

Cell* js::Nursery::allocateCell(gc::AllocSite* site, size_t size,
                                JS::TraceKind kind) {
  // Ensure there's enough space to replace the contents with a
  // RelocationOverlay.
  MOZ_ASSERT(size >= sizeof(RelocationOverlay));
  MOZ_ASSERT(size % CellAlignBytes == 0);

  void* ptr = allocate(sizeof(NurseryCellHeader) + size);
  if (!ptr) {
    return nullptr;
  }

  new (ptr) NurseryCellHeader(site, kind);

  auto cell =
      reinterpret_cast<Cell*>(uintptr_t(ptr) + sizeof(NurseryCellHeader));

  // Update the allocation site. This code is also inlined in
  // MacroAssembler::updateAllocSite.
  if (!site->isInAllocatedList()) {
    pretenuringNursery.insertIntoAllocatedList(site);
  }
  site->incAllocCount();

  gcprobes::NurseryAlloc(cell, kind);
  return cell;
}

Cell* js::Nursery::allocateString(gc::AllocSite* site, size_t size) {
  Cell* cell = allocateCell(site, size, JS::TraceKind::String);
  if (cell) {
    site->zone()->nurseryAllocatedStrings++;
  }
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

#ifdef JS_GC_ZEAL
  if (gc->hasZealMode(ZealMode::CheckNursery)) {
    size += sizeof(Canary);
  }
#endif

  if (MOZ_UNLIKELY(currentEnd() < position() + size)) {
    return moveToNextChunkAndAllocate(size);
  }

  void* thing = (void*)position();
  position_ = position() + size;
  // We count this regardless of the profiler's state, assuming that it costs
  // just as much to count it, as to check the profiler's state and decide not
  // to count it.
  stats().noteNurseryAlloc();

  DebugOnlyPoison(thing, JS_ALLOCATED_NURSERY_PATTERN, size,
                  MemCheckKind::MakeUndefined);

#ifdef JS_GC_ZEAL
  if (gc->hasZealMode(ZealMode::CheckNursery)) {
    writeCanary(position() - sizeof(Canary));
  }
#endif

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
    mozilla::TimeStamp start = ReallyNow();
    {
      AutoLockGCBgAlloc lock(gc);
      if (!allocateNextChunk(chunkno, lock)) {
        return nullptr;
      }
    }
    timeInChunkAlloc_ += ReallyNow() - start;
    MOZ_ASSERT(chunkno < allocatedChunkCount());
  }
  setCurrentChunk(chunkno);
  poisonAndInitCurrentChunk();

  // We know there's enough space to allocate now so we can call allocate()
  // recursively. Adjust the size for the nursery canary which it will add on.
  MOZ_ASSERT(currentEnd() >= position() + size);
#ifdef JS_GC_ZEAL
  if (gc->hasZealMode(ZealMode::CheckNursery)) {
    size -= sizeof(Canary);
  }
#endif
  return allocate(size);
}

#ifdef JS_GC_ZEAL
inline void Nursery::writeCanary(uintptr_t address) {
  auto* canary = reinterpret_cast<Canary*>(address);
  new (canary) Canary{CanaryMagicValue, nullptr};
  if (lastCanary_) {
    MOZ_ASSERT(!lastCanary_->next);
    lastCanary_->next = canary;
  }
  lastCanary_ = canary;
}
#endif

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

void* js::Nursery::allocateBuffer(JSObject* obj, size_t nbytes) {
  MOZ_ASSERT(obj);
  MOZ_ASSERT(nbytes > 0);

  if (!IsInsideNursery(obj)) {
    return obj->zone()->pod_malloc<uint8_t>(nbytes);
  }
  return allocateBuffer(obj->zone(), nbytes);
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

js::TenuringTracer::TenuringTracer(JSRuntime* rt, Nursery* nursery)
    : GenericTracer(rt, JS::TracerKind::Tenuring,
                    JS::WeakMapTraceAction::TraceKeysAndValues),
      nursery_(*nursery),
      tenuredSize(0),
      tenuredCells(0),
      objHead(nullptr),
      objTail(&objHead),
      stringHead(nullptr),
      stringTail(&stringHead) {}

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
                  stats().allocsSinceMinorGCNursery());
    json.property("cells_allocated_tenured",
                  stats().allocsSinceMinorGCTenured());
  }

  if (stats().getStat(gcstats::STAT_NURSERY_STRING_REALMS_DISABLED)) {
    json.property(
        "nursery_string_realms_disabled",
        stats().getStat(gcstats::STAT_NURSERY_STRING_REALMS_DISABLED));
  }
  if (stats().getStat(gcstats::STAT_NURSERY_BIGINT_REALMS_DISABLED)) {
    json.property(
        "nursery_bigint_realms_disabled",
        stats().getStat(gcstats::STAT_NURSERY_BIGINT_REALMS_DISABLED));
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

void js::Nursery::printCollectionProfile(JS::GCReason reason,
                                         double promotionRate) {
  stats().maybePrintProfileHeaders();

  TimeDuration ts = collectionStartTime() - stats().creationTime();

  fprintf(
      stderr, "MinorGC: %6zu %14p %10.6f %-20.20s %5.1f%% %6zu %6zu %6" PRIu32,
      size_t(getpid()), runtime(), ts.ToSeconds(), JS::ExplainGCReason(reason),
      promotionRate * 100, previousGC.nurseryCapacity / 1024, capacity() / 1024,
      stats().getStat(gcstats::STAT_STRINGS_DEDUPLICATED));

  printProfileDurations(profileDurations_);
}

// static
void js::Nursery::printProfileHeader() {
  fprintf(
      stderr,
      "MinorGC: PID    Runtime        Timestamp  Reason               PRate  "
      "OldSz  NewSz  Dedup ");
#define PRINT_HEADER(name, text) fprintf(stderr, " %-6.6s", text);
  FOR_EACH_NURSERY_PROFILE_TIME(PRINT_HEADER)
#undef PRINT_HEADER
  fprintf(stderr, "\n");
}

// static
void js::Nursery::printProfileDurations(const ProfileDurations& times) {
  for (auto time : times) {
    fprintf(stderr, " %6" PRIi64, static_cast<int64_t>(time.ToMicroseconds()));
  }
  fprintf(stderr, "\n");
}

void js::Nursery::printTotalProfileTimes() {
  if (enableProfiling_) {
    fprintf(stderr,
            "MinorGC: %6zu %14p TOTALS: %7" PRIu64
            " collections:               %16" PRIu64,
            size_t(getpid()), runtime(), gc->stringStats.deduplicatedStrings,
            gc->minorGCCount());
    printProfileDurations(totalDurations_);
  }
}

void js::Nursery::maybeClearProfileDurations() {
  for (auto& duration : profileDurations_) {
    duration = mozilla::TimeDuration();
  }
}

inline void js::Nursery::startProfile(ProfileKey key) {
  startTimes_[key] = ReallyNow();
}

inline void js::Nursery::endProfile(ProfileKey key) {
  profileDurations_[key] = ReallyNow() - startTimes_[key];
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
  TimeDuration timeSinceLastCollection = ReallyNow() - previousGC.endTime;
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

#ifdef JS_GC_ZEAL
  if (gc->hasZealMode(ZealMode::CheckNursery)) {
    for (auto canary = lastCanary_; canary; canary = canary->next) {
      MOZ_ASSERT(canary->magicValue == CanaryMagicValue);
    }
  }
  lastCanary_ = nullptr;
#endif

  stats().beginNurseryCollection(reason);
  gcprobes::MinorGCStart();

  stringDeDupSet.emplace();
  auto guardStringDedupSet =
      mozilla::MakeScopeExit([&] { stringDeDupSet.reset(); });

  maybeClearProfileDurations();
  startProfile(ProfileKey::Total);

  previousGC.reason = JS::GCReason::NO_REASON;
  previousGC.nurseryUsedBytes = usedSpace();
  previousGC.nurseryCapacity = capacity();
  previousGC.nurseryCommitted = committed();
  previousGC.tenuredBytes = 0;
  previousGC.tenuredCells = 0;

  // If it isn't empty, it will call doCollection, and possibly after that
  // isEmpty() will become true, so use another variable to keep track of the
  // old empty state.
  bool wasEmpty = isEmpty();
  if (!wasEmpty) {
    CollectionResult result = doCollection(reason);
    previousGC.reason = reason;
    previousGC.tenuredBytes = result.tenuredBytes;
    previousGC.tenuredCells = result.tenuredCells;
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

  previousGC.endTime = ReallyNow();  // Must happen after maybeResizeNursery.
  endProfile(ProfileKey::Total);
  gc->incMinorGcNumber();

  TimeDuration totalTime = profileDurations_[ProfileKey::Total];
  sendTelemetry(reason, totalTime, wasEmpty, promotionRate, sitesPretenured);

  stats().endNurseryCollection(reason);
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
  rt->addTelemetry(JS_TELEMETRY_GC_MINOR_REASON, uint32_t(reason));
  if (totalTime.ToMilliseconds() > 1.0) {
    rt->addTelemetry(JS_TELEMETRY_GC_MINOR_REASON_LONG, uint32_t(reason));
  }
  rt->addTelemetry(JS_TELEMETRY_GC_MINOR_US, totalTime.ToMicroseconds());
  rt->addTelemetry(JS_TELEMETRY_GC_NURSERY_BYTES, committed());

  if (!wasEmpty) {
    rt->addTelemetry(JS_TELEMETRY_GC_PRETENURE_COUNT_2, sitesPretenured);
    rt->addTelemetry(JS_TELEMETRY_GC_NURSERY_PROMOTION_RATE,
                     promotionRate * 100);
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

js::Nursery::CollectionResult js::Nursery::doCollection(JS::GCReason reason) {
  JSRuntime* rt = runtime();
  AutoGCSession session(gc, JS::HeapState::MinorCollecting);
  AutoSetThreadIsPerformingGC performingGC;
  AutoStopVerifyingBarriers av(rt, false);
  AutoDisableProxyCheck disableStrictProxyChecking;
  mozilla::DebugOnly<AutoEnterOOMUnsafeRegion> oomUnsafeRegion;

  // Move objects pointed to by roots from the nursery to the major heap.
  TenuringTracer mover(rt, this);

  // Mark the store buffer. This must happen first.
  StoreBuffer& sb = gc->storeBuffer();

  // Strings in the whole cell buffer must be traced first, in order to mark
  // tenured dependent strings' bases as non-deduplicatable. The rest of
  // nursery collection (whole non-string cells, edges, etc.) can happen later.
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

  startProfile(ProfileKey::MarkDebugger);
  {
    gcstats::AutoPhase ap(stats(), gcstats::PhaseKind::MARK_ROOTS);
    DebugAPI::traceAllForMovingGC(&mover);
  }
  endProfile(ProfileKey::MarkDebugger);

  startProfile(ProfileKey::SweepCaches);
  gc->purgeRuntimeForMinorGC();
  endProfile(ProfileKey::SweepCaches);

  // Most of the work is done here. This loop iterates over objects that have
  // been moved to the major heap. If these objects have any outgoing pointers
  // to the nursery, then those nursery objects get moved as well, until no
  // objects are left to move. That is, we iterate to a fixed point.
  startProfile(ProfileKey::CollectToObjFP);
  collectToObjectFixedPoint(mover);
  endProfile(ProfileKey::CollectToObjFP);

  startProfile(ProfileKey::CollectToStrFP);
  collectToStringFixedPoint(mover);
  endProfile(ProfileKey::CollectToStrFP);

  // Sweep to update any pointers to nursery objects that have now been
  // tenured.
  startProfile(ProfileKey::Sweep);
  sweep(&mover);
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
    gc->checkHashTablesAfterMovingGC();
  }
#endif
  endProfile(ProfileKey::CheckHashTables);

  return {mover.tenuredSize, mover.tenuredCells};
}

size_t js::Nursery::doPretenuring(JSRuntime* rt, JS::GCReason reason,
                                  bool validPromotionRate,
                                  double promotionRate) {
  size_t sitesPretenured = pretenuringNursery.doPretenuring(
      gc, validPromotionRate, promotionRate, reportPretenuring_);

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

  mozilla::Maybe<AutoGCSession> session;
  uint32_t numStringsTenured = 0;
  uint32_t numNurseryStringRealmsDisabled = 0;
  uint32_t numBigIntsTenured = 0;
  uint32_t numNurseryBigIntRealmsDisabled = 0;
  for (ZonesIter zone(gc, SkipAtoms); !zone.done(); zone.next()) {
    // For some tests in JetStream2 and Kraken, the tenuredRate is high but the
    // number of allocated strings is low. So we calculate the tenuredRate only
    // if the number of string allocations is enough.
    bool allocThreshold = zone->nurseryAllocatedStrings > 30000;
    uint64_t zoneTenuredStrings =
        zone->stringStats.ref().liveNurseryStrings -
        zone->previousGCStringStats.ref().liveNurseryStrings;
    double tenuredRate =
        allocThreshold
            ? double(zoneTenuredStrings) / double(zone->nurseryAllocatedStrings)
            : 0.0;
    bool disableNurseryStrings =
        pretenureStr && zone->allocNurseryStrings &&
        tenuredRate > tunables().pretenureStringThreshold();
    bool disableNurseryBigInts = pretenureBigInt && zone->allocNurseryBigInts &&
                                 zone->tenuredBigInts >= 30 * 1000;
    if (disableNurseryStrings || disableNurseryBigInts) {
      if (!session.isSome()) {
        session.emplace(gc, JS::HeapState::MinorCollecting);
      }
      CancelOffThreadIonCompile(zone);
      bool preserving = zone->isPreservingCode();
      zone->setPreservingCode(false);
      zone->discardJitCode(rt->defaultFreeOp());
      zone->setPreservingCode(preserving);
      for (RealmsInZoneIter r(zone); !r.done(); r.next()) {
        if (jit::JitRealm* jitRealm = r->jitRealm()) {
          jitRealm->discardStubs();
          if (disableNurseryStrings) {
            jitRealm->setStringsCanBeInNursery(false);
            numNurseryStringRealmsDisabled++;
          }
          if (disableNurseryBigInts) {
            numNurseryBigIntRealmsDisabled++;
          }
        }
      }
      if (disableNurseryStrings) {
        zone->allocNurseryStrings = false;
      }
      if (disableNurseryBigInts) {
        zone->allocNurseryBigInts = false;
      }
    }
    numStringsTenured += zoneTenuredStrings;
    numBigIntsTenured += zone->tenuredBigInts;
    zone->tenuredBigInts = 0;
    zone->nurseryAllocatedStrings = 0;
  }
  session.reset();  // End the minor GC session, if running one.
  stats().setStat(gcstats::STAT_NURSERY_STRING_REALMS_DISABLED,
                  numNurseryStringRealmsDisabled);
  stats().setStat(gcstats::STAT_STRINGS_TENURED, numStringsTenured);
  stats().setStat(gcstats::STAT_NURSERY_BIGINT_REALMS_DISABLED,
                  numNurseryBigIntRealmsDisabled);
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

void js::Nursery::sweep(JSTracer* trc) {
  // Sweep unique IDs first before we sweep any tables that may be keyed based
  // on them.
  for (Cell* cell : cellsWithUid_) {
    auto* obj = static_cast<JSObject*>(cell);
    if (!IsForwarded(obj)) {
      obj->nurseryZone()->removeUniqueId(obj);
    } else {
      JSObject* dst = Forwarded(obj);
      obj->nurseryZone()->transferUniqueId(dst, obj);
    }
  }
  cellsWithUid_.clear();

  for (CompartmentsIter c(runtime()); !c.done(); c.next()) {
    c->sweepAfterMinorGC(trc);
  }

  for (ZonesIter zone(trc->runtime(), SkipAtoms); !zone.done(); zone.next()) {
    zone->sweepAfterMinorGC(trc);
  }

  sweepMapAndSetObjects();
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
  if (canAllocateStrings_) {
    currentStringEnd_ = currentEnd_;
  }
  if (canAllocateBigInts_) {
    currentBigIntEnd_ = currentEnd_;
  }
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

  decommitTask.join();

  size_t newCapacity = mozilla::Clamp(targetSize(options, reason),
                                      tunables().gcMinNurseryBytes(),
                                      tunables().gcMaxNurseryBytes());

  MOZ_ASSERT(roundSize(newCapacity) == newCapacity);

  if (newCapacity > capacity()) {
    growAllocableSpace(newCapacity);
  } else if (newCapacity < capacity()) {
    shrinkAllocableSpace(newCapacity);
  }

  AutoLockHelperThreadState lock;
  if (!decommitTask.isEmpty(lock)) {
    decommitTask.startOrRunIfIdle(lock);
  }
}

static inline double ClampDouble(double value, double min, double max) {
  MOZ_ASSERT(!std::isnan(value) && !std::isnan(min) && !std::isnan(max));
  MOZ_ASSERT(max >= min);

  if (value <= min) {
    return min;
  }

  if (value >= max) {
    return max;
  }

  return value;
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
  if (gc::IsShutdownReason(reason)) {
    clearRecentGrowthData();
    return capacity();
  }

  TimeStamp now = ReallyNow();

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

  // Calculate the fraction of time spent collecting the nursery.
  double timeFraction = 0.0;
  if (hasRecentGrowthData && !js::SupportDifferentialTesting()) {
    TimeDuration collectorTime = now - collectionStartTime();
    TimeDuration totalTime = now - lastCollectionEndTime();
    timeFraction = collectorTime.ToSeconds() / totalTime.ToSeconds();
  }

  // Adjust the nursery size to try to achieve a target promotion rate and
  // collector time goals.
  static const double PromotionGoal = 0.02;
  static const double TimeGoal = 0.01;
  double growthFactor =
      std::max(fractionPromoted / PromotionGoal, timeFraction / TimeGoal);

  // Limit the range of the growth factor to prevent transient high promotion
  // rates from affecting the nursery size too far into the future.
  static const double GrowthRange = 2.0;
  growthFactor = ClampDouble(growthFactor, 1.0 / GrowthRange, GrowthRange);

  // Use exponential smoothing on the desired growth rate to take into account
  // the promotion rate from recent previous collections.
  if (hasRecentGrowthData &&
      now - lastCollectionEndTime() < TimeDuration::FromMilliseconds(200) &&
      !js::SupportDifferentialTesting()) {
    growthFactor = 0.75 * smoothedGrowthFactor + 0.25 * growthFactor;
  }

  hasRecentGrowthData = true;
  smoothedGrowthFactor = growthFactor;

  // Leave size untouched if we are close to the promotion goal.
  static const double GoalWidth = 1.5;
  if (growthFactor > (1.0 / GoalWidth) && growthFactor < GoalWidth) {
    return capacity();
  }

  // The multiplication below cannot overflow because growthFactor is at
  // most two.
  MOZ_ASSERT(growthFactor <= 2.0);
  MOZ_ASSERT(capacity() < SIZE_MAX / 2);

  return roundSize(size_t(double(capacity()) * growthFactor));
}

void js::Nursery::clearRecentGrowthData() {
  if (js::SupportDifferentialTesting()) {
    return;
  }

  hasRecentGrowthData = false;
  smoothedGrowthFactor = 1.0;
}

/* static */
size_t js::Nursery::roundSize(size_t size) {
  size_t step = size >= ChunkSize ? ChunkSize : SystemPageSize();
  size = Round(size, step);

  MOZ_ASSERT(size >= SystemPageSize());

  return size;
}

void js::Nursery::growAllocableSpace(size_t newCapacity) {
  MOZ_ASSERT_IF(!isSubChunkMode(), newCapacity > currentChunk_ * ChunkSize);
  MOZ_ASSERT(newCapacity <= tunables().gcMaxNurseryBytes());
  MOZ_ASSERT(newCapacity > capacity());

  if (!decommitTask.reserveSpaceForBytes(newCapacity)) {
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
      decommitTask.queueChunk(chunks_[i], lock);
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
    decommitTask.queueRange(capacity_, chunk(0), lock);
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
  auto fop = runtime()->defaultFreeOp();

  for (auto mapobj : mapsWithNurseryMemory_) {
    MapObject::sweepAfterMinorGC(fop, mapobj);
  }
  mapsWithNurseryMemory_.clearAndFree();

  for (auto setobj : setsWithNurseryMemory_) {
    SetObject::sweepAfterMinorGC(fop, setobj);
  }
  setsWithNurseryMemory_.clearAndFree();
}

JS_PUBLIC_API void JS::EnableNurseryStrings(JSContext* cx) {
  AutoEmptyNursery empty(cx);
  ReleaseAllJITCode(cx->defaultFreeOp());
  cx->runtime()->gc.nursery().enableStrings();
}

JS_PUBLIC_API void JS::DisableNurseryStrings(JSContext* cx) {
  AutoEmptyNursery empty(cx);
  ReleaseAllJITCode(cx->defaultFreeOp());
  cx->runtime()->gc.nursery().disableStrings();
}

JS_PUBLIC_API void JS::EnableNurseryBigInts(JSContext* cx) {
  AutoEmptyNursery empty(cx);
  ReleaseAllJITCode(cx->defaultFreeOp());
  cx->runtime()->gc.nursery().enableBigInts();
}

JS_PUBLIC_API void JS::DisableNurseryBigInts(JSContext* cx) {
  AutoEmptyNursery empty(cx);
  ReleaseAllJITCode(cx->defaultFreeOp());
  cx->runtime()->gc.nursery().disableBigInts();
}
