/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "vm/HelperThreads.h"

#include "mozilla/ReverseIterator.h"  // mozilla::Reversed(...)
#include "mozilla/ScopeExit.h"
#include "mozilla/Span.h"  // mozilla::Span<TaggedScriptThingIndex>

#include <algorithm>

#include "frontend/CompilationStencil.h"  // frontend::CompilationStencil
#include "gc/GC.h"
#include "gc/Zone.h"
#include "jit/BaselineCompileTask.h"
#include "jit/Ion.h"
#include "jit/IonCompileTask.h"
#include "jit/JitRuntime.h"
#include "jit/JitScript.h"
#include "js/CompileOptions.h"  // JS::PrefableCompileOptions, JS::ReadOnlyCompileOptions
#include "js/experimental/CompileScript.h"  // JS::ThreadStackQuotaForSize
#include "js/friend/StackLimits.h"          // js::ReportOverRecursed
#include "js/HelperThreadAPI.h"
#include "js/Stack.h"
#include "js/UniquePtr.h"
#include "js/Utility.h"
#include "threading/CpuCount.h"
#include "vm/ErrorReporting.h"
#include "vm/HelperThreadState.h"
#include "vm/InternalThreadPool.h"
#include "vm/MutexIDs.h"
#include "wasm/WasmGenerator.h"

using namespace js;

using mozilla::TimeDuration;

static void CancelOffThreadWasmCompleteTier2GeneratorLocked(
    AutoLockHelperThreadState& lock);
static void CancelOffThreadWasmPartialTier2CompileLocked(
    AutoLockHelperThreadState& lock);

// This file is structured as follows:
//
// (1) Methods for GlobalHelperThreadState, and top level scheduling logic
// (2) Specifics for JS task classes
// (3) Specifics for wasm task classes

///////////////////////////////////////////////////////////////////////////
//                                                                       //
// GlobalHelperThreadState methods and top-level scheduling logic        //
//                                                                       //
///////////////////////////////////////////////////////////////////////////

namespace js {

MOZ_RUNINIT Mutex gHelperThreadLock(mutexid::GlobalHelperThreadState);
GlobalHelperThreadState* gHelperThreadState = nullptr;

}  // namespace js

bool js::CreateHelperThreadsState() {
  MOZ_ASSERT(!gHelperThreadState);
  gHelperThreadState = js_new<GlobalHelperThreadState>();
  return gHelperThreadState;
}

void js::DestroyHelperThreadsState() {
  AutoLockHelperThreadState lock;

  if (!gHelperThreadState) {
    return;
  }

  gHelperThreadState->finish(lock);
  js_delete(gHelperThreadState);
  gHelperThreadState = nullptr;
}

bool js::EnsureHelperThreadsInitialized() {
  MOZ_ASSERT(gHelperThreadState);
  return gHelperThreadState->ensureInitialized();
}

static size_t ClampDefaultCPUCount(size_t cpuCount) {
  // It's extremely rare for SpiderMonkey to have more than a few cores worth
  // of work. At higher core counts, performance can even decrease due to NUMA
  // (and SpiderMonkey's lack of NUMA-awareness), contention, and general lack
  // of optimization for high core counts. So to avoid wasting thread stack
  // resources (and cluttering gdb and core dumps), clamp to 8 cores for now.
  return std::min<size_t>(cpuCount, 8);
}

static size_t ThreadCountForCPUCount(size_t cpuCount) {
  // We need at least two threads for tier-2 wasm compilations, because
  // there's a master task that holds a thread while other threads do the
  // compilation.
  return std::max<size_t>(cpuCount, 2);
}

bool js::SetFakeCPUCount(size_t count) {
  HelperThreadState().setCpuCount(count);
  return true;
}

void GlobalHelperThreadState::setCpuCount(size_t count) {
  // This must be called before any threads have been initialized.
  AutoLockHelperThreadState lock;
  MOZ_ASSERT(!isInitialized(lock));

  // We can't do this if an external thread pool is in use.
  MOZ_ASSERT(!dispatchTaskCallback);

  cpuCount = count;
  threadCount = ThreadCountForCPUCount(count);
}

size_t js::GetHelperThreadCount() { return HelperThreadState().threadCount; }

size_t js::GetHelperThreadCPUCount() { return HelperThreadState().cpuCount; }

void JS::SetProfilingThreadCallbacks(
    JS::RegisterThreadCallback registerThread,
    JS::UnregisterThreadCallback unregisterThread) {
  HelperThreadState().registerThread = registerThread;
  HelperThreadState().unregisterThread = unregisterThread;
}

// Bug 1630189: Without MOZ_NEVER_INLINE, Windows PGO builds have a linking
// error for HelperThreadTaskCallback.
JS_PUBLIC_API MOZ_NEVER_INLINE void JS::SetHelperThreadTaskCallback(
    HelperThreadTaskCallback callback, size_t threadCount, size_t stackSize) {
  AutoLockHelperThreadState lock;
  HelperThreadState().setDispatchTaskCallback(callback, threadCount, stackSize,
                                              lock);
}

JS_PUBLIC_API MOZ_NEVER_INLINE const char* JS::GetHelperThreadTaskName(
    HelperThreadTask* task) {
  return task->getName();
}

void GlobalHelperThreadState::setDispatchTaskCallback(
    JS::HelperThreadTaskCallback callback, size_t threadCount, size_t stackSize,
    const AutoLockHelperThreadState& lock) {
  MOZ_ASSERT(!isInitialized(lock));
  MOZ_ASSERT(!dispatchTaskCallback);
  MOZ_ASSERT(threadCount != 0);
  MOZ_ASSERT(stackSize >= 16 * 1024);

  dispatchTaskCallback = callback;
  this->threadCount = threadCount;
  this->stackQuota = JS::ThreadStackQuotaForSize(stackSize);
}

bool GlobalHelperThreadState::ensureInitialized() {
  MOZ_ASSERT(CanUseExtraThreads());
  MOZ_ASSERT(this == &HelperThreadState());

  AutoLockHelperThreadState lock;

  if (isInitialized(lock)) {
    return true;
  }

  for (size_t& i : runningTaskCount) {
    i = 0;
  }

  useInternalThreadPool_ = !dispatchTaskCallback;
  if (useInternalThreadPool(lock)) {
    if (!InternalThreadPool::Initialize(threadCount, lock)) {
      return false;
    }
  }

  MOZ_ASSERT(dispatchTaskCallback);

  if (!ensureThreadCount(threadCount, lock)) {
    finishThreads(lock);
    return false;
  }

  MOZ_ASSERT(threadCount != 0);
  isInitialized_ = true;
  return true;
}

bool GlobalHelperThreadState::ensureThreadCount(
    size_t count, AutoLockHelperThreadState& lock) {
  if (!helperTasks_.reserve(count)) {
    return false;
  }

  if (useInternalThreadPool(lock)) {
    InternalThreadPool& pool = InternalThreadPool::Get();
    if (pool.threadCount(lock) < count) {
      if (!pool.ensureThreadCount(count, lock)) {
        return false;
      }

      threadCount = pool.threadCount(lock);
    }
  }

  return true;
}

GlobalHelperThreadState::GlobalHelperThreadState()
    : cpuCount(0),
      threadCount(0),
      totalCountRunningTasks(0),
      registerThread(nullptr),
      unregisterThread(nullptr),
      wasmCompleteTier2GeneratorsFinished_(0) {
  MOZ_ASSERT(!gHelperThreadState);

  cpuCount = ClampDefaultCPUCount(GetCPUCount());
  threadCount = ThreadCountForCPUCount(cpuCount);

  MOZ_ASSERT(cpuCount > 0, "GetCPUCount() seems broken");
}

void GlobalHelperThreadState::finish(AutoLockHelperThreadState& lock) {
  if (!isInitialized(lock)) {
    return;
  }

  MOZ_ASSERT_IF(!JSRuntime::hasLiveRuntimes(), gcParallelMarkingThreads == 0);

  finishThreads(lock);

  // Make sure there are no Ion free tasks left. We check this here because,
  // unlike the other tasks, we don't explicitly block on this when
  // destroying a runtime.
  auto& freeList = ionFreeList(lock);
  while (!freeList.empty()) {
    UniquePtr<jit::IonFreeTask> task = std::move(freeList.back());
    freeList.popBack();
    jit::FreeIonCompileTasks(task->compileTasks());
  }
}

void GlobalHelperThreadState::finishThreads(AutoLockHelperThreadState& lock) {
  waitForAllTasksLocked(lock);
  terminating_ = true;

  if (InternalThreadPool::IsInitialized()) {
    InternalThreadPool::ShutDown(lock);
  }
}

#ifdef DEBUG
void GlobalHelperThreadState::assertIsLockedByCurrentThread() const {
  gHelperThreadLock.assertOwnedByCurrentThread();
}
#endif  // DEBUG

void GlobalHelperThreadState::dispatch(const AutoLockHelperThreadState& lock) {
  if (helperTasks_.length() >= threadCount) {
    return;
  }

  HelperThreadTask* task = findHighestPriorityTask(lock);
  if (!task) {
    return;
  }

#ifdef DEBUG
  MOZ_ASSERT(tasksPending_ < threadCount);
  tasksPending_++;
#endif

  // Add task to list of running tasks immediately.
  helperTasks(lock).infallibleEmplaceBack(task);
  runningTaskCount[task->threadType()]++;
  totalCountRunningTasks++;

  lock.queueTaskToDispatch(task);
}

void GlobalHelperThreadState::wait(
    AutoLockHelperThreadState& lock,
    TimeDuration timeout /* = TimeDuration::Forever() */) {
  MOZ_ASSERT(!lock.hasQueuedTasks());
  consumerWakeup.wait_for(lock, timeout);
}

void GlobalHelperThreadState::notifyAll(const AutoLockHelperThreadState&) {
  consumerWakeup.notify_all();
}

void GlobalHelperThreadState::notifyOne(const AutoLockHelperThreadState&) {
  consumerWakeup.notify_one();
}

bool GlobalHelperThreadState::hasActiveThreads(
    const AutoLockHelperThreadState& lock) {
  return !helperTasks(lock).empty();
}

void js::WaitForAllHelperThreads() { HelperThreadState().waitForAllTasks(); }

void js::WaitForAllHelperThreads(AutoLockHelperThreadState& lock) {
  HelperThreadState().waitForAllTasksLocked(lock);
}

void GlobalHelperThreadState::waitForAllTasks() {
  AutoLockHelperThreadState lock;
  waitForAllTasksLocked(lock);
}

void GlobalHelperThreadState::waitForAllTasksLocked(
    AutoLockHelperThreadState& lock) {
  CancelOffThreadWasmCompleteTier2GeneratorLocked(lock);
  CancelOffThreadWasmPartialTier2CompileLocked(lock);

  while (canStartTasks(lock) || hasActiveThreads(lock)) {
    wait(lock);
  }

  MOZ_ASSERT(tasksPending_ == 0);
  MOZ_ASSERT(gcParallelWorklist().isEmpty(lock));
  MOZ_ASSERT(ionWorklist(lock).empty());
  MOZ_ASSERT(wasmWorklist(lock, wasm::CompileState::EagerTier1).empty());
  MOZ_ASSERT(promiseHelperTasks(lock).empty());
  MOZ_ASSERT(compressionWorklist(lock).empty());
  MOZ_ASSERT(ionFreeList(lock).empty());
  MOZ_ASSERT(wasmWorklist(lock, wasm::CompileState::EagerTier2).empty());
  MOZ_ASSERT(wasmCompleteTier2GeneratorWorklist(lock).empty());
  MOZ_ASSERT(wasmPartialTier2CompileWorklist(lock).empty());
  MOZ_ASSERT(!tasksPending_);
  MOZ_ASSERT(!hasActiveThreads(lock));
}

// A task can be a "master" task, ie, it will block waiting for other worker
// threads that perform work on its behalf.  If so it must not take the last
// available thread; there must always be at least one worker thread able to do
// the actual work.  (Or the system may deadlock.)
//
// If a task is a master task it *must* pass isMaster=true here, or perform a
// similar calculation to avoid deadlock from starvation.
//
// isMaster should only be true if the thread calling checkTaskThreadLimit() is
// a helper thread.
//
// NOTE: Calling checkTaskThreadLimit() from a helper thread in the dynamic
// region after currentTask.emplace() and before currentTask.reset() may cause
// it to return a different result than if it is called outside that dynamic
// region, as the predicate inspects the values of the threads' currentTask
// members.

bool GlobalHelperThreadState::checkTaskThreadLimit(
    ThreadType threadType, size_t maxThreads, bool isMaster,
    const AutoLockHelperThreadState& lock) const {
  MOZ_ASSERT(maxThreads >= 1);
  MOZ_ASSERT(maxThreads <= threadCount);

  // Check thread limit for this task kind.
  size_t count = runningTaskCount[threadType];
  if (count >= maxThreads) {
    return false;
  }

  // Check overall idle thread count taking into account master threads. A
  // master thread must not use the last idle thread or it will deadlock itself.
  MOZ_ASSERT(threadCount >= totalCountRunningTasks);
  size_t idleCount = threadCount - totalCountRunningTasks;
  size_t idleRequired = isMaster ? 2 : 1;
  return idleCount >= idleRequired;
}

static inline bool IsHelperThreadSimulatingOOM(js::ThreadType threadType) {
#if defined(DEBUG) || defined(JS_OOM_BREAKPOINT)
  return js::oom::simulator.targetThread() == threadType;
#else
  return false;
#endif
}

void GlobalHelperThreadState::addSizeOfIncludingThis(
    JS::GlobalStats* stats, const AutoLockHelperThreadState& lock) const {
#ifdef DEBUG
  assertIsLockedByCurrentThread();
#endif

  mozilla::MallocSizeOf mallocSizeOf = stats->mallocSizeOf_;
  JS::HelperThreadStats& htStats = stats->helperThread;

  htStats.stateData += mallocSizeOf(this);

  if (InternalThreadPool::IsInitialized()) {
    htStats.stateData +=
        InternalThreadPool::Get().sizeOfIncludingThis(mallocSizeOf, lock);
  }

  // Report memory used by various containers
  htStats.stateData +=
      ionWorklist_.sizeOfExcludingThis(mallocSizeOf) +
      ionFinishedList_.sizeOfExcludingThis(mallocSizeOf) +
      ionFreeList_.sizeOfExcludingThis(mallocSizeOf) +
      wasmWorklist_tier1_.sizeOfExcludingThis(mallocSizeOf) +
      wasmWorklist_tier2_.sizeOfExcludingThis(mallocSizeOf) +
      wasmCompleteTier2GeneratorWorklist_.sizeOfExcludingThis(mallocSizeOf) +
      wasmPartialTier2CompileWorklist_.sizeOfExcludingThis(mallocSizeOf) +
      promiseHelperTasks_.sizeOfExcludingThis(mallocSizeOf) +
      compressionPendingList_.sizeOfExcludingThis(mallocSizeOf) +
      compressionWorklist_.sizeOfExcludingThis(mallocSizeOf) +
      compressionFinishedList_.sizeOfExcludingThis(mallocSizeOf) +
      gcParallelWorklist_.sizeOfExcludingThis(mallocSizeOf, lock) +
      helperTasks_.sizeOfExcludingThis(mallocSizeOf);

  // Report IonCompileTasks on wait lists
  for (auto task : ionWorklist_) {
    htStats.ionCompileTask += task->sizeOfExcludingThis(mallocSizeOf);
  }
  for (auto task : ionFinishedList_) {
    htStats.ionCompileTask += task->sizeOfExcludingThis(mallocSizeOf);
  }
  for (const auto& task : ionFreeList_) {
    for (auto* compileTask : task->compileTasks()) {
      htStats.ionCompileTask += compileTask->sizeOfExcludingThis(mallocSizeOf);
    }
  }

  // Report wasm::CompileTasks on wait lists
  for (auto task : wasmWorklist_tier1_) {
    htStats.wasmCompile += task->sizeOfExcludingThis(mallocSizeOf);
  }
  for (auto task : wasmWorklist_tier2_) {
    htStats.wasmCompile += task->sizeOfExcludingThis(mallocSizeOf);
  }

  // Report number of helper threads.
  MOZ_ASSERT(htStats.idleThreadCount == 0);
  MOZ_ASSERT(threadCount >= totalCountRunningTasks);
  htStats.activeThreadCount = totalCountRunningTasks;
  htStats.idleThreadCount = threadCount - totalCountRunningTasks;
}

size_t GlobalHelperThreadState::maxBaselineCompilationThreads() const {
  if (IsHelperThreadSimulatingOOM(js::THREAD_TYPE_BASELINE)) {
    return 1;
  }
  return threadCount;
}

size_t GlobalHelperThreadState::maxIonCompilationThreads() const {
  if (IsHelperThreadSimulatingOOM(js::THREAD_TYPE_ION)) {
    return 1;
  }
  return threadCount;
}

size_t GlobalHelperThreadState::maxIonFreeThreads() const {
  // IonFree tasks are low priority. Limit to one thread to help avoid jemalloc
  // lock contention.
  return 1;
}

size_t GlobalHelperThreadState::maxPromiseHelperThreads() const {
  if (IsHelperThreadSimulatingOOM(js::THREAD_TYPE_PROMISE_TASK)) {
    return 1;
  }
  return std::min(cpuCount, threadCount);
}

size_t GlobalHelperThreadState::maxDelazifyThreads() const {
  if (IsHelperThreadSimulatingOOM(js::THREAD_TYPE_DELAZIFY)) {
    return 1;
  }
  return std::min(cpuCount, threadCount);
}

size_t GlobalHelperThreadState::maxCompressionThreads() const {
  if (IsHelperThreadSimulatingOOM(js::THREAD_TYPE_COMPRESS)) {
    return 1;
  }

  // Compression is triggered on major GCs to compress ScriptSources. It is
  // considered low priority work.
  return 1;
}

size_t GlobalHelperThreadState::maxGCParallelThreads() const {
  if (IsHelperThreadSimulatingOOM(js::THREAD_TYPE_GCPARALLEL)) {
    return 1;
  }
  return threadCount;
}

size_t GlobalHelperThreadState::maxWasmCompilationThreads() const {
  if (IsHelperThreadSimulatingOOM(js::THREAD_TYPE_WASM_COMPILE_TIER1) ||
      IsHelperThreadSimulatingOOM(js::THREAD_TYPE_WASM_COMPILE_TIER2)) {
    return 1;
  }
  return std::min(cpuCount, threadCount);
}

size_t js::GetMaxWasmCompilationThreads() {
  return HelperThreadState().maxWasmCompilationThreads();
}

size_t GlobalHelperThreadState::maxWasmCompleteTier2GeneratorThreads() const {
  return MaxCompleteTier2GeneratorTasks;
}

size_t GlobalHelperThreadState::maxWasmPartialTier2CompileThreads() const {
  return MaxPartialTier2CompileTasks;
}

void GlobalHelperThreadState::trace(JSTracer* trc) {
  {
    AutoLockHelperThreadState lock;

#ifdef DEBUG
    // Since we hold the helper thread lock here we must disable GCMarker's
    // checking of the atom marking bitmap since that also relies on taking the
    // lock.
    GCMarker* marker = nullptr;
    if (trc->isMarkingTracer()) {
      marker = GCMarker::fromTracer(trc);
      marker->setCheckAtomMarking(false);
    }
    auto reenableAtomMarkingCheck = mozilla::MakeScopeExit([marker] {
      if (marker) {
        marker->setCheckAtomMarking(true);
      }
    });
#endif

    for (auto task : baselineWorklist(lock)) {
      task->trace(trc);
    }
    for (auto task : baselineFinishedList(lock)) {
      task->trace(trc);
    }

    for (auto task : ionWorklist(lock)) {
      task->alloc().lifoAlloc()->setReadWrite();
      task->trace(trc);
      task->alloc().lifoAlloc()->setReadOnly();
    }
    for (auto task : ionFinishedList(lock)) {
      task->trace(trc);
    }

    for (auto* helper : helperTasks(lock)) {
      if (helper->is<jit::IonCompileTask>()) {
        jit::IonCompileTask* ionCompileTask = helper->as<jit::IonCompileTask>();
        ionCompileTask->alloc().lifoAlloc()->setReadWrite();
        ionCompileTask->trace(trc);
      } else if (helper->is<jit::BaselineCompileTask>()) {
        helper->as<jit::BaselineCompileTask>()->trace(trc);
      }
    }
  }

  // The lazy link list is only accessed on the main thread, so trace it after
  // releasing the lock.
  JSRuntime* rt = trc->runtime();
  if (auto* jitRuntime = rt->jitRuntime()) {
    jit::IonCompileTask* task = jitRuntime->ionLazyLinkList(rt).getFirst();
    while (task) {
      task->trace(trc);
      task = task->getNext();
    }
  }
}

// Definition of helper thread tasks.
//
// Priority is determined by the order they're listed here.
const GlobalHelperThreadState::Selector GlobalHelperThreadState::selectors[] = {
    &GlobalHelperThreadState::maybeGetGCParallelTask,
    &GlobalHelperThreadState::maybeGetBaselineCompileTask,
    &GlobalHelperThreadState::maybeGetIonCompileTask,
    &GlobalHelperThreadState::maybeGetWasmTier1CompileTask,
    &GlobalHelperThreadState::maybeGetPromiseHelperTask,
    &GlobalHelperThreadState::maybeGetFreeDelazifyTask,
    &GlobalHelperThreadState::maybeGetDelazifyTask,
    &GlobalHelperThreadState::maybeGetCompressionTask,
    &GlobalHelperThreadState::maybeGetLowPrioIonCompileTask,
    &GlobalHelperThreadState::maybeGetIonFreeTask,
    &GlobalHelperThreadState::maybeGetWasmPartialTier2CompileTask,
    &GlobalHelperThreadState::maybeGetWasmTier2CompileTask,
    &GlobalHelperThreadState::maybeGetWasmCompleteTier2GeneratorTask};

bool GlobalHelperThreadState::canStartTasks(
    const AutoLockHelperThreadState& lock) {
  return canStartGCParallelTask(lock) || canStartBaselineCompileTask(lock) ||
         canStartIonCompileTask(lock) || canStartWasmTier1CompileTask(lock) ||
         canStartPromiseHelperTask(lock) || canStartFreeDelazifyTask(lock) ||
         canStartDelazifyTask(lock) || canStartCompressionTask(lock) ||
         canStartIonFreeTask(lock) || canStartWasmTier2CompileTask(lock) ||
         canStartWasmCompleteTier2GeneratorTask(lock) ||
         canStartWasmPartialTier2CompileTask(lock);
}

void JS::RunHelperThreadTask(HelperThreadTask* task) {
  MOZ_ASSERT(task);
  MOZ_ASSERT(CanUseExtraThreads());

  AutoLockHelperThreadState lock;

  if (!gHelperThreadState || HelperThreadState().isTerminating(lock)) {
    return;
  }

  HelperThreadState().runOneTask(task, lock);
  HelperThreadState().dispatch(lock);
}

void GlobalHelperThreadState::runOneTask(HelperThreadTask* task,
                                         AutoLockHelperThreadState& lock) {
#ifdef DEBUG
  MOZ_ASSERT(tasksPending_ > 0);
  tasksPending_--;
#endif

  runTaskLocked(task, lock);

  notifyAll(lock);
}

HelperThreadTask* GlobalHelperThreadState::findHighestPriorityTask(
    const AutoLockHelperThreadState& locked) {
  // Return the highest priority task that is ready to start, or nullptr.

  for (const auto& selector : selectors) {
    if (auto* task = (this->*(selector))(locked)) {
      return task;
    }
  }

  return nullptr;
}

#ifdef DEBUG
static bool VectorHasTask(
    const Vector<HelperThreadTask*, 0, SystemAllocPolicy>& tasks,
    HelperThreadTask* task) {
  for (HelperThreadTask* t : tasks) {
    if (t == task) {
      return true;
    }
  }

  return false;
}
#endif

void GlobalHelperThreadState::runTaskLocked(HelperThreadTask* task,
                                            AutoLockHelperThreadState& locked) {
  ThreadType threadType = task->threadType();

  MOZ_ASSERT(VectorHasTask(helperTasks(locked), task));
  MOZ_ASSERT(totalCountRunningTasks != 0);
  MOZ_ASSERT(runningTaskCount[threadType] != 0);

  js::oom::SetThreadType(threadType);

  {
    JS::AutoSuppressGCAnalysis nogc;
    task->runHelperThreadTask(locked);
  }

  js::oom::SetThreadType(js::THREAD_TYPE_NONE);

  helperTasks(locked).eraseIfEqual(task);
  totalCountRunningTasks--;
  runningTaskCount[threadType]--;
}

void AutoHelperTaskQueue::queueTaskToDispatch(
    JS::HelperThreadTask* task) const {
  // This is marked const because it doesn't release the mutex.

  task->onThreadPoolDispatch();

  AutoEnterOOMUnsafeRegion oomUnsafe;
  if (!tasksToDispatch.append(task)) {
    oomUnsafe.crash("AutoLockHelperThreadState::queueTaskToDispatch");
  }
}

void AutoHelperTaskQueue::dispatchQueuedTasks() {
  // The hazard analysis can't tell that the callback doesn't GC.
  JS::AutoSuppressGCAnalysis nogc;

  for (size_t i = 0; i < tasksToDispatch.length(); i++) {
    HelperThreadState().dispatchTaskCallback(tasksToDispatch[i]);
  }
  tasksToDispatch.clear();
}

///////////////////////////////////////////////////////////////////////////
//                                                                       //
// JS task definitions                                                   //
//                                                                       //
///////////////////////////////////////////////////////////////////////////

//== IonCompileTask and CompilationSelector ===============================

bool GlobalHelperThreadState::canStartIonCompileTask(
    const AutoLockHelperThreadState& lock) {
  return !ionWorklist(lock).empty() &&
         checkTaskThreadLimit(THREAD_TYPE_ION, maxIonCompilationThreads(),
                              lock);
}

static bool IonCompileTaskHasHigherPriority(jit::IonCompileTask* first,
                                            jit::IonCompileTask* second) {
  // Return true if priority(first) > priority(second).
  //
  // This method can return whatever it wants, though it really ought to be a
  // total order. The ordering is allowed to race (change on the fly), however.

  // A higher warm-up counter indicates a higher priority.
  jit::JitScript* firstJitScript = first->script()->jitScript();
  jit::JitScript* secondJitScript = second->script()->jitScript();
  return firstJitScript->warmUpCount() / first->script()->length() >
         secondJitScript->warmUpCount() / second->script()->length();
}

jit::IonCompileTask* GlobalHelperThreadState::highestPriorityPendingIonCompile(
    const AutoLockHelperThreadState& lock, bool checkExecutionStatus) {
  auto& worklist = ionWorklist(lock);
  MOZ_ASSERT(!worklist.empty());

  // Get the highest priority IonCompileTask which has not started compilation
  // yet.
  size_t index = worklist.length();
  for (size_t i = 0; i < worklist.length(); i++) {
    if (checkExecutionStatus && !worklist[i]->isMainThreadRunningJS()) {
      continue;
    }
    if (i < index ||
        IonCompileTaskHasHigherPriority(worklist[i], worklist[index])) {
      index = i;
    }
  }

  if (index == worklist.length()) {
    return nullptr;
  }
  jit::IonCompileTask* task = worklist[index];
  worklist.erase(&worklist[index]);
  return task;
}

HelperThreadTask* GlobalHelperThreadState::maybeGetIonCompileTask(
    const AutoLockHelperThreadState& lock) {
  if (!canStartIonCompileTask(lock)) {
    return nullptr;
  }

  return highestPriorityPendingIonCompile(lock,
                                          /* checkExecutionStatus */ true);
}

HelperThreadTask* GlobalHelperThreadState::maybeGetLowPrioIonCompileTask(
    const AutoLockHelperThreadState& lock) {
  if (!canStartIonCompileTask(lock)) {
    return nullptr;
  }

  return highestPriorityPendingIonCompile(lock,
                                          /* checkExecutionStatus */ false);
}

bool GlobalHelperThreadState::submitTask(
    jit::IonCompileTask* task, const AutoLockHelperThreadState& locked) {
  MOZ_ASSERT(isInitialized(locked));

  if (!ionWorklist(locked).append(task)) {
    return false;
  }

  // The build is moving off-thread. Freeze the LifoAlloc to prevent any
  // unwanted mutations.
  task->alloc().lifoAlloc()->setReadOnly();

  dispatch(locked);
  return true;
}

bool js::StartOffThreadIonCompile(jit::IonCompileTask* task,
                                  const AutoLockHelperThreadState& lock) {
  return HelperThreadState().submitTask(task, lock);
}

/*
 * Move an IonCompilationTask for which compilation has either finished, failed,
 * or been cancelled into the global finished compilation list. All off thread
 * compilations which are started must eventually be finished.
 */
void js::FinishOffThreadIonCompile(jit::IonCompileTask* task,
                                   const AutoLockHelperThreadState& lock) {
  AutoEnterOOMUnsafeRegion oomUnsafe;
  if (!HelperThreadState().ionFinishedList(lock).append(task)) {
    oomUnsafe.crash("FinishOffThreadIonCompile");
  }
  task->script()
      ->runtimeFromAnyThread()
      ->jitRuntime()
      ->numFinishedOffThreadTasksRef(lock)++;
}

static JSRuntime* GetSelectorRuntime(const CompilationSelector& selector) {
  struct Matcher {
    JSRuntime* operator()(JSScript* script) {
      return script->runtimeFromMainThread();
    }
    JSRuntime* operator()(Zone* zone) { return zone->runtimeFromMainThread(); }
    JSRuntime* operator()(ZonesInState zbs) { return zbs.runtime; }
    JSRuntime* operator()(JSRuntime* runtime) { return runtime; }
  };

  return selector.match(Matcher());
}

static bool IonCompileTaskMatches(const CompilationSelector& selector,
                                  jit::IonCompileTask* task) {
  struct TaskMatches {
    jit::IonCompileTask* task_;

    bool operator()(JSScript* script) { return script == task_->script(); }
    bool operator()(Zone* zone) {
      return zone == task_->script()->zoneFromAnyThread();
    }
    bool operator()(JSRuntime* runtime) {
      return runtime == task_->script()->runtimeFromAnyThread();
    }
    bool operator()(ZonesInState zbs) {
      return zbs.runtime == task_->script()->runtimeFromAnyThread() &&
             zbs.state == task_->script()->zoneFromAnyThread()->gcState();
    }
  };

  return selector.match(TaskMatches{task});
}

// If we're canceling Ion compilations for a zone/runtime, force a new
// IonFreeTask even if there are just a few tasks. This lets us free as much
// memory as possible.
static bool ShouldForceIonFreeTask(const CompilationSelector& selector) {
  struct Matcher {
    bool operator()(JSScript* script) { return false; }
    bool operator()(Zone* zone) { return true; }
    bool operator()(ZonesInState zbs) { return true; }
    bool operator()(JSRuntime* runtime) { return true; }
  };

  return selector.match(Matcher());
}

void GlobalHelperThreadState::cancelOffThreadIonCompile(
    const CompilationSelector& selector) {
  jit::JitRuntime* jitRuntime = GetSelectorRuntime(selector)->jitRuntime();
  MOZ_ASSERT(jitRuntime);

  AutoStartIonFreeTask freeTask(jitRuntime, ShouldForceIonFreeTask(selector));

  {
    AutoLockHelperThreadState lock;
    if (!isInitialized(lock)) {
      return;
    }

    /* Cancel any pending entries for which processing hasn't started. */
    GlobalHelperThreadState::IonCompileTaskVector& worklist = ionWorklist(lock);
    for (size_t i = 0; i < worklist.length(); i++) {
      jit::IonCompileTask* task = worklist[i];
      if (IonCompileTaskMatches(selector, task)) {
        // Once finished, tasks are added to a Linked list which is
        // allocated with the IonCompileTask class. The IonCompileTask is
        // allocated in the LifoAlloc so we need the LifoAlloc to be mutable.
        worklist[i]->alloc().lifoAlloc()->setReadWrite();

        FinishOffThreadIonCompile(task, lock);
        remove(worklist, &i);
      }
    }

    /* Wait for in progress entries to finish up. */
    bool cancelled;
    do {
      cancelled = false;
      for (auto* helper : helperTasks(lock)) {
        if (!helper->is<jit::IonCompileTask>()) {
          continue;
        }

        jit::IonCompileTask* ionCompileTask = helper->as<jit::IonCompileTask>();
        if (IonCompileTaskMatches(selector, ionCompileTask)) {
          ionCompileTask->alloc().lifoAlloc()->setReadWrite();
          ionCompileTask->mirGen().cancel();
          cancelled = true;
        }
      }
      if (cancelled) {
        wait(lock);
      }
    } while (cancelled);

    /* Cancel code generation for any completed entries. */
    GlobalHelperThreadState::IonCompileTaskVector& finished =
        ionFinishedList(lock);
    for (size_t i = 0; i < finished.length(); i++) {
      jit::IonCompileTask* task = finished[i];
      if (IonCompileTaskMatches(selector, task)) {
        JSRuntime* rt = task->script()->runtimeFromAnyThread();
        jitRuntime->numFinishedOffThreadTasksRef(lock)--;
        jit::FinishOffThreadTask(rt, freeTask, task);
        remove(finished, &i);
      }
    }
  }

  /* Cancel lazy linking for pending tasks (attached to the ionScript). */
  JSRuntime* runtime = GetSelectorRuntime(selector);
  jit::IonCompileTask* task = jitRuntime->ionLazyLinkList(runtime).getFirst();
  while (task) {
    jit::IonCompileTask* next = task->getNext();
    if (IonCompileTaskMatches(selector, task)) {
      jit::FinishOffThreadTask(runtime, freeTask, task);
    }
    task = next;
  }
}

static bool JitDataStructuresExist(const CompilationSelector& selector) {
  struct Matcher {
    bool operator()(JSScript* script) { return !!script->zone()->jitZone(); }
    bool operator()(Zone* zone) { return !!zone->jitZone(); }
    bool operator()(ZonesInState zbs) { return zbs.runtime->hasJitRuntime(); }
    bool operator()(JSRuntime* runtime) { return runtime->hasJitRuntime(); }
  };

  return selector.match(Matcher());
}

void js::CancelOffThreadIonCompile(const CompilationSelector& selector) {
  if (!JitDataStructuresExist(selector)) {
    return;
  }

  if (jit::IsPortableBaselineInterpreterEnabled()) {
    return;
  }

  HelperThreadState().cancelOffThreadIonCompile(selector);
}

#ifdef DEBUG
bool GlobalHelperThreadState::hasOffThreadIonCompile(
    Zone* zone, AutoLockHelperThreadState& lock) {
  for (jit::IonCompileTask* task : ionWorklist(lock)) {
    if (task->script()->zoneFromAnyThread() == zone) {
      return true;
    }
  }

  for (auto* helper : helperTasks(lock)) {
    if (helper->is<jit::IonCompileTask>()) {
      JSScript* script = helper->as<jit::IonCompileTask>()->script();
      if (script->zoneFromAnyThread() == zone) {
        return true;
      }
    }
  }

  for (jit::IonCompileTask* task : ionFinishedList(lock)) {
    if (task->script()->zoneFromAnyThread() == zone) {
      return true;
    }
  }

  JSRuntime* rt = zone->runtimeFromMainThread();
  if (rt->hasJitRuntime()) {
    for (jit::IonCompileTask* task : rt->jitRuntime()->ionLazyLinkList(rt)) {
      if (task->script()->zone() == zone) {
        return true;
      }
    }
  }

  return false;
}

bool js::HasOffThreadIonCompile(Zone* zone) {
  if (jit::IsPortableBaselineInterpreterEnabled()) {
    return false;
  }

  AutoLockHelperThreadState lock;

  if (!HelperThreadState().isInitialized(lock)) {
    return false;
  }

  return HelperThreadState().hasOffThreadIonCompile(zone, lock);
}
#endif

//== IonFreeTask ==========================================================

bool GlobalHelperThreadState::canStartIonFreeTask(
    const AutoLockHelperThreadState& lock) {
  return !ionFreeList(lock).empty() &&
         checkTaskThreadLimit(THREAD_TYPE_ION_FREE, maxIonFreeThreads(), lock);
}

HelperThreadTask* GlobalHelperThreadState::maybeGetIonFreeTask(
    const AutoLockHelperThreadState& lock) {
  if (!canStartIonFreeTask(lock)) {
    return nullptr;
  }

  UniquePtr<jit::IonFreeTask> task = std::move(ionFreeList(lock).back());
  ionFreeList(lock).popBack();
  return task.release();
}

void jit::JitRuntime::maybeStartIonFreeTask(bool force) {
  IonFreeCompileTasks& tasks = ionFreeTaskBatch_.ref();
  if (tasks.empty()) {
    return;
  }

  // Start an IonFreeTask if we have at least eight tasks. If |force| is true we
  // always start an IonFreeTask.
  if (!force) {
    constexpr size_t MinBatchSize = 8;
    static_assert(IonFreeCompileTasks::InlineLength >= MinBatchSize,
                  "Minimum batch size shouldn't require malloc");
    if (tasks.length() < MinBatchSize) {
      return;
    }
  }

  auto freeTask = js::MakeUnique<jit::IonFreeTask>(std::move(tasks));
  if (!freeTask) {
    // Free compilation data on the main thread instead.
    MOZ_ASSERT(!tasks.empty(), "shouldn't have moved tasks on OOM");
    jit::FreeIonCompileTasks(tasks);
    tasks.clearAndFree();
    return;
  }

  AutoLockHelperThreadState lock;
  if (!HelperThreadState().submitTask(std::move(freeTask), lock)) {
    // If submitTask OOMs, then freeTask hasn't been moved so we can still use
    // its task list.
    jit::FreeIonCompileTasks(freeTask->compileTasks());
  }

  tasks.clearAndFree();
}

bool GlobalHelperThreadState::submitTask(
    UniquePtr<jit::IonFreeTask>&& task,
    const AutoLockHelperThreadState& locked) {
  MOZ_ASSERT(isInitialized(locked));

  if (!ionFreeList(locked).append(std::move(task))) {
    return false;
  }

  dispatch(locked);
  return true;
}

bool js::AutoStartIonFreeTask::addIonCompileToFreeTaskBatch(
    jit::IonCompileTask* task) {
  return jitRuntime_->addIonCompileToFreeTaskBatch(task);
}

js::AutoStartIonFreeTask::~AutoStartIonFreeTask() {
  jitRuntime_->maybeStartIonFreeTask(force_);
}

//== BaselineCompileTask ==================================================

bool GlobalHelperThreadState::canStartBaselineCompileTask(
    const AutoLockHelperThreadState& lock) {
  return !baselineWorklist(lock).empty() &&
         checkTaskThreadLimit(THREAD_TYPE_BASELINE,
                              maxBaselineCompilationThreads(), lock);
}

HelperThreadTask* GlobalHelperThreadState::maybeGetBaselineCompileTask(
    const AutoLockHelperThreadState& lock) {
  if (!canStartBaselineCompileTask(lock)) {
    return nullptr;
  }

  return baselineWorklist(lock).popCopy();
}

bool GlobalHelperThreadState::submitTask(
    jit::BaselineCompileTask* task, const AutoLockHelperThreadState& locked) {
  MOZ_ASSERT(isInitialized(locked));

  if (!baselineWorklist(locked).append(task)) {
    return false;
  }

  dispatch(locked);
  return true;
}

bool js::StartOffThreadBaselineCompile(jit::BaselineCompileTask* task,
                                       const AutoLockHelperThreadState& lock) {
  return HelperThreadState().submitTask(task, lock);
}

/*
 * Move a BaselineCompileTask for which compilation has either finished, failed,
 * or been cancelled into the global finished compilation list. All off thread
 * compilations which are started must eventually be finished.
 */
void js::FinishOffThreadBaselineCompile(jit::BaselineCompileTask* task,
                                        const AutoLockHelperThreadState& lock) {
  AutoEnterOOMUnsafeRegion oomUnsafe;
  if (!HelperThreadState().baselineFinishedList(lock).append(task)) {
    oomUnsafe.crash("FinishOffThreadBaselineCompile");
  }
  task->runtimeFromAnyThread()->jitRuntime()->numFinishedOffThreadTasksRef(
      lock)++;
}

static bool BaselineCompileTaskMatches(const CompilationSelector& selector,
                                       jit::BaselineCompileTask* task) {
  struct TaskMatches {
    jit::BaselineCompileTask* task_;

    bool operator()(JSScript* script) { return task_->scriptMatches(script); }
    bool operator()(Zone* zone) { return zone == task_->zoneFromAnyThread(); }
    bool operator()(JSRuntime* runtime) {
      return runtime == task_->runtimeFromAnyThread();
    }
    bool operator()(ZonesInState zbs) {
      return zbs.runtime == task_->runtimeFromAnyThread() &&
             zbs.state == task_->zoneFromAnyThread()->gcState();
    }
  };

  return selector.match(TaskMatches{task});
}

void GlobalHelperThreadState::cancelOffThreadBaselineCompile(
    const CompilationSelector& selector) {
  jit::JitRuntime* jitRuntime = GetSelectorRuntime(selector)->jitRuntime();
  MOZ_ASSERT(jitRuntime);

  {
    AutoLockHelperThreadState lock;
    if (!isInitialized(lock)) {
      return;
    }

    /* Cancel any pending entries for which processing hasn't started. */
    GlobalHelperThreadState::BaselineCompileTaskVector& worklist =
        baselineWorklist(lock);
    for (size_t i = 0; i < worklist.length(); i++) {
      jit::BaselineCompileTask* task = worklist[i];
      if (BaselineCompileTaskMatches(selector, task)) {
        FinishOffThreadBaselineCompile(task, lock);
        remove(worklist, &i);
      }
    }

    /* Wait for in progress entries to finish up. */
    while (true) {
      bool inProgress = false;
      for (auto* helper : helperTasks(lock)) {
        if (!helper->is<jit::BaselineCompileTask>()) {
          continue;
        }

        jit::BaselineCompileTask* task = helper->as<jit::BaselineCompileTask>();
        if (BaselineCompileTaskMatches(selector, task)) {
          inProgress = true;
          break;
        }
      }
      if (!inProgress) {
        break;
      }
      wait(lock);
    }

    /* Cancel linking for any completed entries. */
    GlobalHelperThreadState::BaselineCompileTaskVector& finished =
        baselineFinishedList(lock);
    for (size_t i = 0; i < finished.length(); i++) {
      jit::BaselineCompileTask* task = finished[i];
      if (BaselineCompileTaskMatches(selector, task)) {
        jitRuntime->numFinishedOffThreadTasksRef(lock)--;
        jit::BaselineCompileTask::FinishOffThreadTask(task);
        remove(finished, &i);
      }
    }
  }
}

void js::CancelOffThreadBaselineCompile(const CompilationSelector& selector) {
  if (!JitDataStructuresExist(selector)) {
    return;
  }

  if (jit::IsPortableBaselineInterpreterEnabled()) {
    return;
  }

  HelperThreadState().cancelOffThreadBaselineCompile(selector);
}

//== DelazifyTask =========================================================

bool GlobalHelperThreadState::canStartDelazifyTask(
    const AutoLockHelperThreadState& lock) {
  return !delazifyWorklist(lock).isEmpty() &&
         checkTaskThreadLimit(THREAD_TYPE_DELAZIFY, maxDelazifyThreads(),
                              /*isMaster=*/true, lock);
}

HelperThreadTask* GlobalHelperThreadState::maybeGetDelazifyTask(
    const AutoLockHelperThreadState& lock) {
  // NOTE: We want to span all cores availables with delazification tasks, in
  // order to parse a maximum number of functions ahead of their executions.
  // Thus, as opposed to parse task which have a higher priority, we are not
  // exclusively executing these task on parse threads.
  auto& worklist = delazifyWorklist(lock);
  if (worklist.isEmpty()) {
    return nullptr;
  }
  return worklist.popFirst();
}

void GlobalHelperThreadState::submitTask(
    DelazifyTask* task, const AutoLockHelperThreadState& locked) {
  delazifyWorklist(locked).insertBack(task);
  dispatch(locked);
}

void js::StartOffThreadDelazification(
    JSContext* maybeCx, const JS::ReadOnlyCompileOptions& options,
    frontend::InitialStencilAndDelazifications* stencils) {
  // Skip delazify tasks if we parse everything on-demand or ahead.
  auto strategy = options.eagerDelazificationStrategy();
  if (strategy == JS::DelazificationOption::OnDemandOnly ||
      strategy == JS::DelazificationOption::ParseEverythingEagerly) {
    return;
  }

  // Skip delazify task if code coverage is enabled.
  if (maybeCx && maybeCx->realm()->collectCoverageForDebug()) {
    return;
  }

  if (!CanUseExtraThreads()) {
    return;
  }

  JSRuntime* maybeRuntime = maybeCx ? maybeCx->runtime() : nullptr;
  UniquePtr<DelazifyTask> task;
  task = DelazifyTask::Create(maybeRuntime, options, stencils);
  if (!task) {
    return;
  }

  // Schedule delazification task if there is any function to delazify.
  if (!task->done()) {
    AutoLockHelperThreadState lock;
    HelperThreadState().submitTask(task.release(), lock);
  }
}

UniquePtr<DelazifyTask> DelazifyTask::Create(
    JSRuntime* maybeRuntime, const JS::ReadOnlyCompileOptions& options,
    frontend::InitialStencilAndDelazifications* stencils) {
  UniquePtr<DelazifyTask> task;
  task.reset(js_new<DelazifyTask>(maybeRuntime, options.prefableOptions()));
  if (!task) {
    return nullptr;
  }

  if (!task->init(options, stencils)) {
    // In case of errors, skip this and delazify on-demand.
    return nullptr;
  }

  return task;
}

DelazifyTask::DelazifyTask(
    JSRuntime* maybeRuntime,
    const JS::PrefableCompileOptions& initialPrefableOptions)
    : maybeRuntime(maybeRuntime),
      delazificationCx(initialPrefableOptions, HelperThreadState().stackQuota) {
}

DelazifyTask::~DelazifyTask() {
  // The LinkedListElement destructor will remove us from any list we are part
  // of without synchronization, so ensure that doesn't happen.
  MOZ_DIAGNOSTIC_ASSERT(!isInList());
}

bool DelazifyTask::init(const JS::ReadOnlyCompileOptions& options,
                        frontend::InitialStencilAndDelazifications* stencils) {
  return delazificationCx.init(options, stencils);
}

size_t DelazifyTask::sizeOfExcludingThis(
    mozilla::MallocSizeOf mallocSizeOf) const {
  return delazificationCx.sizeOfExcludingThis(mallocSizeOf);
}

void DelazifyTask::runHelperThreadTask(AutoLockHelperThreadState& lock) {
  {
    AutoUnlockHelperThreadState unlock(lock);
    // NOTE: We do not report errors beyond this scope, as there is no where
    // to report these errors to. In the mean time, prevent the eager
    // delazification from running after any kind of errors.
    (void)runTask();
  }

  // If we should continue to delazify even more functions, then re-add this
  // task to the vector of delazification tasks. This might happen when the
  // DelazifyTask is interrupted by a higher priority task. (see
  // mozilla::TaskController & mozilla::Task)
  if (!delazificationCx.done()) {
    HelperThreadState().submitTask(this, lock);
  } else {
    UniquePtr<FreeDelazifyTask> freeTask(js_new<FreeDelazifyTask>(this));
    if (freeTask) {
      HelperThreadState().submitTask(std::move(freeTask), lock);
    }
  }
}

bool DelazifyTask::runTask() { return delazificationCx.delazify(); }

bool DelazifyTask::done() const { return delazificationCx.done(); }

void GlobalHelperThreadState::cancelPendingDelazifyTask(
    JSRuntime* rt, AutoLockHelperThreadState& lock) {
  auto& delazifyList = delazifyWorklist(lock);

  auto end = delazifyList.end();
  for (auto iter = delazifyList.begin(); iter != end;) {
    DelazifyTask* task = *iter;
    ++iter;
    if (task->runtimeMatchesOrNoRuntime(rt)) {
      task->removeFrom(delazifyList);
      js_delete(task);
    }
  }
}

void GlobalHelperThreadState::waitUntilCancelledDelazifyTasks(
    JSRuntime* rt, AutoLockHelperThreadState& lock) {
  while (true) {
    cancelPendingDelazifyTask(rt, lock);

    // If running tasks are delazifying any functions, then we have to wait
    // until they complete to remove them from the pending list. DelazifyTask
    // are inserting themself back to be processed once more after delazifying a
    // function.
    bool inProgress = false;
    for (auto* helper : helperTasks(lock)) {
      if (helper->is<DelazifyTask>() &&
          helper->as<DelazifyTask>()->runtimeMatchesOrNoRuntime(rt)) {
        inProgress = true;
        break;
      }
    }
    if (!inProgress) {
      break;
    }

    wait(lock);
  }

  MOZ_ASSERT(!hasAnyDelazifyTask(rt, lock));
}

void GlobalHelperThreadState::waitUntilEmptyFreeDelazifyTaskVector(
    AutoLockHelperThreadState& lock) {
  while (true) {
    bool inProgress = false;
    if (!freeDelazifyTaskVector(lock).empty()) {
      inProgress = true;
    }

    // If running tasks are delazifying any functions, then we have to wait
    // until they complete to remove them from the pending list. DelazifyTask
    // are inserting themself back to be processed once more after delazifying a
    // function.
    for (auto* helper : helperTasks(lock)) {
      if (helper->is<FreeDelazifyTask>()) {
        inProgress = true;
        break;
      }
    }
    if (!inProgress) {
      break;
    }

    wait(lock);
  }
}

void js::CancelOffThreadDelazify(JSRuntime* runtime) {
  AutoLockHelperThreadState lock;

  if (!HelperThreadState().isInitialized(lock)) {
    return;
  }

  // Cancel all Delazify tasks from the given runtime, and wait if tasks are
  // from the given runtime are being executed.
  HelperThreadState().waitUntilCancelledDelazifyTasks(runtime, lock);

  // Empty the free list of delazify task, in case one of the delazify task
  // ended and therefore did not returned to the pending list of delazify tasks.
  HelperThreadState().waitUntilEmptyFreeDelazifyTaskVector(lock);
}

bool GlobalHelperThreadState::hasAnyDelazifyTask(
    JSRuntime* rt, AutoLockHelperThreadState& lock) {
  for (auto task : delazifyWorklist(lock)) {
    if (task->runtimeMatchesOrNoRuntime(rt)) {
      return true;
    }
  }

  for (auto* helper : helperTasks(lock)) {
    if (helper->is<DelazifyTask>() &&
        helper->as<DelazifyTask>()->runtimeMatchesOrNoRuntime(rt)) {
      return true;
    }
  }

  return false;
}

void js::WaitForAllDelazifyTasks(JSRuntime* rt) {
  AutoLockHelperThreadState lock;
  if (!HelperThreadState().isInitialized(lock)) {
    return;
  }

  while (true) {
    if (!HelperThreadState().hasAnyDelazifyTask(rt, lock)) {
      break;
    }

    HelperThreadState().wait(lock);
  }
}

//== FreeDelazifyTask =====================================================

bool GlobalHelperThreadState::canStartFreeDelazifyTask(
    const AutoLockHelperThreadState& lock) {
  return !freeDelazifyTaskVector(lock).empty() &&
         checkTaskThreadLimit(THREAD_TYPE_DELAZIFY_FREE, maxDelazifyThreads(),
                              /*isMaster=*/true, lock);
}

HelperThreadTask* GlobalHelperThreadState::maybeGetFreeDelazifyTask(
    const AutoLockHelperThreadState& lock) {
  auto& freeList = freeDelazifyTaskVector(lock);
  if (!freeList.empty()) {
    UniquePtr<FreeDelazifyTask> task = std::move(freeList.back());
    freeList.popBack();
    return task.release();
  }
  return nullptr;
}

bool GlobalHelperThreadState::submitTask(
    UniquePtr<FreeDelazifyTask> task, const AutoLockHelperThreadState& locked) {
  if (!freeDelazifyTaskVector(locked).append(std::move(task))) {
    return false;
  }
  dispatch(locked);
  return true;
}

void FreeDelazifyTask::runHelperThreadTask(AutoLockHelperThreadState& locked) {
  {
    AutoUnlockHelperThreadState unlock(locked);
    js_delete(task);
    task = nullptr;
  }

  js_delete(this);
}

//== PromiseHelperTask ====================================================

bool GlobalHelperThreadState::canStartPromiseHelperTask(
    const AutoLockHelperThreadState& lock) {
  // PromiseHelperTasks can be wasm compilation tasks that in turn block on
  // wasm compilation so set isMaster = true.
  return !promiseHelperTasks(lock).empty() &&
         checkTaskThreadLimit(THREAD_TYPE_PROMISE_TASK,
                              maxPromiseHelperThreads(),
                              /*isMaster=*/true, lock);
}

HelperThreadTask* GlobalHelperThreadState::maybeGetPromiseHelperTask(
    const AutoLockHelperThreadState& lock) {
  if (!canStartPromiseHelperTask(lock)) {
    return nullptr;
  }

  return promiseHelperTasks(lock).popCopy();
}

bool GlobalHelperThreadState::submitTask(PromiseHelperTask* task) {
  AutoLockHelperThreadState lock;

  if (!promiseHelperTasks(lock).append(task)) {
    return false;
  }

  dispatch(lock);
  return true;
}

void PromiseHelperTask::executeAndResolveAndDestroy(JSContext* cx) {
  execute();
  run(cx, JS::Dispatchable::NotShuttingDown);
}

void PromiseHelperTask::runHelperThreadTask(AutoLockHelperThreadState& lock) {
  {
    AutoUnlockHelperThreadState unlock(lock);
    execute();
  }

  // Don't release the lock between dispatching the resolve and destroy
  // operation (which may start immediately on another thread) and returning
  // from this method.

  dispatchResolveAndDestroy(lock);
}

bool js::StartOffThreadPromiseHelperTask(JSContext* cx,
                                         UniquePtr<PromiseHelperTask> task) {
  // Execute synchronously if there are no helper threads.
  if (!CanUseExtraThreads()) {
    task.release()->executeAndResolveAndDestroy(cx);
    return true;
  }

  if (!HelperThreadState().submitTask(task.get())) {
    ReportOutOfMemory(cx);
    return false;
  }

  (void)task.release();
  return true;
}

bool js::StartOffThreadPromiseHelperTask(PromiseHelperTask* task) {
  MOZ_ASSERT(CanUseExtraThreads());

  return HelperThreadState().submitTask(task);
}

//== SourceCompressionTask ================================================

bool GlobalHelperThreadState::canStartCompressionTask(
    const AutoLockHelperThreadState& lock) {
  return !compressionWorklist(lock).empty() &&
         checkTaskThreadLimit(THREAD_TYPE_COMPRESS, maxCompressionThreads(),
                              lock);
}

HelperThreadTask* GlobalHelperThreadState::maybeGetCompressionTask(
    const AutoLockHelperThreadState& lock) {
  if (!canStartCompressionTask(lock)) {
    return nullptr;
  }

  auto& worklist = compressionWorklist(lock);
  UniquePtr<SourceCompressionTask> task = std::move(worklist.back());
  worklist.popBack();
  return task.release();
}

bool GlobalHelperThreadState::submitTask(
    UniquePtr<SourceCompressionTask> task,
    const AutoLockHelperThreadState& locked) {
  if (!compressionWorklist(locked).append(std::move(task))) {
    return false;
  }

  dispatch(locked);
  return true;
}

void GlobalHelperThreadState::startHandlingCompressionTasks(
    ScheduleCompressionTask schedule, JSRuntime* maybeRuntime,
    const AutoLockHelperThreadState& lock) {
  MOZ_ASSERT((schedule == ScheduleCompressionTask::GC) ==
             (maybeRuntime != nullptr));

  auto& pending = compressionPendingList(lock);

  for (size_t i = 0; i < pending.length(); i++) {
    UniquePtr<SourceCompressionTask>& task = pending[i];
    if (schedule == ScheduleCompressionTask::API ||
        (task->runtimeMatches(maybeRuntime) && task->shouldStart())) {
      // OOMing during appending results in the task not being scheduled
      // and deleted.
      (void)submitTask(std::move(task), lock);
      remove(pending, &i);
    }
  }
}

void js::AttachFinishedCompressions(JSRuntime* runtime,
                                    AutoLockHelperThreadState& lock) {
  auto& finished = HelperThreadState().compressionFinishedList(lock);
  for (size_t i = 0; i < finished.length(); i++) {
    if (finished[i]->runtimeMatches(runtime)) {
      UniquePtr<SourceCompressionTask> compressionTask(std::move(finished[i]));
      HelperThreadState().remove(finished, &i);
      compressionTask->complete();
    }
  }
}

void js::SweepPendingCompressions(AutoLockHelperThreadState& lock) {
  auto& pending = HelperThreadState().compressionPendingList(lock);
  for (size_t i = 0; i < pending.length(); i++) {
    if (pending[i]->shouldCancel()) {
      HelperThreadState().remove(pending, &i);
    }
  }
}

void js::RunPendingSourceCompressions(JSRuntime* runtime) {
  if (!CanUseExtraThreads()) {
    return;
  }

  AutoLockHelperThreadState lock;
  HelperThreadState().runPendingSourceCompressions(runtime, lock);
}

void GlobalHelperThreadState::runPendingSourceCompressions(
    JSRuntime* runtime, AutoLockHelperThreadState& lock) {
  startHandlingCompressionTasks(
      GlobalHelperThreadState::ScheduleCompressionTask::API, nullptr, lock);
  {
    // Dispatch tasks.
    AutoUnlockHelperThreadState unlock(lock);
  }

  // Wait until all tasks have started compression.
  while (!compressionWorklist(lock).empty()) {
    wait(lock);
  }

  // Wait for all in-process compression tasks to complete.
  waitForAllTasksLocked(lock);

  AttachFinishedCompressions(runtime, lock);
}

bool js::EnqueueOffThreadCompression(JSContext* cx,
                                     UniquePtr<SourceCompressionTask> task) {
  AutoLockHelperThreadState lock;

  auto& pending = HelperThreadState().compressionPendingList(lock);
  if (!pending.append(std::move(task))) {
    ReportOutOfMemory(cx);
    return false;
  }

  return true;
}

void js::StartHandlingCompressionsOnGC(JSRuntime* runtime) {
  AutoLockHelperThreadState lock;
  HelperThreadState().startHandlingCompressionTasks(
      GlobalHelperThreadState::ScheduleCompressionTask::GC, runtime, lock);
}

template <typename T>
static void ClearCompressionTaskList(T& list, JSRuntime* runtime) {
  for (size_t i = 0; i < list.length(); i++) {
    if (list[i]->runtimeMatches(runtime)) {
      HelperThreadState().remove(list, &i);
    }
  }
}

void GlobalHelperThreadState::cancelOffThreadCompressions(
    JSRuntime* runtime, AutoLockHelperThreadState& lock) {
  // Cancel all pending compression tasks.
  ClearCompressionTaskList(compressionPendingList(lock), runtime);
  ClearCompressionTaskList(compressionWorklist(lock), runtime);

  // Cancel all in-process compression tasks and wait for them to join so we
  // clean up the finished tasks.
  while (true) {
    bool inProgress = false;
    for (auto* helper : helperTasks(lock)) {
      if (!helper->is<SourceCompressionTask>()) {
        continue;
      }

      if (helper->as<SourceCompressionTask>()->runtimeMatches(runtime)) {
        inProgress = true;
      }
    }

    if (!inProgress) {
      break;
    }

    wait(lock);
  }

  // Clean up finished tasks.
  ClearCompressionTaskList(compressionFinishedList(lock), runtime);
}

void js::CancelOffThreadCompressions(JSRuntime* runtime) {
  if (!CanUseExtraThreads()) {
    return;
  }

  AutoLockHelperThreadState lock;
  HelperThreadState().cancelOffThreadCompressions(runtime, lock);
}

//== GCParallelTask =======================================================

bool GlobalHelperThreadState::canStartGCParallelTask(
    const AutoLockHelperThreadState& lock) {
  return !gcParallelWorklist().isEmpty(lock) &&
         checkTaskThreadLimit(THREAD_TYPE_GCPARALLEL, maxGCParallelThreads(),
                              lock);
}

HelperThreadTask* GlobalHelperThreadState::maybeGetGCParallelTask(
    const AutoLockHelperThreadState& lock) {
  if (!canStartGCParallelTask(lock)) {
    return nullptr;
  }

  return gcParallelWorklist().popFirst(lock);
}

bool GlobalHelperThreadState::submitTask(
    GCParallelTask* task, const AutoLockHelperThreadState& locked) {
  gcParallelWorklist().insertBack(task, locked);
  dispatch(locked);
  return true;
}

///////////////////////////////////////////////////////////////////////////
//                                                                       //
// Wasm task definitions                                                 //
//                                                                       //
///////////////////////////////////////////////////////////////////////////

//== WasmTier1CompileTask =================================================

HelperThreadTask* GlobalHelperThreadState::maybeGetWasmTier1CompileTask(
    const AutoLockHelperThreadState& lock) {
  return maybeGetWasmCompile(lock, wasm::CompileState::EagerTier1);
}

bool GlobalHelperThreadState::canStartWasmTier1CompileTask(
    const AutoLockHelperThreadState& lock) {
  return canStartWasmCompile(lock, wasm::CompileState::EagerTier1);
}

//== WasmTier2CompileTask =================================================

HelperThreadTask* GlobalHelperThreadState::maybeGetWasmTier2CompileTask(
    const AutoLockHelperThreadState& lock) {
  return maybeGetWasmCompile(lock, wasm::CompileState::EagerTier2);
}

bool GlobalHelperThreadState::canStartWasmTier2CompileTask(
    const AutoLockHelperThreadState& lock) {
  return canStartWasmCompile(lock, wasm::CompileState::EagerTier2);
}

//== WasmCompleteTier2GeneratorTask =======================================

bool GlobalHelperThreadState::canStartWasmCompleteTier2GeneratorTask(
    const AutoLockHelperThreadState& lock) {
  return !wasmCompleteTier2GeneratorWorklist(lock).empty() &&
         checkTaskThreadLimit(THREAD_TYPE_WASM_GENERATOR_COMPLETE_TIER2,
                              maxWasmCompleteTier2GeneratorThreads(),
                              /*isMaster=*/true, lock);
}

HelperThreadTask*
GlobalHelperThreadState::maybeGetWasmCompleteTier2GeneratorTask(
    const AutoLockHelperThreadState& lock) {
  if (!canStartWasmCompleteTier2GeneratorTask(lock)) {
    return nullptr;
  }

  return wasmCompleteTier2GeneratorWorklist(lock).popCopy();
}

bool GlobalHelperThreadState::submitTask(
    wasm::UniqueCompleteTier2GeneratorTask task) {
  AutoLockHelperThreadState lock;

  MOZ_ASSERT(isInitialized(lock));

  if (!wasmCompleteTier2GeneratorWorklist(lock).append(task.get())) {
    return false;
  }
  (void)task.release();

  dispatch(lock);
  return true;
}

void js::StartOffThreadWasmCompleteTier2Generator(
    wasm::UniqueCompleteTier2GeneratorTask task) {
  (void)HelperThreadState().submitTask(std::move(task));
}

void GlobalHelperThreadState::cancelOffThreadWasmCompleteTier2Generator(
    AutoLockHelperThreadState& lock) {
  // Remove pending tasks from the tier2 generator worklist and cancel and
  // delete them.
  {
    wasm::CompleteTier2GeneratorTaskPtrVector& worklist =
        wasmCompleteTier2GeneratorWorklist(lock);
    for (size_t i = 0; i < worklist.length(); i++) {
      wasm::CompleteTier2GeneratorTask* task = worklist[i];
      remove(worklist, &i);
      js_delete(task);
    }
  }

  // There is at most one running CompleteTier2Generator task and we assume that
  // below.
  static_assert(GlobalHelperThreadState::MaxCompleteTier2GeneratorTasks == 1,
                "code must be generalized");

  // If there is a running Tier2 generator task, shut it down in a predictable
  // way.  The task will be deleted by the normal deletion logic.
  for (auto* helper : helperTasks(lock)) {
    if (helper->is<wasm::CompleteTier2GeneratorTask>()) {
      // Set a flag that causes compilation to shortcut itself.
      helper->as<wasm::CompleteTier2GeneratorTask>()->cancel();

      // Wait for the generator task to finish.  This avoids a shutdown race
      // where the shutdown code is trying to shut down helper threads and the
      // ongoing tier2 compilation is trying to finish, which requires it to
      // have access to helper threads.
      uint32_t oldFinishedCount = wasmCompleteTier2GeneratorsFinished(lock);
      while (wasmCompleteTier2GeneratorsFinished(lock) == oldFinishedCount) {
        wait(lock);
      }

      // At most one of these tasks.
      break;
    }
  }
}

static void CancelOffThreadWasmCompleteTier2GeneratorLocked(
    AutoLockHelperThreadState& lock) {
  if (!HelperThreadState().isInitialized(lock)) {
    return;
  }

  HelperThreadState().cancelOffThreadWasmCompleteTier2Generator(lock);
}

void js::CancelOffThreadWasmCompleteTier2Generator() {
  AutoLockHelperThreadState lock;
  CancelOffThreadWasmCompleteTier2GeneratorLocked(lock);
}

//== WasmPartialTier2CompileTask ==========================================

bool GlobalHelperThreadState::canStartWasmPartialTier2CompileTask(
    const AutoLockHelperThreadState& lock) {
  size_t maxThreads = maxWasmPartialTier2CompileThreads();
  // Avoid assertion failure in checkTaskThreadLimit().
  if (maxThreads > threadCount) {
    maxThreads = threadCount;
  }
  return !wasmPartialTier2CompileWorklist(lock).empty() &&
         checkTaskThreadLimit(THREAD_TYPE_WASM_COMPILE_PARTIAL_TIER2,
                              maxThreads, /*isMaster=*/false, lock);
}

HelperThreadTask* GlobalHelperThreadState::maybeGetWasmPartialTier2CompileTask(
    const AutoLockHelperThreadState& lock) {
  if (!canStartWasmPartialTier2CompileTask(lock)) {
    return nullptr;
  }

  // Take the task at the start of the vector and slide the rest down.  The
  // vector is almost always small (fewer than 50 items) and most of the time
  // has only one item, so this isn't a big expense.
  wasm::PartialTier2CompileTaskPtrVector& worklist =
      wasmPartialTier2CompileWorklist(lock);
  MOZ_ASSERT(!worklist.empty());
  HelperThreadTask* task = worklist[0];
  worklist.erase(worklist.begin());
  return task;
}

bool GlobalHelperThreadState::submitTask(
    wasm::UniquePartialTier2CompileTask task) {
  AutoLockHelperThreadState lock;

  MOZ_ASSERT(isInitialized(lock));

  wasm::PartialTier2CompileTaskPtrVector& workList =
      wasmPartialTier2CompileWorklist(lock);

  // Put the new task at the end of the vector.
  // ::maybeGetWasmPartialTier2CompileTask pulls tasks from the front of the
  // vector, hence giving FIFO behaviour.
  if (!workList.append(task.get())) {
    return false;
  }
  (void)task.release();

  dispatch(lock);
  return true;
}

void js::StartOffThreadWasmPartialTier2Compile(
    wasm::UniquePartialTier2CompileTask task) {
  (void)HelperThreadState().submitTask(std::move(task));
}

void GlobalHelperThreadState::cancelOffThreadWasmPartialTier2Compile(
    AutoLockHelperThreadState& lock) {
  // Remove pending tasks from the partial tier2 compilation worklist and
  // cancel and delete them.
  wasm::PartialTier2CompileTaskPtrVector& worklist =
      wasmPartialTier2CompileWorklist(lock);
  for (size_t i = 0; i < worklist.length(); i++) {
    wasm::PartialTier2CompileTask* task = worklist[i];
    remove(worklist, &i);
    js_delete(task);
  }

  // And remove running partial tier2 compilation tasks.  They will be deleted
  // by the normal deletion logic (in
  // PartialTier2CompileTaskImpl::runHelperThreadTask).
  bool anyCancelled;
  do {
    anyCancelled = false;
    for (auto* helper : helperTasks(lock)) {
      if (!helper->is<wasm::PartialTier2CompileTask>()) {
        continue;
      }
      wasm::PartialTier2CompileTask* pt2CompileTask =
          helper->as<wasm::PartialTier2CompileTask>();
      pt2CompileTask->cancel();
      anyCancelled = true;
    }
    if (anyCancelled) {
      wait(lock);
    }
  } while (anyCancelled);
}

static void CancelOffThreadWasmPartialTier2CompileLocked(
    AutoLockHelperThreadState& lock) {
  if (!HelperThreadState().isInitialized(lock)) {
    return;
  }

  HelperThreadState().cancelOffThreadWasmPartialTier2Compile(lock);
}

void js::CancelOffThreadWasmPartialTier2Compile() {
  AutoLockHelperThreadState lock;
  CancelOffThreadWasmPartialTier2CompileLocked(lock);
}

//== wasm task management =================================================

bool GlobalHelperThreadState::canStartWasmCompile(
    const AutoLockHelperThreadState& lock, wasm::CompileState state) {
  if (wasmWorklist(lock, state).empty()) {
    return false;
  }

  // Parallel compilation and background compilation should be disabled on
  // unicore systems.

  MOZ_RELEASE_ASSERT(cpuCount > 1);

  // If CompleteTier2 is very backlogged we must give priority to it, since the
  // CompleteTier2 queue holds onto Tier1 tasks.  Indeed if CompleteTier2 is
  // backlogged we will devote more resources to CompleteTier2 and not start
  // any Tier1 work at all.

  bool completeTier2oversubscribed =
      wasmCompleteTier2GeneratorWorklist(lock).length() > 20;

  // For Tier1 and Once compilation, honor the maximum allowed threads to
  // compile wasm jobs at once, to avoid oversaturating the machine.
  //
  // For CompleteTier2 compilation we need to allow other things to happen too,
  // so we do not allow all logical cores to be used for background work;
  // instead we wish to use a fraction of the physical cores.  We can't
  // directly compute the physical cores from the logical cores, but 1/3 of the
  // logical cores is a safe estimate for the number of physical cores
  // available for background work.

  size_t physCoresAvailable = size_t(ceil(cpuCount / 3.0));

  size_t threads;
  ThreadType threadType;
  if (state == wasm::CompileState::EagerTier2) {
    if (completeTier2oversubscribed) {
      threads = maxWasmCompilationThreads();
    } else {
      threads = physCoresAvailable;
    }
    threadType = THREAD_TYPE_WASM_COMPILE_TIER2;
  } else {
    if (completeTier2oversubscribed) {
      threads = 0;
    } else {
      threads = maxWasmCompilationThreads();
    }
    threadType = THREAD_TYPE_WASM_COMPILE_TIER1;
  }

  return threads != 0 && checkTaskThreadLimit(threadType, threads, lock);
}

HelperThreadTask* GlobalHelperThreadState::maybeGetWasmCompile(
    const AutoLockHelperThreadState& lock, wasm::CompileState state) {
  if (!canStartWasmCompile(lock, state)) {
    return nullptr;
  }

  return wasmWorklist(lock, state).popCopyFront();
}

size_t js::RemovePendingWasmCompileTasks(
    const wasm::CompileTaskState& taskState, wasm::CompileState state,
    const AutoLockHelperThreadState& lock) {
  wasm::CompileTaskPtrFifo& worklist =
      HelperThreadState().wasmWorklist(lock, state);
  return worklist.eraseIf([&taskState](wasm::CompileTask* task) {
    return &task->state == &taskState;
  });
}

bool GlobalHelperThreadState::submitTask(wasm::CompileTask* task,
                                         wasm::CompileState state) {
  AutoLockHelperThreadState lock;
  if (!wasmWorklist(lock, state).pushBack(task)) {
    return false;
  }

  dispatch(lock);
  return true;
}

bool js::StartOffThreadWasmCompile(wasm::CompileTask* task,
                                   wasm::CompileState state) {
  return HelperThreadState().submitTask(task, state);
}
