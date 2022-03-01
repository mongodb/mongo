/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef gc_GCRuntime_h
#define gc_GCRuntime_h

#include "mozilla/Atomics.h"
#include "mozilla/EnumSet.h"
#include "mozilla/Maybe.h"
#include "mozilla/TimeStamp.h"

#include "gc/ArenaList.h"
#include "gc/AtomMarking.h"
#include "gc/GCMarker.h"
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

class AutoAccessAtomsZone;
class AutoLockGC;
class AutoLockGCBgAlloc;
class AutoLockHelperThreadState;
class FinalizationRegistryObject;
class FinalizationQueueObject;
class VerifyPreTracer;
class WeakRefObject;
class ZoneAllocator;

namespace gc {

using BlackGrayEdgeVector = Vector<TenuredCell*, 0, SystemAllocPolicy>;
using ZoneVector = Vector<JS::Zone*, 4, SystemAllocPolicy>;

class AutoCallGCCallbacks;
class AutoGCSession;
class AutoHeapSession;
class AutoTraceSession;
class MarkingValidator;
struct MovingTracer;
enum class ShouldCheckThresholds;
class SweepGroupsIter;

// Interface to a sweep action.
struct SweepAction {
  // The arguments passed to each action.
  struct Args {
    GCRuntime* gc;
    JSFreeOp* fop;
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
  explicit BackgroundMarkTask(GCRuntime* gc)
      : GCParallelTask(gc), budget(SliceBudget::unlimited()) {}
  void setBudget(const SliceBudget& budget) { this->budget = budget; }
  void run(AutoLockHelperThreadState& lock) override;

 private:
  SliceBudget budget;
};

class BackgroundUnmarkTask : public GCParallelTask {
 public:
  explicit BackgroundUnmarkTask(GCRuntime* gc) : GCParallelTask(gc) {}
  void initZones();
  void run(AutoLockHelperThreadState& lock) override;

 private:
  void unmarkZones(AutoLockGC& lock);

  ZoneVector zones;
};

class BackgroundSweepTask : public GCParallelTask {
 public:
  explicit BackgroundSweepTask(GCRuntime* gc) : GCParallelTask(gc) {}
  void run(AutoLockHelperThreadState& lock) override;
};

class BackgroundFreeTask : public GCParallelTask {
 public:
  explicit BackgroundFreeTask(GCRuntime* gc) : GCParallelTask(gc) {}
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
  explicit BackgroundDecommitTask(GCRuntime* gc) : GCParallelTask(gc) {}

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

  void append(Zone* zone);
  void transferFrom(ZoneList& other);
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

class BarrierTracer final : public GenericTracer {
 public:
  static BarrierTracer* fromTracer(JSTracer* trc);

  explicit BarrierTracer(JSRuntime* rt);

  JSObject* onObjectEdge(JSObject* obj) override;
  Shape* onShapeEdge(Shape* shape) override;
  JSString* onStringEdge(JSString* string) override;
  js::BaseScript* onScriptEdge(js::BaseScript* script) override;
  BaseShape* onBaseShapeEdge(BaseShape* base) override;
  GetterSetter* onGetterSetterEdge(GetterSetter* gs) override;
  PropMap* onPropMapEdge(PropMap* map) override;
  Scope* onScopeEdge(Scope* scope) override;
  RegExpShared* onRegExpSharedEdge(RegExpShared* shared) override;
  BigInt* onBigIntEdge(BigInt* bi) override;
  JS::Symbol* onSymbolEdge(JS::Symbol* sym) override;
  jit::JitCode* onJitCodeEdge(jit::JitCode* jit) override;

  void performBarrier(JS::GCCellPtr cell);

 private:
  void handleBufferFull(JS::GCCellPtr cell);

  GCMarker& marker;
};

class GCRuntime {
  friend GCMarker::MarkQueueProgress GCMarker::processMarkQueue();

 public:
  explicit GCRuntime(JSRuntime* rt);
  [[nodiscard]] bool init(uint32_t maxbytes);
  void finishRoots();
  void finish();

  JS::HeapState heapState() const { return heapState_; }

  void freezeSelfHostingZone();
  bool isSelfHostingZoneFrozen() const { return selfHostingZoneFrozen; }

  inline bool hasZealMode(ZealMode mode);
  inline void clearZealMode(ZealMode mode);
  inline bool upcomingZealousGC();
  inline bool needZealousGC();
  inline bool hasIncrementalTwoSliceZealMode();

  [[nodiscard]] bool addRoot(Value* vp, const char* name);
  void removeRoot(Value* vp);
  void setMarkStackLimit(size_t limit, AutoLockGC& lock);

  [[nodiscard]] bool setParameter(JSGCParamKey key, uint32_t value);
  [[nodiscard]] bool setParameter(JSGCParamKey key, uint32_t value,
                                  AutoLockGC& lock);
  void resetParameter(JSGCParamKey key);
  void resetParameter(JSGCParamKey key, AutoLockGC& lock);
  uint32_t getParameter(JSGCParamKey key);
  uint32_t getParameter(JSGCParamKey key, const AutoLockGC& lock);

  void setPerformanceHint(PerformanceHint hint);

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
  bool checkEagerAllocTrigger(const HeapSize& size,
                              const HeapThreshold& threshold);
  // The return value indicates whether a major GC was performed.
  bool gcIfRequested();
  void gc(JS::GCOptions options, JS::GCReason reason);
  void startGC(JS::GCOptions options, JS::GCReason reason, int64_t millis = 0);
  void gcSlice(JS::GCReason reason, int64_t millis = 0);
  void finishGC(JS::GCReason reason);
  void abortGC();
  void startDebugGC(JS::GCOptions options, SliceBudget& budget);
  void debugGCSlice(SliceBudget& budget);

  void triggerFullGCForAtoms(JSContext* cx);

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
  const void* addressOfNurseryCurrentEnd() {
    return nursery_.refNoCheck().addressOfCurrentEnd();
  }
  const void* addressOfStringNurseryCurrentEnd() {
    return nursery_.refNoCheck().addressOfCurrentStringEnd();
  }
  const void* addressOfBigIntNurseryCurrentEnd() {
    return nursery_.refNoCheck().addressOfCurrentBigIntEnd();
  }
  uint32_t* addressOfNurseryAllocCount() {
    return stats().addressOfAllocsSinceMinorGCNursery();
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
  State state() const { return incrementalState; }
  bool isHeapCompacting() const { return state() == State::Compact; }
  bool isForegroundSweeping() const { return state() == State::Sweep; }
  bool isBackgroundSweeping() const { return sweepTask.wasStarted(); }
  bool isBackgroundMarking() const { return markTask.wasStarted(); }
  void waitBackgroundSweepEnd();
  void waitBackgroundAllocEnd() { allocTask.cancelAndWait(); }
  void waitBackgroundFreeEnd();
  void waitForBackgroundTasks();

  void lockGC() { lock.lock(); }

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
  bool isIncrementalGCInProgress() const {
    return state() != State::NotActive && !isVerifyPreBarriersEnabled();
  }

  bool isPerZoneGCEnabled() const { return perZoneGCEnabled; }

  bool hasForegroundWork() const;

  bool isCompactingGCEnabled() const;

  bool isShrinkingGC() const { return gcOptions == JS::GCOptions::Shrink; }

  bool initSweepActions();

  void setGrayRootsTracer(JSTraceDataOp traceOp, void* data);
  [[nodiscard]] bool addBlackRootsTracer(JSTraceDataOp traceOp, void* data);
  void removeBlackRootsTracer(JSTraceDataOp traceOp, void* data);
  void clearBlackAndGrayRootTracers();

  void updateMemoryCountersOnGCStart();

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
                               FinalizationRegistryObject* registry);
  bool registerWithFinalizationRegistry(JSContext* cx, HandleObject target,
                                        HandleObject record);

  void setFullCompartmentChecks(bool enable);

  JS::Zone* getCurrentSweepGroup() { return currentSweepGroup; }
  unsigned getCurrentSweepGroupIndex() {
    return state() == State::Sweep ? sweepGroupIndex : 0;
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

  bool fullGCForAtomsRequested() const { return fullGCForAtomsRequested_; }

  double computeHeapGrowthFactor(size_t lastBytes);
  size_t computeTriggerBytes(double growthFactor, size_t lastBytes);

  inline void updateOnFreeArenaAlloc(const TenuredChunkInfo& info);
  inline void updateOnArenaFree();

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
  void queueAllLifoBlocksForFree(LifoAlloc* lifo);
  void queueAllLifoBlocksForFreeAfterMinorGC(LifoAlloc* lifo);
  void queueBuffersForFreeAfterMinorGC(Nursery::BufferSet& buffers);

  // Public here for ReleaseArenaLists and FinalizeTypedArenas.
  void releaseArena(Arena* arena, const AutoLockGC& lock);

  // Allocator
  template <AllowGC allowGC>
  [[nodiscard]] bool checkAllocatorState(JSContext* cx, AllocKind kind);
  template <AllowGC allowGC>
  JSObject* tryNewNurseryObject(JSContext* cx, size_t thingSize,
                                size_t nDynamicSlots, const JSClass* clasp,
                                AllocSite* site);
  template <AllowGC allowGC>
  static JSObject* tryNewTenuredObject(JSContext* cx, AllocKind kind,
                                       size_t thingSize, size_t nDynamicSlots);
  template <typename T, AllowGC allowGC>
  static T* tryNewTenuredThing(JSContext* cx, AllocKind kind, size_t thingSize);
  template <AllowGC allowGC>
  JSString* tryNewNurseryString(JSContext* cx, size_t thingSize,
                                AllocKind kind);
  template <AllowGC allowGC>
  JS::BigInt* tryNewNurseryBigInt(JSContext* cx, size_t thingSize,
                                  AllocKind kind);
  static TenuredCell* refillFreeListInGC(Zone* zone, AllocKind thingKind);

  void setParallelAtomsAllocEnabled(bool enabled);
  void setParallelUnmarkEnabled(bool enabled);

  /*
   * Concurrent sweep infrastructure.
   */
  void startTask(GCParallelTask& task, gcstats::PhaseKind phase,
                 AutoLockHelperThreadState& locked);
  void joinTask(GCParallelTask& task, gcstats::PhaseKind phase,
                AutoLockHelperThreadState& locked);
  void joinTask(GCParallelTask& task, gcstats::PhaseKind phase);
  void updateHelperThreadCount();
  size_t parallelWorkerCount() const;

  void mergeRealms(JS::Realm* source, JS::Realm* target);

  // WeakRefs
  bool registerWeakRef(HandleObject target, HandleObject weakRef);
  bool unregisterWeakRefWrapper(JSObject* wrapper);
  void traceKeptObjects(JSTracer* trc);

 private:
  enum IncrementalResult { ResetIncremental = 0, Ok };

  TriggerResult checkHeapThreshold(Zone* zone, const HeapSize& heapSize,
                                   const HeapThreshold& heapThreshold);

  void updateGCThresholdsAfterCollection(const AutoLockGC& lock);
  void updateAllGCStartThresholds(const AutoLockGC& lock);

  // Delete an empty zone after its contents have been merged.
  void deleteEmptyZone(Zone* zone);

  // For ArenaLists::allocateFromArena()
  friend class ArenaLists;
  TenuredChunk* pickChunk(AutoLockGCBgAlloc& lock);
  Arena* allocateArena(TenuredChunk* chunk, Zone* zone, AllocKind kind,
                       ShouldCheckThresholds checkThresholds,
                       const AutoLockGC& lock);

  // Allocator internals
  [[nodiscard]] bool gcIfNeededAtAllocation(JSContext* cx);
  template <typename T>
  static void checkIncrementalZoneState(JSContext* cx, T* t);
  static TenuredCell* refillFreeListFromAnyThread(JSContext* cx,
                                                  AllocKind thingKind);
  static TenuredCell* refillFreeListFromMainThread(JSContext* cx,
                                                   AllocKind thingKind);
  static TenuredCell* refillFreeListFromHelperThread(JSContext* cx,
                                                     AllocKind thingKind);
  void attemptLastDitchGC(JSContext* cx);

  /*
   * Return the list of chunks that can be released outside the GC lock.
   * Must be called either during the GC or with the GC lock taken.
   */
  friend class BackgroundDecommitTask;
  bool tooManyEmptyChunks(const AutoLockGC& lock);
  ChunkPool expireEmptyChunkPool(const AutoLockGC& lock);
  void freeEmptyChunks(const AutoLockGC& lock);
  void prepareToFreeChunk(TenuredChunkInfo& info);

  friend class BackgroundAllocTask;
  bool wantBackgroundAllocation(const AutoLockGC& lock) const;
  void startBackgroundAllocTaskIfIdle();

  void requestMajorGC(JS::GCReason reason);
  SliceBudget defaultBudget(JS::GCReason reason, int64_t millis);
  void maybeIncreaseSliceBudget(SliceBudget& budget);
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

  using MaybeGCOptions = mozilla::Maybe<JS::GCOptions>;

  void collect(bool nonincrementalByAPI, const SliceBudget& budget,
               const MaybeGCOptions& options,
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
                                          const MaybeGCOptions& options,
                                          JS::GCReason reason);
  bool shouldRepeatForDeadZone(JS::GCReason reason);

  void incrementalSlice(SliceBudget& budget, const MaybeGCOptions& options,
                        JS::GCReason reason);

  void waitForBackgroundTasksBeforeSlice();
  bool mightSweepInThisSlice(bool nonIncremental);
  void collectNurseryFromMajorGC(const MaybeGCOptions& options,
                                 JS::GCReason reason);
  void collectNursery(JS::GCOptions options, JS::GCReason reason,
                      gcstats::PhaseKind phase);

  friend class AutoCallGCCallbacks;
  void maybeCallGCCallback(JSGCStatus status, JS::GCReason reason);

  void purgeRuntime();
  [[nodiscard]] bool beginPreparePhase(JS::GCReason reason,
                                       AutoGCSession& session);
  bool prepareZonesForCollection(JS::GCReason reason, bool* isFullOut);
  void bufferGrayRoots();
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
  void traceRuntimeAtoms(JSTracer* trc, const AutoAccessAtomsZone& atomsAccess);
  void traceRuntimeCommon(JSTracer* trc, TraceOrMarkRuntime traceOrMark);
  void traceEmbeddingBlackRoots(JSTracer* trc);
  void traceEmbeddingGrayRoots(JSTracer* trc);
  void markFinalizationRegistryRoots(JSTracer* trc);
  void checkNoRuntimeRoots(AutoGCSession& session);
  void maybeDoCycleCollection();
  void findDeadCompartments();

  friend class BackgroundMarkTask;
  IncrementalProgress markUntilBudgetExhausted(
      SliceBudget& sliceBudget,
      GCMarker::ShouldReportMarkTime reportTime = GCMarker::ReportMarkTime);
  void drainMarkStack();
  template <class ZoneIterT>
  IncrementalProgress markWeakReferences(SliceBudget& budget);
  IncrementalProgress markWeakReferencesInCurrentGroup(SliceBudget& budget);
  template <class ZoneIterT>
  void markGrayRoots(gcstats::PhaseKind phase);
  void markBufferedGrayRoots(JS::Zone* zone);
  IncrementalProgress markAllWeakReferences();
  void markAllGrayReferences(gcstats::PhaseKind phase);

  void beginSweepPhase(JS::GCReason reason, AutoGCSession& session);
  void dropStringWrappers();
  void groupZonesForSweeping(JS::GCReason reason);
  [[nodiscard]] bool findSweepGroupEdges();
  void getNextSweepGroup();
  IncrementalProgress markGrayReferencesInCurrentGroup(JSFreeOp* fop,
                                                       SliceBudget& budget);
  IncrementalProgress endMarkingSweepGroup(JSFreeOp* fop, SliceBudget& budget);
  void markIncomingCrossCompartmentPointers(MarkColor color);
  IncrementalProgress beginSweepingSweepGroup(JSFreeOp* fop,
                                              SliceBudget& budget);
  IncrementalProgress markDuringSweeping(JSFreeOp* fop, SliceBudget& budget);
  void updateAtomsBitmap();
  void sweepCCWrappers();
  void sweepMisc();
  void sweepCompressionTasks();
  void sweepWeakMaps();
  void sweepUniqueIds();
  void sweepDebuggerOnMainThread(JSFreeOp* fop);
  void sweepJitDataOnMainThread(JSFreeOp* fop);
  void sweepFinalizationRegistriesOnMainThread();
  void sweepFinalizationRegistries(Zone* zone);
  void queueFinalizationRegistryForCleanup(FinalizationQueueObject* queue);
  void sweepWeakRefs();
  IncrementalProgress endSweepingSweepGroup(JSFreeOp* fop, SliceBudget& budget);
  IncrementalProgress performSweepActions(SliceBudget& sliceBudget);
  void startSweepingAtomsTable();
  IncrementalProgress sweepAtomsTable(JSFreeOp* fop, SliceBudget& budget);
  IncrementalProgress sweepWeakCaches(JSFreeOp* fop, SliceBudget& budget);
  IncrementalProgress finalizeAllocKind(JSFreeOp* fop, SliceBudget& budget);
  IncrementalProgress sweepPropMapTree(JSFreeOp* fop, SliceBudget& budget);
  void endSweepPhase(bool lastGC);
  bool allCCVisibleZonesWereCollected();
  void sweepZones(JSFreeOp* fop, bool destroyingRuntime);
  void startDecommit();
  void decommitFreeArenas(const bool& canel, AutoLockGC& lock);
  void decommitFreeArenasWithoutUnlocking(const AutoLockGC& lock);
  void queueZonesAndStartBackgroundSweep(ZoneList& zones);
  void sweepFromBackgroundThread(AutoLockHelperThreadState& lock);
  void startBackgroundFree();
  void freeFromBackgroundThread(AutoLockHelperThreadState& lock);
  void sweepBackgroundThings(ZoneList& zones);
  void assertBackgroundSweepingFinished();
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
  void updateRttValueObjects(MovingTracer* trc, Zone* zone);
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
      GCParallelTask& task, const SliceBudget& budget,
      ShouldTriggerSliceWhenFinished triggerSlice);

  void maybeRequestGCAfterBackgroundTask(const AutoLockHelperThreadState& lock);
  void cancelRequestedGCAfterBackgroundTask();
  void finishCollection();
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

  void callFinalizeCallbacks(JSFreeOp* fop, JSFinalizeStatus status) const;
  void callWeakPointerZonesCallbacks() const;
  void callWeakPointerCompartmentCallbacks(JS::Compartment* comp) const;
  void callDoCycleCollectionCallback(JSContext* cx);

 public:
  JSRuntime* const rt;

  /* Embedders can use this zone and group however they wish. */
  UnprotectedData<JS::Zone*> systemZone;

  // All zones in the runtime, except the atoms zone.
 private:
  MainThreadOrGCTaskData<ZoneVector> zones_;

 public:
  ZoneVector& zones() { return zones_.ref(); }

  // The unique atoms zone.
  WriteOnceData<Zone*> atomsZone;

 private:
  // Any activity affecting the heap.
  mozilla::Atomic<JS::HeapState, mozilla::SequentiallyConsistent> heapState_;
  friend class AutoHeapSession;
  friend class JS::AutoEnterCycleCollection;

  UnprotectedData<gcstats::Statistics> stats_;

 public:
  gcstats::Statistics& stats() { return stats_.ref(); }

  js::StringStats stringStats;

  GCMarker marker;
  BarrierTracer barrierTracer;

  Vector<JS::GCCellPtr, 0, SystemAllocPolicy> unmarkGrayStack;

  /* Track total GC heap size for this runtime. */
  HeapSize heapSize;

  /* GC scheduling state and parameters. */
  GCSchedulingTunables tunables;
  GCSchedulingState schedulingState;

  // Helper thread configuration.
  MainThreadData<double> helperThreadRatio;
  MainThreadData<size_t> maxHelperThreads;
  MainThreadData<size_t> helperThreadCount;

  // State used for managing atom mark bitmaps in each zone.
  AtomMarkingRuntime atomMarking;

 private:
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

  MainThreadData<RootedValueMap> rootsHash;

  // An incrementing id used to assign unique ids to cells that require one.
  mozilla::Atomic<uint64_t, mozilla::ReleaseAcquire> nextCellUniqueId_;

  /*
   * Number of the committed arenas in all GC chunks including empty chunks.
   */
  mozilla::Atomic<uint32_t, mozilla::ReleaseAcquire> numArenasFreeCommitted;
  MainThreadData<VerifyPreTracer*> verifyPreData;

 private:
  MainThreadData<mozilla::TimeStamp> lastGCStartTime_;
  MainThreadData<mozilla::TimeStamp> lastGCEndTime_;

  MainThreadData<bool> incrementalGCEnabled;
  MainThreadData<bool> perZoneGCEnabled;

  mozilla::Atomic<size_t, mozilla::ReleaseAcquire> numActiveZoneIters;

  /*
   * The self hosting zone is collected once after initialization. We don't
   * allow allocation after this point and we don't collect it again.
   */
  WriteOnceData<bool> selfHostingZoneFrozen;

  /* During shutdown, the GC needs to clean up every possible object. */
  MainThreadData<bool> cleanUpEverything;

  // Gray marking must be done after all black marking is complete. However,
  // we do not have write barriers on XPConnect roots. Therefore, XPConnect
  // roots must be accumulated in the first slice of incremental GC. We
  // accumulate these roots in each zone's gcGrayRoots vector and then mark
  // them later, after black marking is complete for each compartment. This
  // accumulation can fail, but in that case we switch to non-incremental GC.
  enum class GrayBufferState { Unused, Okay, Failed };
  MainThreadOrGCTaskData<GrayBufferState> grayBufferState;
  bool hasValidGrayRootsBuffer() const {
    return grayBufferState == GrayBufferState::Okay;
  }

  // Clear each zone's gray buffers, but do not change the current state.
  void resetBufferedGrayRoots();

  // Reset the gray buffering state to Unused.
  void clearBufferedGrayRoots() {
    grayBufferState = GrayBufferState::Unused;
    resetBufferedGrayRoots();
  }

  /*
   * The gray bits can become invalid if UnmarkGray overflows the stack. A
   * full GC will reset this bit, since it fills in all the gray bits.
   */
  UnprotectedData<bool> grayBitsValid;

  mozilla::Atomic<JS::GCReason, mozilla::ReleaseAcquire> majorGCTriggerReason;

 private:
  /* Perform full GC when we are able to collect the atoms zone. */
  MainThreadData<bool> fullGCForAtomsRequested_;

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

  /* The invocation kind of the current GC, taken from the first slice. */
  MainThreadOrGCTaskData<JS::GCOptions> gcOptions;

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

  /* Whether any sweeping will take place in the separate GC helper thread. */
  MainThreadData<bool> sweepOnBackgroundThread;

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
  MainThreadData<bool> hasMarkedGrayRoots;
  MainThreadData<bool> abortSweepAfterCurrentGroup;
  MainThreadOrGCTaskData<IncrementalProgress> sweepMarkResult;

#ifdef DEBUG
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
  MainThreadOrGCTaskData<Callback<JSTraceDataOp>> grayRootTracer;

  /* Always preserve JIT code during GCs, for testing. */
  MainThreadData<bool> alwaysPreserveCode;

  /* Count of the number of zones that are currently in page load. */
  MainThreadData<size_t> inPageLoadCount;

  MainThreadData<bool> lowMemoryState;

  /* Synchronize GC heap access among GC helper threads and the main thread. */
  friend class js::AutoLockGC;
  friend class js::AutoLockGCBgAlloc;
  js::Mutex lock;

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

 private:
  MainThreadData<Nursery> nursery_;

  // The store buffer used to track tenured to nursery edges for generational
  // GC. This is accessed off main thread when sweeping WeakCaches.
  MainThreadOrGCTaskData<gc::StoreBuffer> storeBuffer_;

  mozilla::TimeStamp lastLastDitchTime;

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

inline bool GCRuntime::upcomingZealousGC() { return nextScheduled == 1; }

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
inline bool GCRuntime::upcomingZealousGC() { return false; }
inline bool GCRuntime::needZealousGC() { return false; }
inline bool GCRuntime::hasIncrementalTwoSliceZealMode() { return false; }
#endif

bool IsCurrentlyAnimating(const mozilla::TimeStamp& lastAnimationTime,
                          const mozilla::TimeStamp& currentTime);

} /* namespace gc */
} /* namespace js */

#endif
