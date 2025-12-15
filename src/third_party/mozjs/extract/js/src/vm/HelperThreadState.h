/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * Definitions for managing off-thread work using a process wide list of
 * worklist items and pool of threads. Worklist items are engine internal, and
 * are distinct from e.g. web workers.
 */

#ifndef vm_HelperThreadState_h
#define vm_HelperThreadState_h

#include "mozilla/AlreadyAddRefed.h"  // already_AddRefed
#include "mozilla/Assertions.h"       // MOZ_ASSERT, MOZ_CRASH
#include "mozilla/Attributes.h"       // MOZ_RAII
#include "mozilla/EnumeratedArray.h"  // mozilla::EnumeratedArray
#include "mozilla/LinkedList.h"  // mozilla::LinkedList, mozilla::LinkedListElement
#include "mozilla/MemoryReporting.h"  // mozilla::MallocSizeOf
#include "mozilla/RefPtr.h"           // RefPtr
#include "mozilla/TimeStamp.h"        // mozilla::TimeDuration

#include <stddef.h>  // size_t
#include <stdint.h>  // uint32_t, uint64_t
#include <utility>   // std::move

#include "ds/Fifo.h"                      // Fifo
#include "frontend/CompilationStencil.h"  // frontend::InitialStencilAndDelazifications
#include "gc/GCRuntime.h"                 // gc::GCRuntime
#include "js/AllocPolicy.h"               // SystemAllocPolicy
#include "js/CompileOptions.h"            // JS::ReadOnlyCompileOptions
#include "js/experimental/JSStencil.h"  // JS::InstantiationStorage
#include "js/HelperThreadAPI.h"         // JS::HelperThreadTaskCallback
#include "js/MemoryMetrics.h"           // JS::GlobalStats
#include "js/ProfilingStack.h"  // JS::RegisterThreadCallback, JS::UnregisterThreadCallback
#include "js/RootingAPI.h"                // JS::Handle
#include "js/UniquePtr.h"                 // UniquePtr
#include "js/Utility.h"                   // ThreadType
#include "threading/ConditionVariable.h"  // ConditionVariable
#include "threading/ProtectedData.h"      // WriteOnceData
#include "vm/ConcurrentDelazification.h"  // DelazificationContext
#include "vm/HelperThreads.h"  // AutoLockHelperThreadState, AutoUnlockHelperThreadState
#include "vm/HelperThreadTask.h"             // HelperThreadTask
#include "vm/JSContext.h"                    // JSContext
#include "vm/JSScript.h"                     // ScriptSource
#include "vm/Runtime.h"                      // JSRuntime
#include "vm/SharedImmutableStringsCache.h"  // SharedImmutableString
#include "wasm/WasmConstants.h"              // wasm::CompileState

class JSTracer;

namespace js {

struct DelazifyTask;
struct FreeDelazifyTask;
struct PromiseHelperTask;
class PromiseObject;

namespace jit {
class BaselineCompileTask;
class IonCompileTask;
class IonFreeTask;
}  // namespace jit

namespace wasm {

struct CompileTask;
using CompileTaskPtrFifo = Fifo<CompileTask*, 0, SystemAllocPolicy>;

struct CompleteTier2GeneratorTask : public HelperThreadTask {
  virtual ~CompleteTier2GeneratorTask() = default;
  virtual void cancel() = 0;
  const char* getName() override { return "CompleteTier2GeneratorTask"; }
};

using UniqueCompleteTier2GeneratorTask = UniquePtr<CompleteTier2GeneratorTask>;
using CompleteTier2GeneratorTaskPtrVector =
    Vector<CompleteTier2GeneratorTask*, 0, SystemAllocPolicy>;

struct PartialTier2CompileTask : public HelperThreadTask {
  virtual ~PartialTier2CompileTask() = default;
  virtual void cancel() = 0;
  const char* getName() override { return "PartialTier2CompileTask"; }
};

using UniquePartialTier2CompileTask = UniquePtr<PartialTier2CompileTask>;
using PartialTier2CompileTaskPtrVector =
    Vector<PartialTier2CompileTask*, 0, SystemAllocPolicy>;

}  // namespace wasm

// Per-process state for off thread work items.
class GlobalHelperThreadState {
 public:
  // A single complete tier-2 ModuleGenerator job spawns many compilation jobs,
  // and we do not want to allow more than one such ModuleGenerator to run at a
  // time.
  static const size_t MaxCompleteTier2GeneratorTasks = 1;

  // The number of partial tier 2 compilation tasks that can run
  // simultaneously.  This constant specifies unfortunately both the default
  // and the maximum.
  static const size_t MaxPartialTier2CompileTasks = 1;

  // Number of CPUs to treat this machine as having when creating threads.
  // May be accessed without locking.
  size_t cpuCount;

  // Number of threads to create. May be accessed without locking.
  size_t threadCount;

  // Thread stack quota to use when running tasks.
  size_t stackQuota;

  bool terminating_ = false;

  using BaselineCompileTaskVector =
      Vector<jit::BaselineCompileTask*, 1, SystemAllocPolicy>;
  using IonCompileTaskVector =
      Vector<jit::IonCompileTask*, 0, SystemAllocPolicy>;
  using IonFreeTaskVector =
      Vector<js::UniquePtr<jit::IonFreeTask>, 0, SystemAllocPolicy>;
  using DelazifyTaskList = mozilla::LinkedList<DelazifyTask>;
  using FreeDelazifyTaskVector =
      Vector<js::UniquePtr<FreeDelazifyTask>, 1, SystemAllocPolicy>;
  using SourceCompressionTaskVector =
      Vector<UniquePtr<SourceCompressionTask>, 0, SystemAllocPolicy>;
  using PromiseHelperTaskVector =
      Vector<PromiseHelperTask*, 0, SystemAllocPolicy>;

  // Count of running task by each threadType.
  mozilla::EnumeratedArray<ThreadType, size_t,
                           size_t(ThreadType::THREAD_TYPE_MAX)>
      runningTaskCount;
  size_t totalCountRunningTasks;

  WriteOnceData<JS::RegisterThreadCallback> registerThread;
  WriteOnceData<JS::UnregisterThreadCallback> unregisterThread;

  // Count of helper threads 'reserved' for parallel marking. This is used to
  // prevent too many runtimes trying to mark in parallel at once. Does not stop
  // threads from being used for other kinds of task, including GC tasks.
  HelperThreadLockData<size_t> gcParallelMarkingThreads;

 private:
  // The lists below are all protected by |lock|.

  // Baseline compilation worklist and finished jobs.
  BaselineCompileTaskVector baselineWorklist_, baselineFinishedList_;

  // Ion compilation worklist and finished jobs.
  IonCompileTaskVector ionWorklist_, ionFinishedList_;
  IonFreeTaskVector ionFreeList_;

  // wasm worklists.
  wasm::CompileTaskPtrFifo wasmWorklist_tier1_;
  wasm::CompileTaskPtrFifo wasmWorklist_tier2_;
  wasm::CompleteTier2GeneratorTaskPtrVector wasmCompleteTier2GeneratorWorklist_;
  wasm::PartialTier2CompileTaskPtrVector wasmPartialTier2CompileWorklist_;

  // Count of finished CompleteTier2Generator tasks.
  uint32_t wasmCompleteTier2GeneratorsFinished_;

  // Async tasks that, upon completion, are dispatched back to the JSContext's
  // owner thread via embedding callbacks instead of a finished list.
  PromiseHelperTaskVector promiseHelperTasks_;

  // Script worklist, which might still have function to delazify.
  DelazifyTaskList delazifyWorklist_;
  // Ideally an instance should not have a method to free it-self as, the method
  // has a this pointer, which aliases the deleted instance, and that the method
  // might have some of its fields aliased on the stack.
  //
  // Delazification task are complex and have a lot of fields. To reduce the
  // risk of having aliased fields on the stack while deleting instances of a
  // DelazifyTask, we have FreeDelazifyTask. While FreeDelazifyTask suffer from
  // the same problem, the limited scope of their actions should mitigate the
  // risk.
  FreeDelazifyTaskVector freeDelazifyTaskVector_;

  // Source compression worklist of tasks that we do not yet know can start.
  SourceCompressionTaskVector compressionPendingList_;

  // Source compression worklist of tasks that can start.
  SourceCompressionTaskVector compressionWorklist_;

  // Finished source compression tasks.
  SourceCompressionTaskVector compressionFinishedList_;

  // GC tasks needing to be done in parallel. These are first queued in the
  // GCRuntime before being dispatched to the helper thread system.
  GCParallelTaskList gcParallelWorklist_;

  using HelperThreadTaskVector =
      Vector<HelperThreadTask*, 0, SystemAllocPolicy>;
  // Vector of running HelperThreadTask.
  // This is used to get the HelperThreadTask that are currently running.
  HelperThreadTaskVector helperTasks_;

  // Callback to dispatch a task to a thread pool. Set by
  // JS::SetHelperThreadTaskCallback. If this is not set the internal thread
  // pool is used.
  JS::HelperThreadTaskCallback dispatchTaskCallback = nullptr;
  friend class AutoHelperTaskQueue;

  // Condition variable for notifiying the main thread that a helper task has
  // completed some work.
  js::ConditionVariable consumerWakeup;

#ifdef DEBUG
  // The number of tasks dispatched to the thread pool that have not started
  // running yet.
  size_t tasksPending_ = 0;
#endif

  bool isInitialized_ = false;

  bool useInternalThreadPool_ = true;

 public:
  void addSizeOfIncludingThis(JS::GlobalStats* stats,
                              const AutoLockHelperThreadState& lock) const;

  size_t maxBaselineCompilationThreads() const;
  size_t maxIonCompilationThreads() const;
  size_t maxIonFreeThreads() const;
  size_t maxWasmCompilationThreads() const;
  size_t maxWasmCompleteTier2GeneratorThreads() const;
  size_t maxWasmPartialTier2CompileThreads() const;
  size_t maxPromiseHelperThreads() const;
  size_t maxDelazifyThreads() const;
  size_t maxCompressionThreads() const;
  size_t maxGCParallelThreads() const;

  GlobalHelperThreadState();

  bool isInitialized(const AutoLockHelperThreadState& lock) const {
    return isInitialized_;
  }

  [[nodiscard]] bool ensureInitialized();
  [[nodiscard]] bool ensureThreadCount(size_t count,
                                       AutoLockHelperThreadState& lock);
  void finish(AutoLockHelperThreadState& lock);
  void finishThreads(AutoLockHelperThreadState& lock);

  void setCpuCount(size_t count);

  void setDispatchTaskCallback(JS::HelperThreadTaskCallback callback,
                               size_t threadCount, size_t stackSize,
                               const AutoLockHelperThreadState& lock);

  void destroyHelperContexts(AutoLockHelperThreadState& lock);

#ifdef DEBUG
  void assertIsLockedByCurrentThread() const;
#endif

  void wait(AutoLockHelperThreadState& lock,
            mozilla::TimeDuration timeout = mozilla::TimeDuration::Forever());
  void notifyAll(const AutoLockHelperThreadState&);

  bool useInternalThreadPool(const AutoLockHelperThreadState& lock) const {
    return useInternalThreadPool_;
  }

  bool isTerminating(const AutoLockHelperThreadState& locked) const {
    return terminating_;
  }

 private:
  void notifyOne(const AutoLockHelperThreadState&);

 public:
  // Helper method for removing items from the vectors below while iterating
  // over them.
  template <typename T>
  static void remove(T& vector, size_t* index) {
    // Self-moving is undefined behavior.
    if (*index != vector.length() - 1) {
      vector[*index] = std::move(vector.back());
    }
    (*index)--;
    vector.popBack();
  }

  BaselineCompileTaskVector& baselineWorklist(
      const AutoLockHelperThreadState&) {
    return baselineWorklist_;
  }
  BaselineCompileTaskVector& baselineFinishedList(
      const AutoLockHelperThreadState&) {
    return baselineFinishedList_;
  }
  IonCompileTaskVector& ionWorklist(const AutoLockHelperThreadState&) {
    return ionWorklist_;
  }
  IonCompileTaskVector& ionFinishedList(const AutoLockHelperThreadState&) {
    return ionFinishedList_;
  }
  IonFreeTaskVector& ionFreeList(const AutoLockHelperThreadState&) {
    return ionFreeList_;
  }

  wasm::CompileTaskPtrFifo& wasmWorklist(const AutoLockHelperThreadState&,
                                         wasm::CompileState state) {
    switch (state) {
      case wasm::CompileState::Once:
      case wasm::CompileState::EagerTier1:
      case wasm::CompileState::LazyTier1:
        return wasmWorklist_tier1_;
      case wasm::CompileState::EagerTier2:
      case wasm::CompileState::LazyTier2:
        return wasmWorklist_tier2_;
      default:
        MOZ_CRASH();
    }
  }

  wasm::CompleteTier2GeneratorTaskPtrVector& wasmCompleteTier2GeneratorWorklist(
      const AutoLockHelperThreadState&) {
    return wasmCompleteTier2GeneratorWorklist_;
  }

  wasm::PartialTier2CompileTaskPtrVector& wasmPartialTier2CompileWorklist(
      const AutoLockHelperThreadState&) {
    return wasmPartialTier2CompileWorklist_;
  }

  void incWasmCompleteTier2GeneratorsFinished(
      const AutoLockHelperThreadState&) {
    wasmCompleteTier2GeneratorsFinished_++;
  }

  uint32_t wasmCompleteTier2GeneratorsFinished(
      const AutoLockHelperThreadState&) const {
    return wasmCompleteTier2GeneratorsFinished_;
  }

  PromiseHelperTaskVector& promiseHelperTasks(
      const AutoLockHelperThreadState&) {
    return promiseHelperTasks_;
  }

  DelazifyTaskList& delazifyWorklist(const AutoLockHelperThreadState&) {
    return delazifyWorklist_;
  }

  FreeDelazifyTaskVector& freeDelazifyTaskVector(
      const AutoLockHelperThreadState&) {
    return freeDelazifyTaskVector_;
  }

  SourceCompressionTaskVector& compressionPendingList(
      const AutoLockHelperThreadState&) {
    return compressionPendingList_;
  }

  SourceCompressionTaskVector& compressionWorklist(
      const AutoLockHelperThreadState&) {
    return compressionWorklist_;
  }

  SourceCompressionTaskVector& compressionFinishedList(
      const AutoLockHelperThreadState&) {
    return compressionFinishedList_;
  }

 private:
  GCParallelTaskList& gcParallelWorklist() { return gcParallelWorklist_; }

  HelperThreadTaskVector& helperTasks(const AutoLockHelperThreadState&) {
    return helperTasks_;
  }

  bool canStartWasmCompile(const AutoLockHelperThreadState& lock,
                           wasm::CompileState state);

  bool canStartWasmTier1CompileTask(const AutoLockHelperThreadState& lock);
  bool canStartWasmTier2CompileTask(const AutoLockHelperThreadState& lock);
  bool canStartWasmCompleteTier2GeneratorTask(
      const AutoLockHelperThreadState& lock);
  bool canStartWasmPartialTier2CompileTask(
      const AutoLockHelperThreadState& lock);
  bool canStartPromiseHelperTask(const AutoLockHelperThreadState& lock);
  bool canStartBaselineCompileTask(const AutoLockHelperThreadState& lock);
  bool canStartIonCompileTask(const AutoLockHelperThreadState& lock);
  bool canStartIonFreeTask(const AutoLockHelperThreadState& lock);
  bool canStartFreeDelazifyTask(const AutoLockHelperThreadState& lock);
  bool canStartDelazifyTask(const AutoLockHelperThreadState& lock);
  bool canStartCompressionTask(const AutoLockHelperThreadState& lock);
  bool canStartGCParallelTask(const AutoLockHelperThreadState& lock);

  HelperThreadTask* maybeGetWasmCompile(const AutoLockHelperThreadState& lock,
                                        wasm::CompileState state);

  HelperThreadTask* maybeGetWasmTier1CompileTask(
      const AutoLockHelperThreadState& lock);
  HelperThreadTask* maybeGetWasmTier2CompileTask(
      const AutoLockHelperThreadState& lock);
  HelperThreadTask* maybeGetWasmCompleteTier2GeneratorTask(
      const AutoLockHelperThreadState& lock);
  HelperThreadTask* maybeGetWasmPartialTier2CompileTask(
      const AutoLockHelperThreadState& lock);
  HelperThreadTask* maybeGetPromiseHelperTask(
      const AutoLockHelperThreadState& lock);
  HelperThreadTask* maybeGetBaselineCompileTask(
      const AutoLockHelperThreadState& lock);
  HelperThreadTask* maybeGetIonCompileTask(
      const AutoLockHelperThreadState& lock);
  HelperThreadTask* maybeGetLowPrioIonCompileTask(
      const AutoLockHelperThreadState& lock);
  HelperThreadTask* maybeGetIonFreeTask(const AutoLockHelperThreadState& lock);
  HelperThreadTask* maybeGetFreeDelazifyTask(
      const AutoLockHelperThreadState& lock);
  HelperThreadTask* maybeGetDelazifyTask(const AutoLockHelperThreadState& lock);
  HelperThreadTask* maybeGetCompressionTask(
      const AutoLockHelperThreadState& lock);
  HelperThreadTask* maybeGetGCParallelTask(
      const AutoLockHelperThreadState& lock);

  jit::IonCompileTask* highestPriorityPendingIonCompile(
      const AutoLockHelperThreadState& lock, bool checkExecutionStatus);

  bool checkTaskThreadLimit(ThreadType threadType, size_t maxThreads,
                            bool isMaster,
                            const AutoLockHelperThreadState& lock) const;
  bool checkTaskThreadLimit(ThreadType threadType, size_t maxThreads,
                            const AutoLockHelperThreadState& lock) const {
    return checkTaskThreadLimit(threadType, maxThreads, /* isMaster */ false,
                                lock);
  }

  bool hasActiveThreads(const AutoLockHelperThreadState&);
  bool canStartTasks(const AutoLockHelperThreadState& locked);

 public:
  // Used by a major GC to signal processing enqueued compression tasks.
  enum class ScheduleCompressionTask { GC, API };
  void startHandlingCompressionTasks(ScheduleCompressionTask schedule,
                                     JSRuntime* maybeRuntime,
                                     const AutoLockHelperThreadState& lock);

  void runPendingSourceCompressions(JSRuntime* runtime,
                                    AutoLockHelperThreadState& lock);

  void trace(JSTracer* trc);

  void waitForAllTasks();
  void waitForAllTasksLocked(AutoLockHelperThreadState&);

#ifdef DEBUG
  bool hasOffThreadIonCompile(Zone* zone, AutoLockHelperThreadState& lock);
#endif

  void cancelOffThreadBaselineCompile(const CompilationSelector& selector);
  void cancelOffThreadIonCompile(const CompilationSelector& selector);
  void cancelOffThreadWasmCompleteTier2Generator(
      AutoLockHelperThreadState& lock);
  void cancelOffThreadWasmPartialTier2Compile(AutoLockHelperThreadState& lock);

  bool hasAnyDelazifyTask(JSRuntime* rt, AutoLockHelperThreadState& lock);
  void cancelPendingDelazifyTask(JSRuntime* rt,
                                 AutoLockHelperThreadState& lock);
  void waitUntilCancelledDelazifyTasks(JSRuntime* rt,
                                       AutoLockHelperThreadState& lock);
  void waitUntilEmptyFreeDelazifyTaskVector(AutoLockHelperThreadState& lock);

  void cancelOffThreadCompressions(JSRuntime* runtime,
                                   AutoLockHelperThreadState& lock);

  void triggerFreeUnusedMemory();

  bool submitTask(wasm::UniqueCompleteTier2GeneratorTask task);
  bool submitTask(wasm::UniquePartialTier2CompileTask task);
  bool submitTask(wasm::CompileTask* task, wasm::CompileState state);
  bool submitTask(jit::BaselineCompileTask* task,
                  const AutoLockHelperThreadState& locked);
  bool submitTask(UniquePtr<jit::IonFreeTask>&& task,
                  const AutoLockHelperThreadState& lock);
  bool submitTask(jit::IonCompileTask* task,
                  const AutoLockHelperThreadState& locked);
  bool submitTask(UniquePtr<SourceCompressionTask> task,
                  const AutoLockHelperThreadState& locked);
  void submitTask(DelazifyTask* task, const AutoLockHelperThreadState& locked);
  bool submitTask(UniquePtr<FreeDelazifyTask> task,
                  const AutoLockHelperThreadState& locked);
  bool submitTask(PromiseHelperTask* task);
  bool submitTask(GCParallelTask* task,
                  const AutoLockHelperThreadState& locked);

  void runOneTask(HelperThreadTask* task, AutoLockHelperThreadState& lock);
  void dispatch(const AutoLockHelperThreadState& locked);

 private:
  HelperThreadTask* findHighestPriorityTask(
      const AutoLockHelperThreadState& locked);

  void runTaskLocked(HelperThreadTask* task, AutoLockHelperThreadState& lock);

  using Selector = HelperThreadTask* (
      GlobalHelperThreadState::*)(const AutoLockHelperThreadState&);
  static const Selector selectors[];
};

static inline bool IsHelperThreadStateInitialized() {
  extern GlobalHelperThreadState* gHelperThreadState;
  return gHelperThreadState;
}

static inline GlobalHelperThreadState& HelperThreadState() {
  extern GlobalHelperThreadState* gHelperThreadState;

  MOZ_ASSERT(gHelperThreadState);
  return *gHelperThreadState;
}

// Eagerly delazify functions, and send the result back to the runtime which
// requested the stencil to be parsed, by filling the stencil cache.
//
// This task is scheduled multiple times, each time it is scheduled, it
// delazifies a single function. Once the function is delazified, it schedules
// the inner functions of the delazified function for delazification using the
// DelazifyStrategy. The DelazifyStrategy is responsible for ordering and
// filtering functions to be delazified.
//
// When no more function have to be delazified, a FreeDelazifyTask is scheduled
// to remove the memory held by the DelazifyTask.
struct DelazifyTask : public mozilla::LinkedListElement<DelazifyTask>,
                      public HelperThreadTask {
  // HelperThreads are shared between all runtimes in the process so explicitly
  // track which one we are associated with.
  JSRuntime* maybeRuntime = nullptr;

  DelazificationContext delazificationCx;

  // Create a new DelazifyTask and initialize it.
  //
  // In case of early failure, no errors are reported, as a DelazifyTask is an
  // optimization and the VM should remain working even without this
  // optimization in place.
  static UniquePtr<DelazifyTask> Create(
      JSRuntime* maybeRuntime, const JS::ReadOnlyCompileOptions& options,
      frontend::InitialStencilAndDelazifications* stencils);

  DelazifyTask(JSRuntime* maybeRuntime,
               const JS::PrefableCompileOptions& initialPrefableOptions);
  ~DelazifyTask();

  [[nodiscard]] bool init(const JS::ReadOnlyCompileOptions& options,
                          frontend::InitialStencilAndDelazifications* stencils);

  bool runtimeMatchesOrNoRuntime(JSRuntime* rt) {
    return !maybeRuntime || maybeRuntime == rt;
  }

  size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) const;
  size_t sizeOfIncludingThis(mozilla::MallocSizeOf mallocSizeOf) const {
    return mallocSizeOf(this) + sizeOfExcludingThis(mallocSizeOf);
  }

  void runHelperThreadTask(AutoLockHelperThreadState& locked) override;
  [[nodiscard]] bool runTask();
  ThreadType threadType() override { return ThreadType::THREAD_TYPE_DELAZIFY; }

  bool done() const;

  const char* getName() override { return "DelazifyTask"; }
};

// The FreeDelazifyTask exists as this is a bad practice to `js_delete(this)`,
// as fields might be aliased across the destructor, such as with RAII guards.
// The FreeDelazifyTask limits the risk of adding these kind of issues by
// limiting the number of fields to the DelazifyTask pointer, before deleting
// it-self.
struct FreeDelazifyTask : public HelperThreadTask {
  DelazifyTask* task;

  explicit FreeDelazifyTask(DelazifyTask* t) : task(t) {}
  void runHelperThreadTask(AutoLockHelperThreadState& locked) override;
  ThreadType threadType() override {
    return ThreadType::THREAD_TYPE_DELAZIFY_FREE;
  }

  const char* getName() override { return "FreeDelazifyTask"; }
};

// It is not desirable to eagerly compress: if lazy functions that are tied to
// the ScriptSource were to be executed relatively soon after parsing, they
// would need to block on decompression, which hurts responsiveness.
//
// To this end, compression tasks are heap allocated and enqueued in a pending
// list by ScriptSource::setSourceCopy. When a major GC occurs, we schedule
// pending compression tasks and move the ones that are ready to be compressed
// to the worklist. Currently, a compression task is considered ready 2 major
// GCs after being enqueued. Completed tasks are handled during the sweeping
// phase by AttachCompressedSourcesTask, which runs in parallel with other GC
// sweeping tasks.
class SourceCompressionTask : public HelperThreadTask {
  friend class HelperThread;
  friend class ScriptSource;

  // The runtime that the ScriptSource is associated with, in the sense that
  // it uses the runtime's immutable string cache.
  JSRuntime* runtime_;

  // The major GC number of the runtime when the task was enqueued.
  uint64_t majorGCNumber_;

  // The source to be compressed.
  RefPtr<ScriptSource> source_;

  // The resultant compressed string. If the compressed string is larger
  // than the original, or we OOM'd during compression, or nothing else
  // except the task is holding the ScriptSource alive when scheduled to
  // compress, this will remain None upon completion.
  SharedImmutableString resultString_;

 public:
  // The majorGCNumber is used for scheduling tasks.
  SourceCompressionTask(JSRuntime* rt, ScriptSource* source)
      : runtime_(rt), majorGCNumber_(rt->gc.majorGCCount()), source_(source) {
    source->noteSourceCompressionTask();
  }
  virtual ~SourceCompressionTask() = default;

  bool runtimeMatches(JSRuntime* runtime) const { return runtime == runtime_; }
  bool shouldStart() const {
    // We wait 2 major GCs to start compressing, in order to avoid immediate
    // compression. If the script source has no other references then don't
    // compress it and let SweepPendingCompressions remove this task.
    return !shouldCancel() && runtime_->gc.majorGCCount() > majorGCNumber_ + 1;
  }

  bool shouldCancel() const {
    // If the refcount is exactly 1, then nothing else is holding on to the
    // ScriptSource, so no reason to compress it and we should cancel the task.
    return source_->refs == 1;
  }

  void runTask();
  void runHelperThreadTask(AutoLockHelperThreadState& locked) override;
  void complete();

  ThreadType threadType() override { return ThreadType::THREAD_TYPE_COMPRESS; }

  const char* getName() override { return "SourceCompressionTask"; }

 private:
  struct PerformTaskWork;
  friend struct PerformTaskWork;

  // The work algorithm, aware whether it's compressing one-byte UTF-8 source
  // text or UTF-16, for CharT either Utf8Unit or char16_t.  Invoked by
  // work() after doing a type-test of the ScriptSource*.
  template <typename CharT>
  void workEncodingSpecific();
};

// A PromiseHelperTask is an OffThreadPromiseTask that executes a single job on
// a helper thread. Call js::StartOffThreadPromiseHelperTask to submit a
// PromiseHelperTask for execution.
//
// Concrete subclasses must implement execute and OffThreadPromiseTask::resolve.
// The helper thread will call execute() to do the main work. Then, the thread
// of the JSContext used to create the PromiseHelperTask will call resolve() to
// resolve promise according to those results.
struct PromiseHelperTask : OffThreadPromiseTask, public HelperThreadTask {
  PromiseHelperTask(JSContext* cx, JS::Handle<PromiseObject*> promise)
      : OffThreadPromiseTask(cx, promise) {}

  // To be called on a helper thread and implemented by the derived class.
  virtual void execute() = 0;

  // May be called in the absence of helper threads or off-thread promise
  // support to synchronously execute and resolve a PromiseTask.
  //
  // Warning: After this function returns, 'this' can be deleted at any time, so
  // the caller must immediately return from the stream callback.
  void executeAndResolveAndDestroy(JSContext* cx);

  void runHelperThreadTask(AutoLockHelperThreadState& locked) override;
  ThreadType threadType() override { return THREAD_TYPE_PROMISE_TASK; }

  const char* getName() override { return "PromiseHelperTask"; }
};

} /* namespace js */

#endif /* vm_HelperThreadState_h */
