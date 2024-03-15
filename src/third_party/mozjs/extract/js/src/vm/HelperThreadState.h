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

#include "mozilla/Attributes.h"
#include "mozilla/EnumeratedArray.h"
#include "mozilla/RefPtr.h"  // RefPtr
#include "mozilla/TimeStamp.h"
#include "mozilla/Vector.h"  // mozilla::Vector

#include "ds/Fifo.h"
#include "frontend/CompilationStencil.h"  // CompilationStencil, CompilationGCOutput
#include "frontend/FrontendContext.h"
#include "js/CompileOptions.h"
#include "js/experimental/CompileScript.h"  // JS::CompilationStorage
#include "js/experimental/JSStencil.h"      // JS::InstantiationStorage
#include "js/HelperThreadAPI.h"
#include "js/TypeDecls.h"
#include "threading/ConditionVariable.h"
#include "vm/HelperThreads.h"
#include "vm/HelperThreadTask.h"
#include "vm/JSContext.h"
#include "vm/OffThreadPromiseRuntimeState.h"  // js::OffThreadPromiseTask

namespace js {

struct ParseTask;
struct DelazifyTask;
struct FreeDelazifyTask;
struct PromiseHelperTask;
class PromiseObject;

namespace jit {
class IonCompileTask;
class IonFreeTask;
}  // namespace jit

enum class ParseTaskKind {
  // The output is CompilationStencil for script.
  ScriptStencil,

  // The output is CompilationStencil for module.
  ModuleStencil,

  // The output is CompilationStencil for script/stencil.
  StencilDecode,

  // The output is an array of CompilationStencil.
  MultiStencilsDecode,
};

namespace wasm {

struct CompileTask;
typedef Fifo<CompileTask*, 0, SystemAllocPolicy> CompileTaskPtrFifo;

struct Tier2GeneratorTask : public HelperThreadTask {
  virtual ~Tier2GeneratorTask() = default;
  virtual void cancel() = 0;
};

using UniqueTier2GeneratorTask = UniquePtr<Tier2GeneratorTask>;
typedef Vector<Tier2GeneratorTask*, 0, SystemAllocPolicy>
    Tier2GeneratorTaskPtrVector;

}  // namespace wasm

// Per-process state for off thread work items.
class GlobalHelperThreadState {
  friend class AutoLockHelperThreadState;
  friend class AutoUnlockHelperThreadState;

 public:
  // A single tier-2 ModuleGenerator job spawns many compilation jobs, and we
  // do not want to allow more than one such ModuleGenerator to run at a time.
  static const size_t MaxTier2GeneratorTasks = 1;

  // Number of CPUs to treat this machine as having when creating threads.
  // May be accessed without locking.
  size_t cpuCount;

  // Number of threads to create. May be accessed without locking.
  size_t threadCount;

  // Thread stack quota to use when running tasks.
  size_t stackQuota;

  bool terminating_ = false;

  typedef Vector<jit::IonCompileTask*, 0, SystemAllocPolicy>
      IonCompileTaskVector;
  using IonFreeTaskVector =
      Vector<js::UniquePtr<jit::IonFreeTask>, 0, SystemAllocPolicy>;
  typedef Vector<UniquePtr<ParseTask>, 0, SystemAllocPolicy> ParseTaskVector;
  using ParseTaskList = mozilla::LinkedList<ParseTask>;
  using DelazifyTaskList = mozilla::LinkedList<DelazifyTask>;
  using FreeDelazifyTaskVector =
      Vector<js::UniquePtr<FreeDelazifyTask>, 1, SystemAllocPolicy>;
  typedef Vector<UniquePtr<SourceCompressionTask>, 0, SystemAllocPolicy>
      SourceCompressionTaskVector;
  typedef Vector<PromiseHelperTask*, 0, SystemAllocPolicy>
      PromiseHelperTaskVector;
  typedef Vector<JSContext*, 0, SystemAllocPolicy> ContextVector;

  // Count of running task by each threadType.
  mozilla::EnumeratedArray<ThreadType, ThreadType::THREAD_TYPE_MAX, size_t>
      runningTaskCount;
  size_t totalCountRunningTasks;

  WriteOnceData<JS::RegisterThreadCallback> registerThread;
  WriteOnceData<JS::UnregisterThreadCallback> unregisterThread;

 private:
  // The lists below are all protected by |lock|.

  // Ion compilation worklist and finished jobs.
  IonCompileTaskVector ionWorklist_, ionFinishedList_;
  IonFreeTaskVector ionFreeList_;

  // wasm worklists.
  wasm::CompileTaskPtrFifo wasmWorklist_tier1_;
  wasm::CompileTaskPtrFifo wasmWorklist_tier2_;
  wasm::Tier2GeneratorTaskPtrVector wasmTier2GeneratorWorklist_;

  // Count of finished Tier2Generator tasks.
  uint32_t wasmTier2GeneratorsFinished_;

  // Async tasks that, upon completion, are dispatched back to the JSContext's
  // owner thread via embedding callbacks instead of a finished list.
  PromiseHelperTaskVector promiseHelperTasks_;

  // Script parsing/emitting worklist and finished jobs.
  ParseTaskVector parseWorklist_;
  ParseTaskList parseFinishedList_;

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

  // GC tasks needing to be done in parallel.
  GCParallelTaskList gcParallelWorklist_;
  size_t gcParallelThreadCount;

  // Global list of JSContext for GlobalHelperThreadState to use.
  ContextVector helperContexts_;

  using HelperThreadTaskVector =
      Vector<HelperThreadTask*, 0, SystemAllocPolicy>;
  // Vector of running HelperThreadTask.
  // This is used to get the HelperThreadTask that are currently running.
  HelperThreadTaskVector helperTasks_;

  // Callback to dispatch a task to a thread pool. Set by
  // JS::SetHelperThreadTaskCallback. If this is not set the internal thread
  // pool is used.
  JS::HelperThreadTaskCallback dispatchTaskCallback = nullptr;

  // The number of tasks dispatched to the thread pool that have not started
  // running yet.
  size_t tasksPending_ = 0;

  bool isInitialized_ = false;

  bool useInternalThreadPool_ = true;

  ParseTask* removeFinishedParseTask(JSContext* cx, JS::OffThreadToken* token);

 public:
  void addSizeOfIncludingThis(JS::GlobalStats* stats,
                              const AutoLockHelperThreadState& lock) const;

  size_t maxIonCompilationThreads() const;
  size_t maxWasmCompilationThreads() const;
  size_t maxWasmTier2GeneratorThreads() const;
  size_t maxPromiseHelperThreads() const;
  size_t maxParseThreads() const;
  size_t maxCompressionThreads() const;
  size_t maxGCParallelThreads(const AutoLockHelperThreadState& lock) const;

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

  JSContext* getFirstUnusedContext(AutoLockHelperThreadState& locked);
  void destroyHelperContexts(AutoLockHelperThreadState& lock);

#ifdef DEBUG
  void assertIsLockedByCurrentThread() const;
#endif

  void wait(AutoLockHelperThreadState& locked,
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
  void remove(T& vector, size_t* index) {
    // Self-moving is undefined behavior.
    if (*index != vector.length() - 1) {
      vector[*index] = std::move(vector.back());
    }
    (*index)--;
    vector.popBack();
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
                                         wasm::CompileMode m) {
    switch (m) {
      case wasm::CompileMode::Once:
      case wasm::CompileMode::Tier1:
        return wasmWorklist_tier1_;
      case wasm::CompileMode::Tier2:
        return wasmWorklist_tier2_;
      default:
        MOZ_CRASH();
    }
  }

  wasm::Tier2GeneratorTaskPtrVector& wasmTier2GeneratorWorklist(
      const AutoLockHelperThreadState&) {
    return wasmTier2GeneratorWorklist_;
  }

  void incWasmTier2GeneratorsFinished(const AutoLockHelperThreadState&) {
    wasmTier2GeneratorsFinished_++;
  }

  uint32_t wasmTier2GeneratorsFinished(const AutoLockHelperThreadState&) const {
    return wasmTier2GeneratorsFinished_;
  }

  PromiseHelperTaskVector& promiseHelperTasks(
      const AutoLockHelperThreadState&) {
    return promiseHelperTasks_;
  }

  ParseTaskVector& parseWorklist(const AutoLockHelperThreadState&) {
    return parseWorklist_;
  }
  ParseTaskList& parseFinishedList(const AutoLockHelperThreadState&) {
    return parseFinishedList_;
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

  GCParallelTaskList& gcParallelWorklist() { return gcParallelWorklist_; }

  size_t getGCParallelThreadCount(const AutoLockHelperThreadState& lock) const {
    return gcParallelThreadCount;
  }
  void setGCParallelThreadCount(size_t count,
                                const AutoLockHelperThreadState& lock) {
    MOZ_ASSERT(count >= 1);
    MOZ_ASSERT(count <= threadCount);
    gcParallelThreadCount = count;
  }

  HelperThreadTaskVector& helperTasks(const AutoLockHelperThreadState&) {
    return helperTasks_;
  }

  bool canStartWasmCompile(const AutoLockHelperThreadState& lock,
                           wasm::CompileMode mode);

  bool canStartWasmTier1CompileTask(const AutoLockHelperThreadState& lock);
  bool canStartWasmTier2CompileTask(const AutoLockHelperThreadState& lock);
  bool canStartWasmTier2GeneratorTask(const AutoLockHelperThreadState& lock);
  bool canStartPromiseHelperTask(const AutoLockHelperThreadState& lock);
  bool canStartIonCompileTask(const AutoLockHelperThreadState& lock);
  bool canStartIonFreeTask(const AutoLockHelperThreadState& lock);
  bool canStartParseTask(const AutoLockHelperThreadState& lock);
  bool canStartFreeDelazifyTask(const AutoLockHelperThreadState& lock);
  bool canStartDelazifyTask(const AutoLockHelperThreadState& lock);
  bool canStartCompressionTask(const AutoLockHelperThreadState& lock);
  bool canStartGCParallelTask(const AutoLockHelperThreadState& lock);

  HelperThreadTask* maybeGetWasmCompile(const AutoLockHelperThreadState& lock,
                                        wasm::CompileMode mode);

  HelperThreadTask* maybeGetWasmTier1CompileTask(
      const AutoLockHelperThreadState& lock);
  HelperThreadTask* maybeGetWasmTier2CompileTask(
      const AutoLockHelperThreadState& lock);
  HelperThreadTask* maybeGetWasmTier2GeneratorTask(
      const AutoLockHelperThreadState& lock);
  HelperThreadTask* maybeGetPromiseHelperTask(
      const AutoLockHelperThreadState& lock);
  HelperThreadTask* maybeGetIonCompileTask(
      const AutoLockHelperThreadState& lock);
  HelperThreadTask* maybeGetLowPrioIonCompileTask(
      const AutoLockHelperThreadState& lock);
  HelperThreadTask* maybeGetIonFreeTask(const AutoLockHelperThreadState& lock);
  HelperThreadTask* maybeGetParseTask(const AutoLockHelperThreadState& lock);
  HelperThreadTask* maybeGetFreeDelazifyTask(
      const AutoLockHelperThreadState& lock);
  HelperThreadTask* maybeGetDelazifyTask(const AutoLockHelperThreadState& lock);
  HelperThreadTask* maybeGetCompressionTask(
      const AutoLockHelperThreadState& lock);
  HelperThreadTask* maybeGetGCParallelTask(
      const AutoLockHelperThreadState& lock);

  enum class ScheduleCompressionTask { GC, API };

  // Used by a major GC to signal processing enqueued compression tasks.
  void startHandlingCompressionTasks(ScheduleCompressionTask schedule,
                                     JSRuntime* maybeRuntime,
                                     const AutoLockHelperThreadState& lock);

  jit::IonCompileTask* highestPriorityPendingIonCompile(
      const AutoLockHelperThreadState& lock, bool checkExecutionStatus);

 private:
  UniquePtr<ParseTask> finishParseTaskCommon(JSContext* cx,
                                             JS::OffThreadToken* token);

  bool finishMultiParseTask(JSContext* cx, ParseTaskKind kind,
                            JS::OffThreadToken* token,
                            mozilla::Vector<RefPtr<JS::Stencil>>* stencils);

 public:
  void cancelParseTask(JSRuntime* rt, JS::OffThreadToken* token);
  void destroyParseTask(JSRuntime* rt, ParseTask* parseTask);

  void trace(JSTracer* trc);

  already_AddRefed<frontend::CompilationStencil> finishStencilTask(
      JSContext* cx, JS::OffThreadToken* token,
      JS::InstantiationStorage* storage);
  bool finishMultiStencilsDecodeTask(
      JSContext* cx, JS::OffThreadToken* token,
      mozilla::Vector<RefPtr<JS::Stencil>>* stencils);

  bool hasActiveThreads(const AutoLockHelperThreadState&);
  bool canStartTasks(const AutoLockHelperThreadState& locked);
  void waitForAllTasks();
  void waitForAllTasksLocked(AutoLockHelperThreadState&);

  bool checkTaskThreadLimit(ThreadType threadType, size_t maxThreads,
                            bool isMaster,
                            const AutoLockHelperThreadState& lock) const;
  bool checkTaskThreadLimit(ThreadType threadType, size_t maxThreads,
                            const AutoLockHelperThreadState& lock) const {
    return checkTaskThreadLimit(threadType, maxThreads, /* isMaster */ false,
                                lock);
  }

  void triggerFreeUnusedMemory();

 private:
  // Condition variable for notifiying the main thread that a helper task has
  // completed some work.
  js::ConditionVariable consumerWakeup;

  void dispatch(JS::DispatchReason reason,
                const AutoLockHelperThreadState& locked);

  void runTask(HelperThreadTask* task, AutoLockHelperThreadState& lock);

 public:
  bool submitTask(wasm::UniqueTier2GeneratorTask task);
  bool submitTask(wasm::CompileTask* task, wasm::CompileMode mode);
  bool submitTask(UniquePtr<jit::IonFreeTask> task,
                  const AutoLockHelperThreadState& lock);
  bool submitTask(jit::IonCompileTask* task,
                  const AutoLockHelperThreadState& locked);
  bool submitTask(UniquePtr<SourceCompressionTask> task,
                  const AutoLockHelperThreadState& locked);
  bool submitTask(JSRuntime* rt, UniquePtr<ParseTask> task,
                  const AutoLockHelperThreadState& locked);
  void submitTask(DelazifyTask* task, const AutoLockHelperThreadState& locked);
  bool submitTask(UniquePtr<FreeDelazifyTask> task,
                  const AutoLockHelperThreadState& locked);
  bool submitTask(PromiseHelperTask* task);
  bool submitTask(GCParallelTask* task,
                  const AutoLockHelperThreadState& locked);
  void runOneTask(AutoLockHelperThreadState& lock);
  void runTaskLocked(HelperThreadTask* task, AutoLockHelperThreadState& lock);

  using Selector = HelperThreadTask* (
      GlobalHelperThreadState::*)(const AutoLockHelperThreadState&);
  static const Selector selectors[];

  HelperThreadTask* findHighestPriorityTask(
      const AutoLockHelperThreadState& locked);
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

class MOZ_RAII AutoSetHelperThreadContext {
  JSContext* cx;
  AutoLockHelperThreadState& lock;

 public:
  AutoSetHelperThreadContext(const JS::ContextOptions& options,
                             AutoLockHelperThreadState& lock);
  ~AutoSetHelperThreadContext();
};

struct MOZ_RAII AutoSetContextRuntime {
  explicit AutoSetContextRuntime(JSRuntime* rt) {
    TlsContext.get()->setRuntime(rt);
  }
  ~AutoSetContextRuntime() { TlsContext.get()->setRuntime(nullptr); }
};

struct ParseTask : public mozilla::LinkedListElement<ParseTask>,
                   public JS::OffThreadToken,
                   public HelperThreadTask {
  ParseTaskKind kind;
  JS::OwningCompileOptions options;

  // Context options from the main thread.
  const JS::ContextOptions contextOptions;

  // HelperThreads are shared between all runtimes in the process so explicitly
  // track which one we are associated with.
  JSRuntime* runtime = nullptr;

  // Callback invoked off thread when the parse finishes.
  JS::OffThreadCompileCallback callback;
  void* callbackData;

  // For the multi-decode stencil case, holds onto the set of stencils produced
  // offthread
  mozilla::Vector<RefPtr<JS::Stencil>> stencils;

  // The input of the compilation.
  JS::CompilationStorage compileStorage_;

  // The output of the compilation/decode task.
  RefPtr<frontend::CompilationStencil> stencil_;

  JS::InstantiationStorage instantiationStorage_;

  // Record any errors happening while parsing or generating bytecode.
  FrontendContext fc_;

  ParseTask(ParseTaskKind kind, JSContext* cx,
            JS::OffThreadCompileCallback callback, void* callbackData);
  virtual ~ParseTask();

  bool init(JSContext* cx, const JS::ReadOnlyCompileOptions& options);

  void moveInstantiationStorageInto(JS::InstantiationStorage& storage);

  void activate(JSRuntime* rt);
  void deactivate(JSRuntime* rt);

  virtual void parse(FrontendContext* fc) = 0;

  bool runtimeMatches(JSRuntime* rt) { return runtime == rt; }

  void trace(JSTracer* trc);

  size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) const;
  size_t sizeOfIncludingThis(mozilla::MallocSizeOf mallocSizeOf) const {
    return mallocSizeOf(this) + sizeOfExcludingThis(mallocSizeOf);
  }

  void runHelperThreadTask(AutoLockHelperThreadState& locked) override;
  void runTask(AutoLockHelperThreadState& lock);
  void scheduleDelazifyTask(AutoLockHelperThreadState& lock);
  ThreadType threadType() override { return ThreadType::THREAD_TYPE_PARSE; }
};

// Base class for implementing the various strategies to iterate over the
// functions to be delazified, or to decide when to stop doing any
// delazification.
//
// When created, the `add` function should be called with the top-level
// ScriptIndex.
struct DelazifyStrategy {
  using ScriptIndex = frontend::ScriptIndex;
  virtual ~DelazifyStrategy() = default;

  // Returns true if no more functions should be delazified. Note, this does not
  // imply that every function got delazified.
  virtual bool done() const = 0;

  // Return a function identifier which represent the next function to be
  // delazified. If no more function should be delazified, then return 0.
  virtual ScriptIndex next() = 0;

  // Empty the list of functions to be processed next. done() should return true
  // after this call.
  virtual void clear() = 0;

  // Insert an index in the container of the delazification strategy. A strategy
  // can choose to ignore the insertion of an index in its queue of function to
  // delazify. Return false only in case of errors while inserting, and true
  // otherwise.
  [[nodiscard]] virtual bool insert(ScriptIndex index,
                                    frontend::ScriptStencilRef& ref) = 0;

  // Add the inner functions of a delazified function. This function should only
  // be called with a function which has some bytecode associated with it, and
  // register functions which parent are already delazified.
  //
  // This function is called with the script index of:
  //  - top-level script, when starting the off-thread delazification.
  //  - functions added by `add` and delazified by `DelazifyTask`.
  [[nodiscard]] bool add(FrontendContext* fc,
                         const frontend::CompilationStencil& stencil,
                         ScriptIndex index);
};

// Delazify all functions using a Depth First traversal of the function-tree
// ordered, where each functions is visited in source-order.
//
// When `add` is called with the top-level ScriptIndex. This will push all inner
// functions to a stack such that they are popped in source order. Each
// function, once delazified, would be used to schedule their inner functions
// the same way.
//
// Hypothesis: This strategy parses all functions in source order, with the
// expectation that calls will follow the same order, and that helper thread
// would always be ahead of the execution.
struct DepthFirstDelazification final : public DelazifyStrategy {
  Vector<ScriptIndex, 0, SystemAllocPolicy> stack;

  bool done() const override { return stack.empty(); }
  ScriptIndex next() override { return stack.popCopy(); }
  void clear() override { return stack.clear(); }
  bool insert(ScriptIndex index, frontend::ScriptStencilRef&) override {
    return stack.append(index);
  }
};

// Delazify all functions using a traversal which select the largest function
// first. The intent being that if the main thread races with the helper thread,
// then the main thread should only have to parse small functions instead of the
// large ones which would be prioritized by this delazification strategy.
struct LargeFirstDelazification final : public DelazifyStrategy {
  using SourceSize = uint32_t;
  Vector<std::pair<SourceSize, ScriptIndex>, 0, SystemAllocPolicy> heap;

  bool done() const override { return heap.empty(); }
  ScriptIndex next() override;
  void clear() override { return heap.clear(); }
  bool insert(ScriptIndex, frontend::ScriptStencilRef&) override;
};

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
  JSRuntime* runtime = nullptr;

  // Context options originally from the main thread.
  const JS::ContextOptions contextOptions;

  // Queue of functions to be processed while delazifying.
  UniquePtr<DelazifyStrategy> strategy;

  // Every delazified function is merged back to provide context for delazifying
  // even more functions.
  frontend::CompilationStencilMerger merger;

  // Record any errors happening while parsing or generating bytecode.
  FrontendContext fc_;

  // Create a new DelazifyTask and initialize it.
  //
  // In case of early failure, no errors are reported, as a DelazifyTask is an
  // optimization and the VM should remain working even without this
  // optimization in place.
  static UniquePtr<DelazifyTask> Create(
      JSRuntime* runtime, const JS::ContextOptions& contextOptions,
      const JS::ReadOnlyCompileOptions& options,
      const frontend::CompilationStencil& stencil);

  DelazifyTask(JSRuntime* runtime, const JS::ContextOptions& options);
  ~DelazifyTask();

  [[nodiscard]] bool init(
      const JS::ReadOnlyCompileOptions& options,
      UniquePtr<frontend::ExtensibleCompilationStencil>&& initial);

  // This function is called by delazify task thread to know whether the task
  // should be interrupted.
  //
  // A delazify task holds on a thread until all functions iterated over by the
  // strategy. However, as a delazify task iterates over multiple functions, it
  // can easily be interrupted at function boundaries.
  //
  // TODO: (Bug 1773683) Plug this with the mozilla::Task::RequestInterrupt
  // function which is wrapping HelperThreads tasks within Mozilla.
  bool isInterrupted() { return false; }

  bool runtimeMatches(JSRuntime* rt) { return runtime == rt; }

  size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) const;
  size_t sizeOfIncludingThis(mozilla::MallocSizeOf mallocSizeOf) const {
    return mallocSizeOf(this) + sizeOfExcludingThis(mallocSizeOf);
  }

  void runHelperThreadTask(AutoLockHelperThreadState& locked) override;
  [[nodiscard]] bool runTask(JSContext* cx);
  ThreadType threadType() override { return ThreadType::THREAD_TYPE_DELAZIFY; }
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
    // We wait 2 major GCs to start compressing, in order to avoid
    // immediate compression.
    return runtime_->gc.majorGCCount() > majorGCNumber_ + 1;
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
  PromiseHelperTask(JSContext* cx, Handle<PromiseObject*> promise)
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
};

} /* namespace js */

#endif /* vm_HelperThreadState_h */
