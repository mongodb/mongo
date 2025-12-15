/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * API for managing off-thread work.
 */

#ifndef vm_HelperThreads_h
#define vm_HelperThreads_h

#include "mozilla/Variant.h"

#include "js/AllocPolicy.h"
#include "js/HelperThreadAPI.h"
#include "js/shadow/Zone.h"
#include "js/UniquePtr.h"
#include "js/Vector.h"
#include "threading/LockGuard.h"
#include "threading/Mutex.h"
#include "wasm/WasmConstants.h"

namespace mozilla {
union Utf8Unit;
}

namespace JS {
class JS_PUBLIC_API ReadOnlyCompileOptions;
class JS_PUBLIC_API ReadOnlyDecodeOptions;
class Zone;

template <typename UnitT>
class SourceText;
}  // namespace JS

namespace js {

class AutoLockHelperThreadState;
struct PromiseHelperTask;
class SourceCompressionTask;

namespace frontend {
struct InitialStencilAndDelazifications;
}

namespace gc {
class GCRuntime;
}

namespace jit {
class BaselineCompileTask;
class IonCompileTask;
class IonFreeTask;
class JitRuntime;
using IonFreeCompileTasks = Vector<IonCompileTask*, 8, SystemAllocPolicy>;
}  // namespace jit

namespace wasm {
struct CompileTask;
struct CompileTaskState;
struct CompleteTier2GeneratorTask;
using UniqueCompleteTier2GeneratorTask = UniquePtr<CompleteTier2GeneratorTask>;
struct PartialTier2CompileTask;
using UniquePartialTier2CompileTask = UniquePtr<PartialTier2CompileTask>;
}  // namespace wasm

/*
 * Lock protecting all mutable shared state accessed by helper threads, and used
 * by all condition variables.
 */
extern Mutex gHelperThreadLock MOZ_UNANNOTATED;

// Set of tasks to dispatch when the helper thread state lock is released.
class AutoHelperTaskQueue {
 public:
  ~AutoHelperTaskQueue() { dispatchQueuedTasks(); }
  bool hasQueuedTasks() const { return !tasksToDispatch.empty(); }
  void queueTaskToDispatch(JS::HelperThreadTask* task) const;
  void dispatchQueuedTasks();

 private:
  // TODO: Convert this to use a linked list.
  mutable Vector<JS::HelperThreadTask*, 1, SystemAllocPolicy> tasksToDispatch;
};

// A lock guard for data protected by the helper thread lock.
//
// This can also queue helper thread tasks to be triggered when the lock is
// released.
class MOZ_RAII AutoLockHelperThreadState
    : public AutoHelperTaskQueue,  // Must come before LockGuard.
      public LockGuard<Mutex> {
 public:
  AutoLockHelperThreadState() : LockGuard<Mutex>(gHelperThreadLock) {}
  AutoLockHelperThreadState(const AutoLockHelperThreadState&) = delete;

 private:
  friend class UnlockGuard<AutoLockHelperThreadState>;
  void unlock() {
    LockGuard<Mutex>::unlock();
    dispatchQueuedTasks();
  }

  friend class GlobalHelperThreadState;
};

using AutoUnlockHelperThreadState = UnlockGuard<AutoLockHelperThreadState>;

// Create data structures used by helper threads.
bool CreateHelperThreadsState();

// Destroy data structures used by helper threads.
void DestroyHelperThreadsState();

// Initialize helper threads unless already initialized.
bool EnsureHelperThreadsInitialized();

size_t GetHelperThreadCount();
size_t GetHelperThreadCPUCount();
size_t GetMaxWasmCompilationThreads();

// This allows the JS shell to override GetCPUCount() when passed the
// --thread-count=N option.
bool SetFakeCPUCount(size_t count);

// Enqueues a wasm compilation task.
bool StartOffThreadWasmCompile(wasm::CompileTask* task,
                               wasm::CompileState state);

// Remove any pending wasm compilation tasks queued with
// StartOffThreadWasmCompile that match the arguments. Return the number
// removed.
size_t RemovePendingWasmCompileTasks(const wasm::CompileTaskState& taskState,
                                     wasm::CompileState state,
                                     const AutoLockHelperThreadState& lock);

// Enqueues a wasm Complete Tier-2 compilation task.  This (logically, at
// least) manages a set of sub-tasks that perform compilation of groups of
// functions.
void StartOffThreadWasmCompleteTier2Generator(
    wasm::UniqueCompleteTier2GeneratorTask task);

// Enqueues a wasm Partial Tier-2 compilation task.  This compiles one
// function, doing so itself, without any sub-tasks.
void StartOffThreadWasmPartialTier2Compile(
    wasm::UniquePartialTier2CompileTask task);

// Cancel all background Wasm Complete Tier-2 compilations, both the generator
// task and the individual compilation tasks.
void CancelOffThreadWasmCompleteTier2Generator();

// Cancel a single background Wasm Partial Tier-2 compilation.
void CancelOffThreadWasmPartialTier2Compile();

/*
 * If helper threads are available, call execute() then dispatchResolve() on the
 * given task in a helper thread. If no helper threads are available, the given
 * task is executed and resolved synchronously.
 *
 * This function takes ownership of task unconditionally; if it fails, task is
 * deleted.
 */
bool StartOffThreadPromiseHelperTask(JSContext* cx,
                                     UniquePtr<PromiseHelperTask> task);

/*
 * Like the JSContext-accepting version, but only safe to use when helper
 * threads are available, so we can be sure we'll never need to fall back on
 * synchronous execution.
 *
 * This function can be called from any thread, but takes ownership of the task
 * only on success. On OOM, it is the caller's responsibility to arrange for the
 * task to be cleaned up properly.
 */
bool StartOffThreadPromiseHelperTask(PromiseHelperTask* task);

/*
 * Schedule an off-thread Baseline compilation for a script, given a task.
 */
bool StartOffThreadBaselineCompile(jit::BaselineCompileTask* task,
                                   const AutoLockHelperThreadState& lock);

void FinishOffThreadBaselineCompile(jit::BaselineCompileTask* task,
                                    const AutoLockHelperThreadState& lock);

/*
 * Schedule an off-thread Ion compilation for a script, given a task.
 */
bool StartOffThreadIonCompile(jit::IonCompileTask* task,
                              const AutoLockHelperThreadState& lock);

void FinishOffThreadIonCompile(jit::IonCompileTask* task,
                               const AutoLockHelperThreadState& lock);

// RAII class to handle batching compile tasks and starting an IonFreeTask.
class MOZ_RAII AutoStartIonFreeTask {
  jit::JitRuntime* jitRuntime_;

  // If true, start an IonFreeTask even if the batch is small.
  bool force_;

 public:
  explicit AutoStartIonFreeTask(jit::JitRuntime* jitRuntime, bool force = false)
      : jitRuntime_(jitRuntime), force_(force) {}
  ~AutoStartIonFreeTask();

  [[nodiscard]] bool addIonCompileToFreeTaskBatch(jit::IonCompileTask* task);
};

struct ZonesInState {
  JSRuntime* runtime;
  JS::shadow::Zone::GCState state;
};

using CompilationSelector =
    mozilla::Variant<JSScript*, JS::Zone*, ZonesInState, JSRuntime*>;

/*
 * Cancel scheduled or in progress Ion compilations.
 */
void CancelOffThreadIonCompile(const CompilationSelector& selector);

inline void CancelOffThreadIonCompile(JSScript* script) {
  CancelOffThreadIonCompile(CompilationSelector(script));
}

inline void CancelOffThreadIonCompile(JS::Zone* zone) {
  CancelOffThreadIonCompile(CompilationSelector(zone));
}

inline void CancelOffThreadIonCompile(JSRuntime* runtime,
                                      JS::shadow::Zone::GCState state) {
  CancelOffThreadIonCompile(CompilationSelector(ZonesInState{runtime, state}));
}

inline void CancelOffThreadIonCompile(JSRuntime* runtime) {
  CancelOffThreadIonCompile(CompilationSelector(runtime));
}

#ifdef DEBUG
bool HasOffThreadIonCompile(JS::Zone* zone);
#endif

/*
 * Cancel scheduled or in progress Baseline compilations.
 */
void CancelOffThreadBaselineCompile(const CompilationSelector& selector);

inline void CancelOffThreadBaselineCompile(JSScript* script) {
  CancelOffThreadBaselineCompile(CompilationSelector(script));
}

inline void CancelOffThreadBaselineCompile(JS::Zone* zone) {
  CancelOffThreadBaselineCompile(CompilationSelector(zone));
}

inline void CancelOffThreadBaselineCompile(JSRuntime* runtime,
                                           JS::shadow::Zone::GCState state) {
  CancelOffThreadBaselineCompile(
      CompilationSelector(ZonesInState{runtime, state}));
}

inline void CancelOffThreadBaselineCompile(JSRuntime* runtime) {
  CancelOffThreadBaselineCompile(CompilationSelector(runtime));
}

/*
 * Cancel baseline and Ion compilations.
 */
inline void CancelOffThreadCompile(JSRuntime* runtime,
                                   JS::shadow::Zone::GCState state) {
  CancelOffThreadBaselineCompile(
      CompilationSelector(ZonesInState{runtime, state}));
  CancelOffThreadIonCompile(CompilationSelector(ZonesInState{runtime, state}));
}

inline void CancelOffThreadCompile(JSRuntime* runtime) {
  CancelOffThreadBaselineCompile(runtime);
  CancelOffThreadIonCompile(runtime);
}

/*
 * Cancel all scheduled or in progress eager delazification phases for a
 * runtime.
 */
void CancelOffThreadDelazify(JSRuntime* runtime);

/*
 * Wait for all delazification to complete.
 */
void WaitForAllDelazifyTasks(JSRuntime* rt);

// Start off-thread delazification task, to race the delazification of inner
// functions.
void StartOffThreadDelazification(
    JSContext* maybeCx, const JS::ReadOnlyCompileOptions& options,
    frontend::InitialStencilAndDelazifications* stencils);

// Drain the task queues and wait for all helper threads to finish running.
//
// Note that helper threads are shared between runtimes and it's possible that
// another runtime could saturate the helper thread system and cause this to
// never return.
void WaitForAllHelperThreads();
void WaitForAllHelperThreads(AutoLockHelperThreadState& lock);

// Enqueue a compression job to be processed later. These are started at the
// start of the major GC after the next one.
bool EnqueueOffThreadCompression(JSContext* cx,
                                 UniquePtr<SourceCompressionTask> task);

// Start handling any compression tasks for this runtime. Called at the start of
// major GC.
void StartHandlingCompressionsOnGC(JSRuntime* rt);

// Cancel all scheduled, in progress, or finished compression tasks for
// runtime.
void CancelOffThreadCompressions(JSRuntime* runtime);

void AttachFinishedCompressions(JSRuntime* runtime,
                                AutoLockHelperThreadState& lock);

// Sweep pending tasks that are holding onto should-be-dead ScriptSources.
void SweepPendingCompressions(AutoLockHelperThreadState& lock);

// Run all pending source compression tasks synchronously, for testing purposes
void RunPendingSourceCompressions(JSRuntime* runtime);

// False if the off-thread source compression mechanism isn't being used. This
// happens on low core count machines where we are concerned about blocking
// main-thread execution.
bool IsOffThreadSourceCompressionEnabled();

void AttachFinishedBaselineCompilations(JSContext* cx,
                                        AutoLockHelperThreadState& lock);

}  // namespace js

#endif /* vm_HelperThreads_h */
