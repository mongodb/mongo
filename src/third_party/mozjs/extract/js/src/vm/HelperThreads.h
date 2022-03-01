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
#include "NamespaceImports.h"

#include "js/OffThreadScriptCompilation.h"
#include "js/Transcoding.h"
#include "js/UniquePtr.h"
#include "threading/LockGuard.h"
#include "threading/Mutex.h"
#include "wasm/WasmConstants.h"

namespace JS {
class OffThreadToken {};
class ReadOnlyCompileOptions;
class Zone;
}  // namespace JS

namespace js {

class AutoLockHelperThreadState;
struct PromiseHelperTask;
class SourceCompressionTask;

namespace gc {
class GCRuntime;
}

namespace jit {
class IonCompileTask;
class IonFreeTask;
}  // namespace jit

namespace wasm {
struct CompileTask;
struct CompileTaskState;
struct Tier2GeneratorTask;
using UniqueTier2GeneratorTask = UniquePtr<Tier2GeneratorTask>;
}  // namespace wasm

/*
 * Lock protecting all mutable shared state accessed by helper threads, and used
 * by all condition variables.
 */
extern Mutex gHelperThreadLock;

class MOZ_RAII AutoLockHelperThreadState : public LockGuard<Mutex> {
  using Base = LockGuard<Mutex>;

 public:
  explicit AutoLockHelperThreadState() : Base(gHelperThreadLock) {}
};

class MOZ_RAII AutoUnlockHelperThreadState : public UnlockGuard<Mutex> {
  using Base = UnlockGuard<Mutex>;

 public:
  explicit AutoUnlockHelperThreadState(AutoLockHelperThreadState& locked)
      : Base(locked) {}
};

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
bool StartOffThreadWasmCompile(wasm::CompileTask* task, wasm::CompileMode mode);

// Remove any pending wasm compilation tasks queued with
// StartOffThreadWasmCompile that match the arguments. Return the number
// removed.
size_t RemovePendingWasmCompileTasks(const wasm::CompileTaskState& taskState,
                                     wasm::CompileMode mode,
                                     const AutoLockHelperThreadState& lock);

// Enqueues a wasm compilation task.
void StartOffThreadWasmTier2Generator(wasm::UniqueTier2GeneratorTask task);

// Cancel all background Wasm Tier-2 compilations.
void CancelOffThreadWasmTier2Generator();

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
 * Schedule an off-thread Ion compilation for a script, given a task.
 */
bool StartOffThreadIonCompile(jit::IonCompileTask* task,
                              const AutoLockHelperThreadState& lock);

/*
 * Schedule deletion of Ion compilation data.
 */
bool StartOffThreadIonFree(jit::IonCompileTask* task,
                           const AutoLockHelperThreadState& lock);

void FinishOffThreadIonCompile(jit::IonCompileTask* task,
                               const AutoLockHelperThreadState& lock);

struct ZonesInState {
  JSRuntime* runtime;
  JS::shadow::Zone::GCState state;
};

using CompilationSelector = mozilla::Variant<JSScript*, JS::Realm*, JS::Zone*,
                                             ZonesInState, JSRuntime*>;

/*
 * Cancel scheduled or in progress Ion compilations.
 */
void CancelOffThreadIonCompile(const CompilationSelector& selector);

inline void CancelOffThreadIonCompile(JSScript* script) {
  CancelOffThreadIonCompile(CompilationSelector(script));
}

inline void CancelOffThreadIonCompile(JS::Realm* realm) {
  CancelOffThreadIonCompile(CompilationSelector(realm));
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
bool HasOffThreadIonCompile(JS::Realm* realm);
#endif

/*
 * Wait for all scheduled, in progress or finished parse tasks for the runtime
 * to complete.
 */
void WaitForOffThreadParses(JSRuntime* runtime);

/*
 * Cancel all scheduled, in progress or finished parses for runtime.
 *
 * Parse tasks which have completed but for which JS::FinishOffThreadScript (or
 * equivalent) has not been called are removed from the system. This is only
 * safe to do during shutdown, or if you know that the main thread isn't waiting
 * for tasks to complete.
 */
void CancelOffThreadParses(JSRuntime* runtime);

/*
 * Start a parse/emit cycle for a stream of source. The characters must stay
 * alive until the compilation finishes.
 */
JS::OffThreadToken* StartOffThreadParseScript(
    JSContext* cx, const JS::ReadOnlyCompileOptions& options,
    JS::SourceText<char16_t>& srcBuf, JS::OffThreadCompileCallback callback,
    void* callbackData);
JS::OffThreadToken* StartOffThreadParseScript(
    JSContext* cx, const JS::ReadOnlyCompileOptions& options,
    JS::SourceText<mozilla::Utf8Unit>& srcBuf,
    JS::OffThreadCompileCallback callback, void* callbackData);

JS::OffThreadToken* StartOffThreadCompileToStencil(
    JSContext* cx, const JS::ReadOnlyCompileOptions& options,
    JS::SourceText<char16_t>& srcBuf, JS::OffThreadCompileCallback callback,
    void* callbackData);
JS::OffThreadToken* StartOffThreadCompileToStencil(
    JSContext* cx, const JS::ReadOnlyCompileOptions& options,
    JS::SourceText<mozilla::Utf8Unit>& srcBuf,
    JS::OffThreadCompileCallback callback, void* callbackData);

JS::OffThreadToken* StartOffThreadParseModule(
    JSContext* cx, const JS::ReadOnlyCompileOptions& options,
    JS::SourceText<char16_t>& srcBuf, JS::OffThreadCompileCallback callback,
    void* callbackData);
JS::OffThreadToken* StartOffThreadParseModule(
    JSContext* cx, const JS::ReadOnlyCompileOptions& options,
    JS::SourceText<mozilla::Utf8Unit>& srcBuf,
    JS::OffThreadCompileCallback callback, void* callbackData);

JS::OffThreadToken* StartOffThreadDecodeScript(
    JSContext* cx, const JS::ReadOnlyCompileOptions& options,
    const JS::TranscodeRange& range, JS::OffThreadCompileCallback callback,
    void* callbackData);

JS::OffThreadToken* StartOffThreadDecodeMultiScripts(
    JSContext* cx, const JS::ReadOnlyCompileOptions& options,
    JS::TranscodeSources& sources, JS::OffThreadCompileCallback callback,
    void* callbackData);

/*
 * Called at the end of GC to enqueue any Parse tasks that were waiting on an
 * atoms-zone GC to finish.
 */
void EnqueuePendingParseTasksAfterGC(JSRuntime* rt);

// Drain the task queues and wait for all helper threads to finish running.
//
// Note that helper threads are shared between runtimes and it's possible that
// another runtime could saturate the helper thread system and cause this to
// never return.
void WaitForAllHelperThreads();
void WaitForAllHelperThreads(AutoLockHelperThreadState& lock);

struct AutoEnqueuePendingParseTasksAfterGC {
  const gc::GCRuntime& gc_;
  explicit AutoEnqueuePendingParseTasksAfterGC(const gc::GCRuntime& gc)
      : gc_(gc) {}
  ~AutoEnqueuePendingParseTasksAfterGC();
};

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

// Return whether, if a new parse task was started, it would need to wait for
// an in-progress GC to complete before starting.
extern bool OffThreadParsingMustWaitForGC(JSRuntime* rt);

}  // namespace js

#endif /* vm_HelperThreads_h */
