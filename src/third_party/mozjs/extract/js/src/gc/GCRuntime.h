/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef gc_GCRuntime_h
#define gc_GCRuntime_h

#include "mozilla/Atomics.h"
#include "mozilla/DoublyLinkedList.h"
#include "mozilla/EnumSet.h"
#include "mozilla/Maybe.h"
#include "mozilla/TimeStamp.h"

#include "gc/ArenaList.h"
#include "gc/AtomMarking.h"
#include "gc/GCContext.h"
#include "gc/GCMarker.h"
#include "gc/GCParallelTask.h"
#include "gc/IteratorUtils.h"
#include "gc/Nursery.h"
#include "gc/Scheduling.h"
#include "gc/Statistics.h"
#include "gc/StoreBuffer.h"
#include "js/friend/PerformanceHint.h"
#include "js/GCAnnotations.h"
#include "js/UniquePtr.h"
#include "vm/AtomsTable.h"

namespace js {

class AutoLockGC;
class AutoLockGCBgAlloc;
class AutoLockHelperThreadState;
class FinalizationRegistryObject;
class FinalizationRecordObject;
class FinalizationQueueObject;
class GlobalObject;
class VerifyPreTracer;
class WeakRefObject;

namespace gc {

using BlackGrayEdgeVector = Vector<TenuredCell*, 0, SystemAllocPolicy>;
using ZoneVector = Vector<JS::Zone*, 4, SystemAllocPolicy>;

class AutoCallGCCallbacks;
class AutoGCSession;
class AutoHeapSession;
class AutoTraceSession;
struct FinalizePhase;
class MarkingValidator;
struct MovingTracer;
class ParallelMarkTask;
enum class ShouldCheckThresholds;
class SweepGroupsIter;

// Interface to a sweep action.
struct SweepAction {
  // The arguments passed to each action.
  struct Args {
    GCRuntime* gc;
    JS::GCContext* gcx;
    SliceBudget& budget;
  };

  virtual ~SweepAction() = default;
  virtual IncrementalProgress run(Args& state) = 0;
  virtual void assertFinished() const = 0;
  virtual bool shouldSkip() { return false; }
};

class ChunkPool {
  TenuredChunk* head_;
  size_t count_;

 public:
  ChunkPool() : head_(nullptr), count_(0) {}
  ChunkPool(const ChunkPool& other) = delete;
  ChunkPool(ChunkPool&& other) { *this = std::move(other); }

  ~ChunkPool() {
    MOZ_ASSERT(!head_);
    MOZ_ASSERT(count_ == 0);
  }

  ChunkPool& operator=(const ChunkPool& other) = delete;
  ChunkPool& operator=(ChunkPool&& other) {
    head_ = other.head_;
    other.head_ = nullptr;
    count_ = other.count_;
    other.count_ = 0;
    return *this;
  }

  bool empty() const { return !head_; }
  size_t count() const { return count_; }

  TenuredChunk* head() {
    MOZ_ASSERT(head_);
    return head_;
  }
  TenuredChunk* pop();
  void push(TenuredChunk* chunk);
  TenuredChunk* remove(TenuredChunk* chunk);

  void sort();

 private:
  TenuredChunk* mergeSort(TenuredChunk* list, size_t count);
  bool isSorted() const;

#ifdef DEBUG
 public:
  bool contains(TenuredChunk* chunk) const;
  bool verify() const;
  void verifyChunks() const;
#endif

 public:
  // Pool mutation does not invalidate an Iter unless the mutation
  // is of the TenuredChunk currently being visited by the Iter.
  class Iter {
   public:
    explicit Iter(ChunkPool& pool) : current_(pool.head_) {}
    bool done() const { return !current_; }
    void next();
    TenuredChunk* get() const { return current_; }
    operator TenuredChunk*() const { return get(); }
    TenuredChunk* operator->() const { return get(); }

   private:
    TenuredChunk* current_;
  };
};

class BackgroundMarkTask : public GCParallelTask {
 public:
  explicit BackgroundMarkTask(GCRuntime* gc);
  void setBudget(const SliceBudget& budget) { this->budget = budget; }
  void run(AutoLockHelperThreadState& lock) override;

 private:
  SliceBudget budget;
};

class BackgroundUnmarkTask : public GCParallelTask {
 public:
  explicit BackgroundUnmarkTask(GCRuntime* gc);
  void initZones();
  void run(AutoLockHelperThreadState& lock) override;

  ZoneVector zones;
};

class BackgroundSweepTask : public GCParallelTask {
 public:
  explicit BackgroundSweepTask(GCRuntime* gc);
  void run(AutoLockHelperThreadState& lock) override;
};

class BackgroundFreeTask : public GCParallelTask {
 public:
  explicit BackgroundFreeTask(GCRuntime* gc);
  void run(AutoLockHelperThreadState& lock) override;
};

// Performs extra allocation off thread so that when memory is required on the
// main thread it will already be available and waiting.
class BackgroundAllocTask : public GCParallelTask {
  // Guarded by the GC lock.
  GCLockData<ChunkPool&> chunkPool_;

  const bool enabled_;

 public:
  BackgroundAllocTask(GCRuntime* gc, ChunkPool& pool);
  bool enabled() const { return enabled_; }

  void run(AutoLockHelperThreadState& lock) override;
};

// Search the provided chunks for free arenas and decommit them.
class BackgroundDecommitTask : public GCParallelTask {
 public:
  explicit BackgroundDecommitTask(GCRuntime* gc);
  void run(AutoLockHelperThreadState& lock) override;
};

template <typename F>
struct Callback {
  F op;
  void* data;

  Callback() : op(nullptr), data(nullptr) {}
  Callback(F op, void* data) : op(op), data(data) {}
};

template <typename F>
using CallbackVector = Vector<Callback<F>, 4, SystemAllocPolicy>;

typedef HashMap<Value*, const char*, DefaultHasher<Value*>, SystemAllocPolicy>
    RootedValueMap;

using AllocKinds = mozilla::EnumSet<AllocKind, uint64_t>;

// A singly linked list of zones.
class ZoneList {
  static Zone* const End;

  Zone* head;
  Zone* tail;

 public:
  ZoneList();
  ~ZoneList();

  bool isEmpty() const;
  Zone* front() const;

  void prepend(Zone* zone);
  void append(Zone* zone);
  void prependList(ZoneList&& other);
  void appendList(ZoneList&& other);
  Zone* removeFront();
  void clear();

 private:
  explicit ZoneList(Zone* singleZone);
  void check() const;

  ZoneList(const ZoneList& other) = delete;
  ZoneList& operator=(const ZoneList& other) = delete;
};

struct WeakCacheToSweep {
  JS::detail::WeakCacheBase* cache;
  JS::Zone* zone;
};

class WeakCacheSweepIterator {
  using WeakCacheBase = JS::detail::WeakCacheBase;

  JS::Zone* sweepZone;
  WeakCacheBase* sweepCache;

 public:
  explicit WeakCacheSweepIterator(JS::Zone* sweepGroup);

  bool done() const;
  WeakCacheToSweep get() const;
  void next();

 private:
  void settle();
};

struct SweepingTracer final : public GenericTracerImpl<SweepingTracer> {
  explicit SweepingTracer(JSRuntime* rt);

 private:
  template <typename T>
  void onEdge(T** thingp, const char* name);
  friend class GenericTracerImpl<SweepingTracer>;
};

class GCRuntime {
 public:
  explicit GCRuntime(JSRuntime* rt);
  [[nodiscard]] bool init(uint32_t maxbytes);
  bool wasInitialized() const { return initialized; }
  void finishRoots();
  void finish();

  Zone* atomsZone() {
    Zone* zone = zones()[0];
    MOZ_ASSERT(JS::shadow::Zone::from(zone)->isAtomsZone());
    return zone;
  }
  Zone* maybeSharedAtomsZone() { return sharedAtomsZone_; }

  [[nodiscard]] bool freezeSharedAtomsZone();
  void restoreSharedAtomsZone();

  JS::HeapState heapState() const { return heapState_; }

  inline bool hasZealMode(ZealMode mode);
  inline void clearZealMode(ZealMode mode);
  inline bool needZealousGC();
  inline bool hasIncrementalTwoSliceZealMode();

  [[nodiscard]] bool addRoot(Value* vp, const char* name);
  void removeRoot(Value* vp);

  [[nodiscard]] bool setParameter(JSContext* cx, JSGCParamKey key,
                                  uint32_t value);
  void resetParameter(JSContext* cx, JSGCParamKey key);
  uint32_t getParameter(JSGCParamKey key);

  void setPerformanceHint(PerformanceHint hint);
  bool isInPageLoad() const { return inPageLoadCount != 0; }

  [[nodiscard]] bool triggerGC(JS::GCReason reason);
  // Check whether to trigger a zone GC after allocating GC cells.
  void maybeTriggerGCAfterAlloc(Zone* zone);
  // Check whether to trigger a zone GC after malloc memory.
  void maybeTriggerGCAfterMalloc(Zone* zone);
  bool maybeTriggerGCAfterMalloc(Zone* zone, const HeapSize& heap,
                                 const HeapThreshold& threshold,
                                 JS::GCReason reason);
  // The return value indicates if we were able to do the GC.
  bool triggerZoneGC(Zone* zone, JS::GCReason reason, size_t usedBytes,
                     size_t thresholdBytes);

  void maybeGC();

  // Return whether we want to run a major GC. If eagerOk is true, include eager
  // triggers (eg EAGER_ALLOC_TRIGGER) in this determination, and schedule all
  // zones that exceed the eager thresholds.
  JS::GCReason wantMajorGC(bool eagerOk);
  bool checkEagerAllocTrigger(const HeapSize& size,
                              const HeapThreshold& threshold);

  // Do a minor GC if requested, followed by a major GC if requested. The return
  // value indicates whether a major GC was performed.
  bool gcIfRequested() { return gcIfRequestedImpl(false); }

  // Internal function to do a GC if previously requested. But if not and
  // eagerOk, do an eager GC for all Zones that have exceeded the eager
  // thresholds.
  //
  // Return whether a major GC was performed or started.
  bool gcIfRequestedImpl(bool eagerOk);

  void gc(JS::GCOptions options, JS::GCReason reason);
  void startGC(JS::GCOptions options, JS::GCReason reason,
               const SliceBudget& budget);
  void gcSlice(JS::GCReason reason, const SliceBudget& budget);
  void finishGC(JS::GCReason reason);
  void abortGC();
  void startDebugGC(JS::GCOptions options, const SliceBudget& budget);
  void debugGCSlice(const SliceBudget& budget);

  void runDebugGC();
  void notifyRootsRemoved();

  enum TraceOrMarkRuntime { TraceRuntime, MarkRuntime };
  void traceRuntime(JSTracer* trc, AutoTraceSession& session);
  void traceRuntimeForMinorGC(JSTracer* trc, AutoGCSession& session);

  void purgeRuntimeForMinorGC();

  void shrinkBuffers();
  void onOutOfMallocMemory();
  void onOutOfMallocMemory(const AutoLockGC& lock);

  Nursery& nursery() { return nursery_.ref(); }
  gc::StoreBuffer& storeBuffer() { return storeBuffer_.ref(); }

  void minorGC(JS::GCReason reason,
               gcstats::PhaseKind phase = gcstats::PhaseKind::MINOR_GC)
      JS_HAZ_GC_CALL;
  void evictNursery(JS::GCReason reason = JS::GCReason::EVICT_NURSERY) {
    minorGC(reason, gcstats::PhaseKind::EVICT_NURSERY);
  }

  void* addressOfNurseryPosition() {
    return nursery_.refNoCheck().addressOfPosition();
  }

  const void* addressOfLastBufferedWholeCell() {
    return storeBuffer_.refNoCheck().addressOfLastBufferedWholeCell();
  }

#ifdef JS_GC_ZEAL
  const uint32_t* addressOfZealModeBits() { return &zealModeBits.refNoCheck(); }
  void getZealBits(uint32_t* zealBits, uint32_t* frequency,
                   uint32_t* nextScheduled);
  void setZeal(uint8_t zeal, uint32_t frequency);
  void unsetZeal(uint8_t zeal);
  bool parseAndSetZeal(const char* str);
  void setNextScheduled(uint32_t count);
  void verifyPreBarriers();
  void maybeVerifyPreBarriers(bool always);
  bool selectForMarking(JSObject* object);
  void clearSelectedForMarking();
  void setDeterministic(bool enable);
  void setMarkStackLimit(size_t limit, AutoLockGC& lock);
#endif

  uint64_t nextCellUniqueId() {
    MOZ_ASSERT(nextCellUniqueId_ > 0);
    uint64_t uid = ++nextCellUniqueId_;
    return uid;
  }

  void setLowMemoryState(bool newState) { lowMemoryState = newState; }
  bool systemHasLowMemory() const { return lowMemoryState; }

 public:
  // Internal public interface
  ZoneVector& zones() { return zones_.ref(); }
  gcstats::Statistics& stats() { return stats_.ref(); }
  const gcstats::Statistics& stats() const { return stats_.ref(); }
  State state() const { return incrementalState; }
  bool isHeapCompacting() const { return state() == State::Compact; }
  bool isForegroundSweeping() const { return state() == State::Sweep; }
  bool isBackgroundSweeping() const { return sweepTask.wasStarted(); }
  bool isBackgroundMarking() const { return markTask.wasStarted(); }
  void waitBackgroundSweepEnd();
  void waitBackgroundAllocEnd() { allocTask.cancelAndWait(); }
  void waitBackgroundFreeEnd();
  void waitForBackgroundTasks();
  bool isWaitingOnBackgroundTask() const;

  void lockGC() { lock.lock(); }
  bool tryLockGC() { return lock.tryLock(); }
  void unlockGC() { lock.unlock(); }

#ifdef DEBUG
  void assertCurrentThreadHasLockedGC() const {
    lock.assertOwnedByCurrentThread();
  }
#endif  // DEBUG

  void setAlwaysPreserveCode() { alwaysPreserveCode = true; }

  bool isIncrementalGCAllowed() const { return incrementalAllowed; }
  void disallowIncrementalGC() { incrementalAllowed = false; }

  void setIncrementalGCEnabled(bool enabled);

  bool isIncrementalGCEnabled() const { return incrementalGCEnabled; }
  bool isPerZoneGCEnabled() const { return perZoneGCEnabled; }
  bool isCompactingGCEnabled() const;
  bool isParallelMarkingEnabled() const { return parallelMarkingEnabled; }

  bool isIncrementalGCInProgress() const {
    return state() != State::NotActive && !isVerifyPreBarriersEnabled();
  }

  bool hasForegroundWork() const;

  bool isShrinkingGC() const { return gcOptions() == JS::GCOptions::Shrink; }

  bool isShutdownGC() const { return gcOptions() == JS::GCOptions::Shutdown; }

#ifdef DEBUG
  bool isShuttingDown() const { return hadShutdownGC; }
#endif

  bool initSweepActions();

  void setGrayRootsTracer(JSGrayRootsTracer traceOp, void* data);
  [[nodiscard]] bool addBlackRootsTracer(JSTraceDataOp traceOp, void* data);
  void removeBlackRootsTracer(JSTraceDataOp traceOp, void* data);
  void clearBlackAndGrayRootTracers();

  void setGCCallback(JSGCCallback callback, void* data);
  void callGCCallback(JSGCStatus status, JS::GCReason reason) const;
  void setObjectsTenuredCallback(JSObjectsTenuredCallback callback, void* data);
  void callObjectsTenuredCallback();
  [[nodiscard]] bool addFinalizeCallback(JSFinalizeCallback callback,
                                         void* data);
  void removeFinalizeCallback(JSFinalizeCallback func);
  void setHostCleanupFinalizationRegistryCallback(
      JSHostCleanupFinalizationRegistryCallback callback, void* data);
  void callHostCleanupFinalizationRegistryCallback(
      JSFunction* doCleanup, GlobalObject* incumbentGlobal);
  [[nodiscard]] bool addWeakPointerZonesCallback(
      JSWeakPointerZonesCallback callback, void* data);
  void removeWeakPointerZonesCallback(JSWeakPointerZonesCallback callback);
  [[nodiscard]] bool addWeakPointerCompartmentCallback(
      JSWeakPointerCompartmentCallback callback, void* data);
  void removeWeakPointerCompartmentCallback(
      JSWeakPointerCompartmentCallback callback);
  JS::GCSliceCallback setSliceCallback(JS::GCSliceCallback callback);
  JS::GCNurseryCollectionCallback setNurseryCollectionCallback(
      JS::GCNurseryCollectionCallback callback);
  JS::DoCycleCollectionCallback setDoCycleCollectionCallback(
      JS::DoCycleCollectionCallback callback);

  bool addFinalizationRegistry(JSContext* cx,
                               Handle<FinalizationRegistryObject*> registry);
  bool registerWithFinalizationRegistry(JSContext* cx, HandleObject target,
                                        HandleObject record);
  void queueFinalizationRegistryForCleanup(FinalizationQueueObject* queue);

  void nukeFinalizationRecordWrapper(JSObject* wrapper,
                                     FinalizationRecordObject* record);
  void nukeWeakRefWrapper(JSObject* wrapper, WeakRefObject* record);

  void setFullCompartmentChecks(bool enable);

  // Get the main marking tracer.
  GCMarker& marker() { return *markers[0]; }

  JS::Zone* getCurrentSweepGroup() { return currentSweepGroup; }
  unsigned getCurrentSweepGroupIndex() {
    MOZ_ASSERT_IF(unsigned(state()) < unsigned(State::Sweep),
                  sweepGroupIndex == 0);
    return sweepGroupIndex;
  }

  uint64_t gcNumber() const { return number; }
  void incGcNumber() { ++number; }

  uint64_t minorGCCount() const { return minorGCNumber; }
  void incMinorGcNumber() { ++minorGCNumber; }

  uint64_t majorGCCount() const { return majorGCNumber; }
  void incMajorGcNumber() { ++majorGCNumber; }

  uint64_t gcSliceCount() const { return sliceNumber; }
  void incGcSliceNumber() { ++sliceNumber; }

  int64_t defaultSliceBudgetMS() const { return defaultTimeBudgetMS_; }

  bool isIncrementalGc() const { return isIncremental; }
  bool isFullGc() const { return isFull; }
  bool isCompactingGc() const { return isCompacting; }
  bool didCompactZones() const { return isCompacting && zonesCompacted; }

  bool areGrayBitsValid() const { return grayBitsValid; }
  void setGrayBitsInvalid() { grayBitsValid = false; }

  mozilla::TimeStamp lastGCStartTime() const { return lastGCStartTime_; }
  mozilla::TimeStamp lastGCEndTime() const { return lastGCEndTime_; }

  bool majorGCRequested() const {
    return majorGCTriggerReason != JS::GCReason::NO_REASON;
  }

  double computeHeapGrowthFactor(size_t lastBytes);
  size_t computeTriggerBytes(double growthFactor, size_t lastBytes);

  inline void updateOnFreeArenaAlloc(const TenuredChunkInfo& info);
  void updateOnArenaFree() { ++numArenasFreeCommitted; }

  ChunkPool& fullChunks(const AutoLockGC& lock) { return fullChunks_.ref(); }
  ChunkPool& availableChunks(const AutoLockGC& lock) {
    return availableChunks_.ref();
  }
  ChunkPool& emptyChunks(const AutoLockGC& lock) { return emptyChunks_.ref(); }
  const ChunkPool& fullChunks(const AutoLockGC& lock) const {
    return fullChunks_.ref();
  }
  const ChunkPool& availableChunks(const AutoLockGC& lock) const {
    return availableChunks_.ref();
  }
  const ChunkPool& emptyChunks(const AutoLockGC& lock) const {
    return emptyChunks_.ref();
  }
  using NonEmptyChunksIter = ChainedIterator<ChunkPool::Iter, 2>;
  NonEmptyChunksIter allNonEmptyChunks(const AutoLockGC& lock) {
    return NonEmptyChunksIter(availableChunks(lock), fullChunks(lock));
  }
  uint32_t minEmptyChunkCount(const AutoLockGC& lock) const {
    return minEmptyChunkCount_;
  }
  uint32_t maxEmptyChunkCount(const AutoLockGC& lock) const {
    return maxEmptyChunkCount_;
  }
#ifdef DEBUG
  void verifyAllChunks();
#endif

  TenuredChunk* getOrAllocChunk(AutoLockGCBgAlloc& lock);
  void recycleChunk(TenuredChunk* chunk, const AutoLockGC& lock);

#ifdef JS_GC_ZEAL
  void startVerifyPreBarriers();
  void endVerifyPreBarriers();
  void finishVerifier();
  bool isVerifyPreBarriersEnabled() const { return verifyPreData.refNoCheck(); }
  bool shouldYieldForZeal(ZealMode mode);
#else
  bool isVerifyPreBarriersEnabled() const { return false; }
#endif

#ifdef JSGC_HASH_TABLE_CHECKS
  void checkHashTablesAfterMovingGC();
#endif

#ifdef DEBUG
  // Crawl the heap to check whether an arbitary pointer is within a cell of
  // the given kind.
  bool isPointerWithinTenuredCell(void* ptr, JS::TraceKind traceKind);

  bool hasZone(Zone* target);
#endif

  // Queue memory memory to be freed on a background thread if possible.
  void queueUnusedLifoBlocksForFree(LifoAlloc* lifo);
  void queueAllLifoBlocksForFreeAfterMinorGC(LifoAlloc* lifo);
  void queueBuffersForFreeAfterMinorGC(Nursery::BufferSet& buffers);

  // Public here for ReleaseArenaLists and FinalizeTypedArenas.
  void releaseArena(Arena* arena, const AutoLockGC& lock);

  // Allocator
  template <AllowGC allowGC>
  [[nodiscard]] bool checkAllocatorState(JSContext* cx, AllocKind kind);
  template <JS::TraceKind kind, AllowGC allowGC>
  void* tryNewNurseryCell(JSContext* cx, size_t thingSize, AllocSite* site);
  template <AllowGC allowGC>
  static void* tryNewTenuredThing(JSContext* cx, AllocKind kind,
                                  size_t thingSize);
  static void* refillFreeListInGC(Zone* zone, AllocKind thingKind);

  // Delayed marking.
  void delayMarkingChildren(gc::Cell* cell, MarkColor color);
  bool hasDelayedMarking() const;
  void markAllDelayedChildren(ShouldReportMarkTime reportTime);

  /*
   * Concurrent sweep infrastructure.
   */
  void startTask(GCParallelTask& task, AutoLockHelperThreadState& lock);
  void joinTask(GCParallelTask& task, AutoLockHelperThreadState& lock);
  void updateHelperThreadCount();
  bool updateMarkersVector();
  size_t parallelWorkerCount() const;
  size_t markingWorkerCount() const;

  // WeakRefs
  bool registerWeakRef(HandleObject target, HandleObject weakRef);
  void traceKeptObjects(JSTracer* trc);

  JS::GCReason lastStartReason() const { return initialReason; }

  void updateAllocationRates();

#ifdef DEBUG
  const GCVector<HeapPtr<JS::Value>, 0, SystemAllocPolicy>& getTestMarkQueue()
      const;
  [[nodiscard]] bool appendTestMarkQueue(const JS::Value& value);
  void clearTestMarkQueue();
  size_t testMarkQueuePos() const;
#endif

 private:
  enum IncrementalResult { ResetIncremental = 0, Ok };

  [[nodiscard]] bool setParameter(JSGCParamKey key, uint32_t value,
                                  AutoLockGC& lock);
  void resetParameter(JSGCParamKey key, AutoLockGC& lock);
  uint32_t getParameter(JSGCParamKey key, const AutoLockGC& lock);
  bool setThreadParameter(JSGCParamKey key, uint32_t value, AutoLockGC& lock);
  void resetThreadParameter(JSGCParamKey key, AutoLockGC& lock);
  void updateThreadDataStructures(AutoLockGC& lock);

  JS::GCOptions gcOptions() const { return maybeGcOptions.ref().ref(); }

  TriggerResult checkHeapThreshold(Zone* zone, const HeapSize& heapSize,
                                   const HeapThreshold& heapThreshold);

  void updateSchedulingStateOnGCStart();
  void updateSchedulingStateAfterCollection(mozilla::TimeStamp currentTime);
  void updateAllGCStartThresholds();

  // For ArenaLists::allocateFromArena()
  friend class ArenaLists;
  TenuredChunk* pickChunk(AutoLockGCBgAlloc& lock);
  Arena* allocateArena(TenuredChunk* chunk, Zone* zone, AllocKind kind,
                       ShouldCheckThresholds checkThresholds,
                       const AutoLockGC& lock);

  // Allocator internals
  void gcIfNeededAtAllocation(JSContext* cx);
  static void* refillFreeList(JSContext* cx, AllocKind thingKind);
  void attemptLastDitchGC(JSContext* cx);
#ifdef DEBUG
  static void checkIncrementalZoneState(JSContext* cx, void* ptr);
#endif

  /*
   * Return the list of chunks that can be released outside the GC lock.
   * Must be called either during the GC or with the GC lock taken.
   */
  friend class BackgroundDecommitTask;
  bool tooManyEmptyChunks(const AutoLockGC& lock);
  ChunkPool expireEmptyChunkPool(const AutoLockGC& lock);
  void freeEmptyChunks(const AutoLockGC& lock);
  void prepareToFreeChunk(TenuredChunkInfo& info);
  void setMinEmptyChunkCount(uint32_t value, const AutoLockGC& lock);
  void setMaxEmptyChunkCount(uint32_t value, const AutoLockGC& lock);

  friend class BackgroundAllocTask;
  bool wantBackgroundAllocation(const AutoLockGC& lock) const;
  void startBackgroundAllocTaskIfIdle();

  void requestMajorGC(JS::GCReason reason);
  SliceBudget defaultBudget(JS::GCReason reason, int64_t millis);
  bool maybeIncreaseSliceBudget(SliceBudget& budget);
  bool maybeIncreaseSliceBudgetForLongCollections(SliceBudget& budget);
  bool maybeIncreaseSliceBudgetForUrgentCollections(SliceBudget& budget);
  IncrementalResult budgetIncrementalGC(bool nonincrementalByAPI,
                                        JS::GCReason reason,
                                        SliceBudget& budget);
  void checkZoneIsScheduled(Zone* zone, JS::GCReason reason,
                            const char* trigger);
  IncrementalResult resetIncrementalGC(GCAbortReason reason);

  // Assert if the system state is such that we should never
  // receive a request to do GC work.
  void checkCanCallAPI();

  // Check if the system state is such that GC has been supressed
  // or otherwise delayed.
  [[nodiscard]] bool checkIfGCAllowedInCurrentState(JS::GCReason reason);

  gcstats::ZoneGCStats scanZonesBeforeGC();

  void setGCOptions(JS::GCOptions options);

  void collect(bool nonincrementalByAPI, const SliceBudget& budget,
               JS::GCReason reason) JS_HAZ_GC_CALL;

  /*
   * Run one GC "cycle" (either a slice of incremental GC or an entire
   * non-incremental GC).
   *
   * Returns:
   *  * ResetIncremental if we "reset" an existing incremental GC, which would
   *    force us to run another cycle or
   *  * Ok otherwise.
   */
  [[nodiscard]] IncrementalResult gcCycle(bool nonincrementalByAPI,
                                          const SliceBudget& budgetArg,
                                          JS::GCReason reason);
  bool shouldRepeatForDeadZone(JS::GCReason reason);

  void incrementalSlice(SliceBudget& budget, JS::GCReason reason,
                        bool budgetWasIncreased);

  bool mightSweepInThisSlice(bool nonIncremental);
  void collectNurseryFromMajorGC(JS::GCReason reason);
  void collectNursery(JS::GCOptions options, JS::GCReason reason,
                      gcstats::PhaseKind phase);

  friend class AutoCallGCCallbacks;
  void maybeCallGCCallback(JSGCStatus status, JS::GCReason reason);

  void startCollection(JS::GCReason reason);

  void purgeRuntime();
  [[nodiscard]] bool beginPreparePhase(JS::GCReason reason,
                                       AutoGCSession& session);
  bool prepareZonesForCollection(JS::GCReason reason, bool* isFullOut);
  void unmarkWeakMaps();
  void endPreparePhase(JS::GCReason reason);
  void beginMarkPhase(AutoGCSession& session);
  bool shouldPreserveJITCode(JS::Realm* realm,
                             const mozilla::TimeStamp& currentTime,
                             JS::GCReason reason, bool canAllocateMoreCode,
                             bool isActiveCompartment);
  void discardJITCodeForGC();
  void startBackgroundFreeAfterMinorGC();
  void relazifyFunctionsForShrinkingGC();
  void purgePropMapTablesForShrinkingGC();
  void purgeSourceURLsForShrinkingGC();
  void traceRuntimeForMajorGC(JSTracer* trc, AutoGCSession& session);
  void traceRuntimeAtoms(JSTracer* trc);
  void traceRuntimeCommon(JSTracer* trc, TraceOrMarkRuntime traceOrMark);
  void traceEmbeddingBlackRoots(JSTracer* trc);
  void traceEmbeddingGrayRoots(JSTracer* trc);
  IncrementalProgress traceEmbeddingGrayRoots(JSTracer* trc,
                                              SliceBudget& budget);
  void checkNoRuntimeRoots(AutoGCSession& session);
  void maybeDoCycleCollection();
  void findDeadCompartments();

  friend class BackgroundMarkTask;
  enum ParallelMarking : bool {
    SingleThreadedMarking = false,
    AllowParallelMarking = true
  };
  IncrementalProgress markUntilBudgetExhausted(
      SliceBudget& sliceBudget,
      ParallelMarking allowParallelMarking = SingleThreadedMarking,
      ShouldReportMarkTime reportTime = ReportMarkTime);
  bool canMarkInParallel() const;

  bool hasMarkingWork(MarkColor color) const;

  void drainMarkStack();

#ifdef DEBUG
  void assertNoMarkingWork() const;
#else
  void assertNoMarkingWork() const {}
#endif

  void markDelayedChildren(gc::Arena* arena, MarkColor color);
  void processDelayedMarkingList(gc::MarkColor color);
  void rebuildDelayedMarkingList();
  void appendToDelayedMarkingList(gc::Arena** listTail, gc::Arena* arena);
  void resetDelayedMarking();
  template <typename F>
  void forEachDelayedMarkingArena(F&& f);

  template <class ZoneIterT>
  IncrementalProgress markWeakReferences(SliceBudget& budget);
  IncrementalProgress markWeakReferencesInCurrentGroup(SliceBudget& budget);
  template <class ZoneIterT>
  IncrementalProgress markGrayRoots(SliceBudget& budget,
                                    gcstats::PhaseKind phase);
  void markBufferedGrayRoots(JS::Zone* zone);
  IncrementalProgress markAllWeakReferences();
  void markAllGrayReferences(gcstats::PhaseKind phase);

  // The mark queue is a testing-only feature for controlling mark ordering and
  // yield timing.
  enum MarkQueueProgress {
    QueueYielded,   // End this incremental GC slice, if possible
    QueueComplete,  // Done with the queue
    QueueSuspended  // Continue the GC without ending the slice
  };
  MarkQueueProgress processTestMarkQueue();

  // GC Sweeping. Implemented in Sweeping.cpp.
  void beginSweepPhase(JS::GCReason reason, AutoGCSession& session);
  void dropStringWrappers();
  void groupZonesForSweeping(JS::GCReason reason);
  [[nodiscard]] bool findSweepGroupEdges();
  [[nodiscard]] bool addEdgesForMarkQueue();
  void getNextSweepGroup();
  void resetGrayList(Compartment* comp);
  IncrementalProgress beginMarkingSweepGroup(JS::GCContext* gcx,
                                             SliceBudget& budget);
  IncrementalProgress markGrayRootsInCurrentGroup(JS::GCContext* gcx,
                                                  SliceBudget& budget);
  IncrementalProgress markGray(JS::GCContext* gcx, SliceBudget& budget);
  IncrementalProgress endMarkingSweepGroup(JS::GCContext* gcx,
                                           SliceBudget& budget);
  void markIncomingGrayCrossCompartmentPointers();
  IncrementalProgress beginSweepingSweepGroup(JS::GCContext* gcx,
                                              SliceBudget& budget);
  void initBackgroundSweep(Zone* zone, JS::GCContext* gcx,
                           const FinalizePhase& phase);
  IncrementalProgress markDuringSweeping(JS::GCContext* gcx,
                                         SliceBudget& budget);
  void updateAtomsBitmap();
  void sweepCCWrappers();
  void sweepRealmGlobals();
  void sweepEmbeddingWeakPointers(JS::GCContext* gcx);
  void sweepMisc();
  void sweepCompressionTasks();
  void sweepWeakMaps();
  void sweepUniqueIds();
  void sweepDebuggerOnMainThread(JS::GCContext* gcx);
  void sweepJitDataOnMainThread(JS::GCContext* gcx);
  void sweepFinalizationObserversOnMainThread();
  void traceWeakFinalizationObserverEdges(JSTracer* trc, Zone* zone);
  void sweepWeakRefs();
  IncrementalProgress endSweepingSweepGroup(JS::GCContext* gcx,
                                            SliceBudget& budget);
  IncrementalProgress performSweepActions(SliceBudget& sliceBudget);
  void startSweepingAtomsTable();
  IncrementalProgress sweepAtomsTable(JS::GCContext* gcx, SliceBudget& budget);
  IncrementalProgress sweepWeakCaches(JS::GCContext* gcx, SliceBudget& budget);
  IncrementalProgress finalizeAllocKind(JS::GCContext* gcx,
                                        SliceBudget& budget);
  bool foregroundFinalize(JS::GCContext* gcx, Zone* zone, AllocKind thingKind,
                          js::SliceBudget& sliceBudget,
                          SortedArenaList& sweepList);
  IncrementalProgress sweepPropMapTree(JS::GCContext* gcx, SliceBudget& budget);
  void endSweepPhase(bool lastGC);
  void queueZonesAndStartBackgroundSweep(ZoneList&& zones);
  void sweepFromBackgroundThread(AutoLockHelperThreadState& lock);
  void startBackgroundFree();
  void freeFromBackgroundThread(AutoLockHelperThreadState& lock);
  void sweepBackgroundThings(ZoneList& zones);
  void backgroundFinalize(JS::GCContext* gcx, Zone* zone, AllocKind kind,
                          Arena** empty);
  void assertBackgroundSweepingFinished();

  bool allCCVisibleZonesWereCollected();
  void sweepZones(JS::GCContext* gcx, bool destroyingRuntime);
  bool shouldDecommit() const;
  void startDecommit();
  void decommitEmptyChunks(const bool& cancel, AutoLockGC& lock);
  void decommitFreeArenas(const bool& cancel, AutoLockGC& lock);
  void decommitFreeArenasWithoutUnlocking(const AutoLockGC& lock);

  // Compacting GC. Implemented in Compacting.cpp.
  bool shouldCompact();
  void beginCompactPhase();
  IncrementalProgress compactPhase(JS::GCReason reason,
                                   SliceBudget& sliceBudget,
                                   AutoGCSession& session);
  void endCompactPhase();
  void sweepZoneAfterCompacting(MovingTracer* trc, Zone* zone);
  bool canRelocateZone(Zone* zone) const;
  [[nodiscard]] bool relocateArenas(Zone* zone, JS::GCReason reason,
                                    Arena*& relocatedListOut,
                                    SliceBudget& sliceBudget);
  void updateCellPointers(Zone* zone, AllocKinds kinds);
  void updateAllCellPointers(MovingTracer* trc, Zone* zone);
  void updateZonePointersToRelocatedCells(Zone* zone);
  void updateRuntimePointersToRelocatedCells(AutoGCSession& session);
  void clearRelocatedArenas(Arena* arenaList, JS::GCReason reason);
  void clearRelocatedArenasWithoutUnlocking(Arena* arenaList,
                                            JS::GCReason reason,
                                            const AutoLockGC& lock);
  void releaseRelocatedArenas(Arena* arenaList);
  void releaseRelocatedArenasWithoutUnlocking(Arena* arenaList,
                                              const AutoLockGC& lock);
#ifdef DEBUG
  void protectOrReleaseRelocatedArenas(Arena* arenaList, JS::GCReason reason);
  void protectAndHoldArenas(Arena* arenaList);
  void unprotectHeldRelocatedArenas(const AutoLockGC& lock);
  void releaseHeldRelocatedArenas();
  void releaseHeldRelocatedArenasWithoutUnlocking(const AutoLockGC& lock);
#endif

  /*
   * Whether to immediately trigger a slice after a background task
   * finishes. This may not happen at a convenient time, so the consideration is
   * whether the slice will run quickly or may take a long time.
   */
  enum ShouldTriggerSliceWhenFinished : bool {
    DontTriggerSliceWhenFinished = false,
    TriggerSliceWhenFinished = true
  };

  IncrementalProgress waitForBackgroundTask(
      GCParallelTask& task, const SliceBudget& budget, bool shouldPauseMutator,
      ShouldTriggerSliceWhenFinished triggerSlice);

  void maybeRequestGCAfterBackgroundTask(const AutoLockHelperThreadState& lock);
  void cancelRequestedGCAfterBackgroundTask();
  void finishCollection(JS::GCReason reason);
  void maybeStopPretenuring();
  void checkGCStateNotInUse();
  IncrementalProgress joinBackgroundMarkTask();

#ifdef JS_GC_ZEAL
  void computeNonIncrementalMarkingForValidation(AutoGCSession& session);
  void validateIncrementalMarking();
  void finishMarkingValidation();
#endif

#ifdef DEBUG
  void checkForCompartmentMismatches();
#endif

  void callFinalizeCallbacks(JS::GCContext* gcx, JSFinalizeStatus status) const;
  void callWeakPointerZonesCallbacks(JSTracer* trc) const;
  void callWeakPointerCompartmentCallbacks(JSTracer* trc,
                                           JS::Compartment* comp) const;
  void callDoCycleCollectionCallback(JSContext* cx);

 public:
  JSRuntime* const rt;

  // Embedders can use this zone however they wish.
  MainThreadData<JS::Zone*> systemZone;

  MainThreadData<JS::GCContext> mainThreadContext;

 private:
  // For parent runtimes, a zone containing atoms that is shared by child
  // runtimes.
  MainThreadData<Zone*> sharedAtomsZone_;

  // All zones in the runtime. The first element is always the atoms zone.
  MainThreadOrGCTaskData<ZoneVector> zones_;

  // Any activity affecting the heap.
  MainThreadOrGCTaskData<JS::HeapState> heapState_;
  friend class AutoHeapSession;
  friend class JS::AutoEnterCycleCollection;

  UnprotectedData<gcstats::Statistics> stats_;

 public:
  js::StringStats stringStats;

  Vector<UniquePtr<GCMarker>, 1, SystemAllocPolicy> markers;

  // Delayed marking support in case we OOM pushing work onto the mark stack.
  MainThreadOrGCTaskData<js::gc::Arena*> delayedMarkingList;
  MainThreadOrGCTaskData<bool> delayedMarkingWorkAdded;
#ifdef DEBUG
  /* Count of arenas that are currently in the stack. */
  MainThreadOrGCTaskData<size_t> markLaterArenas;
#endif

  SweepingTracer sweepingTracer;

  /* Track total GC heap size for this runtime. */
  HeapSize heapSize;

  /* GC scheduling state and parameters. */
  GCSchedulingTunables tunables;
  GCSchedulingState schedulingState;
  MainThreadData<bool> fullGCRequested;

  // Helper thread configuration.
  MainThreadData<double> helperThreadRatio;
  MainThreadData<size_t> maxHelperThreads;
  MainThreadOrGCTaskData<size_t> helperThreadCount;
  MainThreadData<size_t> markingThreadCount;

  // State used for managing atom mark bitmaps in each zone.
  AtomMarkingRuntime atomMarking;

  /*
   * Pointer to a callback that, if set, will be used to create a
   * budget for internally-triggered GCs.
   */
  MainThreadData<JS::CreateSliceBudgetCallback> createBudgetCallback;

 private:
  // Arenas used for permanent things created at startup and shared by child
  // runtimes.
  MainThreadData<ArenaList> permanentAtoms;
  MainThreadData<ArenaList> permanentFatInlineAtoms;
  MainThreadData<ArenaList> permanentWellKnownSymbols;

  // When chunks are empty, they reside in the emptyChunks pool and are
  // re-used as needed or eventually expired if not re-used. The emptyChunks
  // pool gets refilled from the background allocation task heuristically so
  // that empty chunks should always be available for immediate allocation
  // without syscalls.
  GCLockData<ChunkPool> emptyChunks_;

  // Chunks which have had some, but not all, of their arenas allocated live
  // in the available chunk lists. When all available arenas in a chunk have
  // been allocated, the chunk is removed from the available list and moved
  // to the fullChunks pool. During a GC, if all arenas are free, the chunk
  // is moved back to the emptyChunks pool and scheduled for eventual
  // release.
  GCLockData<ChunkPool> availableChunks_;

  // When all arenas in a chunk are used, it is moved to the fullChunks pool
  // so as to reduce the cost of operations on the available lists.
  GCLockData<ChunkPool> fullChunks_;

  /*
   * JSGC_MIN_EMPTY_CHUNK_COUNT
   * JSGC_MAX_EMPTY_CHUNK_COUNT
   *
   * Controls the number of empty chunks reserved for future allocation.
   *
   * They can be read off main thread by the background allocation task and the
   * background decommit task.
   */
  GCLockData<uint32_t> minEmptyChunkCount_;
  GCLockData<uint32_t> maxEmptyChunkCount_;

  MainThreadData<RootedValueMap> rootsHash;

  // An incrementing id used to assign unique ids to cells that require one.
  MainThreadData<uint64_t> nextCellUniqueId_;

  /*
   * Number of the committed arenas in all GC chunks including empty chunks.
   */
  mozilla::Atomic<uint32_t, mozilla::ReleaseAcquire> numArenasFreeCommitted;
  MainThreadData<VerifyPreTracer*> verifyPreData;

  MainThreadData<mozilla::TimeStamp> lastGCStartTime_;
  MainThreadData<mozilla::TimeStamp> lastGCEndTime_;

  WriteOnceData<bool> initialized;
  MainThreadData<bool> incrementalGCEnabled;
  MainThreadData<bool> perZoneGCEnabled;

  mozilla::Atomic<size_t, mozilla::ReleaseAcquire> numActiveZoneIters;

  /* During shutdown, the GC needs to clean up every possible object. */
  MainThreadData<bool> cleanUpEverything;

  /*
   * The gray bits can become invalid if UnmarkGray overflows the stack. A
   * full GC will reset this bit, since it fills in all the gray bits.
   */
  UnprotectedData<bool> grayBitsValid;

  mozilla::Atomic<JS::GCReason, mozilla::ReleaseAcquire> majorGCTriggerReason;

  /* Incremented at the start of every minor GC. */
  MainThreadData<uint64_t> minorGCNumber;

  /* Incremented at the start of every major GC. */
  MainThreadData<uint64_t> majorGCNumber;

  /* Incremented on every GC slice or minor collection. */
  MainThreadData<uint64_t> number;

  /* Incremented on every GC slice. */
  MainThreadData<uint64_t> sliceNumber;

  /* Whether the currently running GC can finish in multiple slices. */
  MainThreadOrGCTaskData<bool> isIncremental;

  /* Whether all zones are being collected in first GC slice. */
  MainThreadData<bool> isFull;

  /* Whether the heap will be compacted at the end of GC. */
  MainThreadData<bool> isCompacting;

  /* The invocation kind of the current GC, set at the start of collection. */
  MainThreadOrGCTaskData<mozilla::Maybe<JS::GCOptions>> maybeGcOptions;

  /* The initial GC reason, taken from the first slice. */
  MainThreadData<JS::GCReason> initialReason;

  /*
   * The current incremental GC phase. This is also used internally in
   * non-incremental GC.
   */
  MainThreadOrGCTaskData<State> incrementalState;

  /* The incremental state at the start of this slice. */
  MainThreadOrGCTaskData<State> initialState;

  /* Whether to pay attention the zeal settings in this incremental slice. */
#ifdef JS_GC_ZEAL
  MainThreadData<bool> useZeal;
#else
  const bool useZeal;
#endif

  /* Indicates that the last incremental slice exhausted the mark stack. */
  MainThreadData<bool> lastMarkSlice;

  // Whether it's currently safe to yield to the mutator in an incremental GC.
  MainThreadData<bool> safeToYield;

  // Whether to do any marking caused by barriers on a background thread during
  // incremental sweeping, while also sweeping zones which have finished
  // marking.
  MainThreadData<bool> markOnBackgroundThreadDuringSweeping;

  // Whether any sweeping and decommitting will run on a separate GC helper
  // thread.
  MainThreadData<bool> useBackgroundThreads;

#ifdef DEBUG
  /* Shutdown has started. Further collections must be shutdown collections. */
  MainThreadData<bool> hadShutdownGC;
#endif

  /* Singly linked list of zones to be swept in the background. */
  HelperThreadLockData<ZoneList> backgroundSweepZones;

  /*
   * Whether to trigger a GC slice after a background task is complete, so that
   * the collector can continue or finsish collecting. This is only used for the
   * tasks that run concurrently with the mutator, which are background
   * finalization and background decommit.
   */
  HelperThreadLockData<bool> requestSliceAfterBackgroundTask;

  /*
   * Free LIFO blocks are transferred to these allocators before being freed on
   * a background thread.
   */
  HelperThreadLockData<LifoAlloc> lifoBlocksToFree;
  MainThreadData<LifoAlloc> lifoBlocksToFreeAfterMinorGC;
  HelperThreadLockData<Nursery::BufferSet> buffersToFreeAfterMinorGC;

  /* Index of current sweep group (for stats). */
  MainThreadData<unsigned> sweepGroupIndex;

  /*
   * Incremental sweep state.
   */
  MainThreadData<JS::Zone*> sweepGroups;
  MainThreadOrGCTaskData<JS::Zone*> currentSweepGroup;
  MainThreadData<UniquePtr<SweepAction>> sweepActions;
  MainThreadOrGCTaskData<JS::Zone*> sweepZone;
  MainThreadOrGCTaskData<AllocKind> sweepAllocKind;
  MainThreadData<mozilla::Maybe<AtomsTable::SweepIterator>> maybeAtomsToSweep;
  MainThreadOrGCTaskData<mozilla::Maybe<WeakCacheSweepIterator>>
      weakCachesToSweep;
  MainThreadData<bool> abortSweepAfterCurrentGroup;
  MainThreadOrGCTaskData<IncrementalProgress> sweepMarkResult;

#ifdef DEBUG
  /*
   * List of objects to mark at the beginning of a GC for testing purposes. May
   * also contain string directives to change mark color or wait until different
   * phases of the GC.
   *
   * This is a WeakCache because not everything in this list is guaranteed to
   * end up marked (eg if you insert an object from an already-processed sweep
   * group in the middle of an incremental GC). Also, the mark queue is not
   * used during shutdown GCs. In either case, unmarked objects may need to be
   * discarded.
   */
  JS::WeakCache<GCVector<HeapPtr<JS::Value>, 0, SystemAllocPolicy>>
      testMarkQueue;

  /* Position within the test mark queue. */
  size_t queuePos;

  /* The test marking queue might want to be marking a particular color. */
  mozilla::Maybe<js::gc::MarkColor> queueMarkColor;

  // During gray marking, delay AssertCellIsNotGray checks by
  // recording the cell pointers here and checking after marking has
  // finished.
  MainThreadData<Vector<const Cell*, 0, SystemAllocPolicy>>
      cellsToAssertNotGray;
  friend void js::gc::detail::AssertCellIsNotGray(const Cell*);
#endif

  friend class SweepGroupsIter;

  /*
   * Incremental compacting state.
   */
  MainThreadData<bool> startedCompacting;
  MainThreadData<ZoneList> zonesToMaybeCompact;
  MainThreadData<size_t> zonesCompacted;
#ifdef DEBUG
  GCLockData<Arena*> relocatedArenasToRelease;
#endif

#ifdef JS_GC_ZEAL
  MainThreadData<MarkingValidator*> markingValidator;
#endif

  /*
   * Default budget for incremental GC slice. See js/SliceBudget.h.
   *
   * JSGC_SLICE_TIME_BUDGET_MS
   * pref: javascript.options.mem.gc_incremental_slice_ms,
   */
  MainThreadData<int64_t> defaultTimeBudgetMS_;

  /*
   * We disable incremental GC if we encounter a Class with a trace hook
   * that does not implement write barriers.
   */
  MainThreadData<bool> incrementalAllowed;

  /*
   * Whether compacting GC can is enabled globally.
   *
   * JSGC_COMPACTING_ENABLED
   * pref: javascript.options.mem.gc_compacting
   */
  MainThreadData<bool> compactingEnabled;

  /*
   * Whether parallel marking is enabled globally.
   *
   * JSGC_PARALLEL_MARKING_ENABLED
   * pref: javascript.options.mem.gc_parallel_marking
   */
  MainThreadData<bool> parallelMarkingEnabled;

  MainThreadData<bool> rootsRemoved;

  /*
   * These options control the zealousness of the GC. At every allocation,
   * nextScheduled is decremented. When it reaches zero we do a full GC.
   *
   * At this point, if zeal_ is one of the types that trigger periodic
   * collection, then nextScheduled is reset to the value of zealFrequency.
   * Otherwise, no additional GCs take place.
   *
   * You can control these values in several ways:
   *   - Set the JS_GC_ZEAL environment variable
   *   - Call gczeal() or schedulegc() from inside shell-executed JS code
   *     (see the help for details)
   *
   * If gcZeal_ == 1 then we perform GCs in select places (during MaybeGC and
   * whenever we are notified that GC roots have been removed). This option is
   * mainly useful to embedders.
   *
   * We use zeal_ == 4 to enable write barrier verification. See the comment
   * in gc/Verifier.cpp for more information about this.
   *
   * zeal_ values from 8 to 10 periodically run different types of
   * incremental GC.
   *
   * zeal_ value 14 performs periodic shrinking collections.
   */
#ifdef JS_GC_ZEAL
  static_assert(size_t(ZealMode::Count) <= 32,
                "Too many zeal modes to store in a uint32_t");
  MainThreadData<uint32_t> zealModeBits;
  MainThreadData<int> zealFrequency;
  MainThreadData<int> nextScheduled;
  MainThreadData<bool> deterministicOnly;
  MainThreadData<int> zealSliceBudget;
  MainThreadData<size_t> maybeMarkStackLimit;

  MainThreadData<PersistentRooted<GCVector<JSObject*, 0, SystemAllocPolicy>>>
      selectedForMarking;
#endif

  MainThreadData<bool> fullCompartmentChecks;

  MainThreadData<uint32_t> gcCallbackDepth;

  MainThreadData<Callback<JSGCCallback>> gcCallback;
  MainThreadData<Callback<JS::DoCycleCollectionCallback>>
      gcDoCycleCollectionCallback;
  MainThreadData<Callback<JSObjectsTenuredCallback>> tenuredCallback;
  MainThreadData<CallbackVector<JSFinalizeCallback>> finalizeCallbacks;
  MainThreadOrGCTaskData<Callback<JSHostCleanupFinalizationRegistryCallback>>
      hostCleanupFinalizationRegistryCallback;
  MainThreadData<CallbackVector<JSWeakPointerZonesCallback>>
      updateWeakPointerZonesCallbacks;
  MainThreadData<CallbackVector<JSWeakPointerCompartmentCallback>>
      updateWeakPointerCompartmentCallbacks;

  /*
   * The trace operations to trace embedding-specific GC roots. One is for
   * tracing through black roots and the other is for tracing through gray
   * roots. The black/gray distinction is only relevant to the cycle
   * collector.
   */
  MainThreadData<CallbackVector<JSTraceDataOp>> blackRootTracers;
  MainThreadOrGCTaskData<Callback<JSGrayRootsTracer>> grayRootTracer;

  /* Always preserve JIT code during GCs, for testing. */
  MainThreadData<bool> alwaysPreserveCode;

  /* Count of the number of zones that are currently in page load. */
  MainThreadData<size_t> inPageLoadCount;

  MainThreadData<bool> lowMemoryState;

  /*
   * General purpose GC lock, used for synchronising operations on
   * arenas and during parallel marking.
   */
  friend class js::AutoLockGC;
  friend class js::AutoLockGCBgAlloc;
  js::Mutex lock MOZ_UNANNOTATED;

  /* Lock used to synchronise access to delayed marking state. */
  js::Mutex delayedMarkingLock MOZ_UNANNOTATED;

  friend class BackgroundSweepTask;
  friend class BackgroundFreeTask;

  BackgroundAllocTask allocTask;
  BackgroundUnmarkTask unmarkTask;
  BackgroundMarkTask markTask;
  BackgroundSweepTask sweepTask;
  BackgroundFreeTask freeTask;
  BackgroundDecommitTask decommitTask;

  /*
   * During incremental sweeping, this field temporarily holds the arenas of
   * the current AllocKind being swept in order of increasing free space.
   */
  MainThreadData<SortedArenaList> incrementalSweepList;

  MainThreadData<Nursery> nursery_;

  // The store buffer used to track tenured to nursery edges for generational
  // GC. This is accessed off main thread when sweeping WeakCaches.
  MainThreadOrGCTaskData<gc::StoreBuffer> storeBuffer_;

  mozilla::TimeStamp lastLastDitchTime;

  // The last time per-zone allocation rates were updated.
  MainThreadData<mozilla::TimeStamp> lastAllocRateUpdateTime;

  // Total collector time since per-zone allocation rates were last updated.
  MainThreadData<mozilla::TimeDuration> collectorTimeSinceAllocRateUpdate;

  friend class MarkingValidator;
  friend class AutoEnterIteration;
};

/* Prevent compartments and zones from being collected during iteration. */
class MOZ_RAII AutoEnterIteration {
  GCRuntime* gc;

 public:
  explicit AutoEnterIteration(GCRuntime* gc_) : gc(gc_) {
    ++gc->numActiveZoneIters;
  }

  ~AutoEnterIteration() {
    MOZ_ASSERT(gc->numActiveZoneIters);
    --gc->numActiveZoneIters;
  }
};

#ifdef JS_GC_ZEAL

inline bool GCRuntime::hasZealMode(ZealMode mode) {
  static_assert(size_t(ZealMode::Limit) < sizeof(zealModeBits) * 8,
                "Zeal modes must fit in zealModeBits");
  return zealModeBits & (1 << uint32_t(mode));
}

inline void GCRuntime::clearZealMode(ZealMode mode) {
  zealModeBits &= ~(1 << uint32_t(mode));
  MOZ_ASSERT(!hasZealMode(mode));
}

inline bool GCRuntime::needZealousGC() {
  if (nextScheduled > 0 && --nextScheduled == 0) {
    if (hasZealMode(ZealMode::Alloc) || hasZealMode(ZealMode::GenerationalGC) ||
        hasZealMode(ZealMode::IncrementalMultipleSlices) ||
        hasZealMode(ZealMode::Compact) || hasIncrementalTwoSliceZealMode()) {
      nextScheduled = zealFrequency;
    }
    return true;
  }
  return false;
}

inline bool GCRuntime::hasIncrementalTwoSliceZealMode() {
  return hasZealMode(ZealMode::YieldBeforeRootMarking) ||
         hasZealMode(ZealMode::YieldBeforeMarking) ||
         hasZealMode(ZealMode::YieldBeforeSweeping) ||
         hasZealMode(ZealMode::YieldBeforeSweepingAtoms) ||
         hasZealMode(ZealMode::YieldBeforeSweepingCaches) ||
         hasZealMode(ZealMode::YieldBeforeSweepingObjects) ||
         hasZealMode(ZealMode::YieldBeforeSweepingNonObjects) ||
         hasZealMode(ZealMode::YieldBeforeSweepingPropMapTrees) ||
         hasZealMode(ZealMode::YieldWhileGrayMarking);
}

#else
inline bool GCRuntime::hasZealMode(ZealMode mode) { return false; }
inline void GCRuntime::clearZealMode(ZealMode mode) {}
inline bool GCRuntime::needZealousGC() { return false; }
inline bool GCRuntime::hasIncrementalTwoSliceZealMode() { return false; }
#endif

bool IsCurrentlyAnimating(const mozilla::TimeStamp& lastAnimationTime,
                          const mozilla::TimeStamp& currentTime);

} /* namespace gc */
} /* namespace js */

#endif
