/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_IonCompileTask_h
#define jit_IonCompileTask_h

#include "mozilla/LinkedList.h"

#include "jit/CompilationDependencyTracker.h"
#include "jit/MIRGenerator.h"

#include "js/Utility.h"
#include "vm/HelperThreadTask.h"

struct JS_PUBLIC_API JSContext;

namespace js {
namespace jit {

class CodeGenerator;
class WarpSnapshot;

// IonCompileTask represents a single off-thread Ion compilation task.
class IonCompileTask final : public HelperThreadTask,
                             public mozilla::LinkedListElement<IonCompileTask> {
  MIRGenerator& mirGen_;

  // If off thread compilation is successful, the final code generator is
  // attached here. Code has been generated, but not linked (there is not yet
  // an IonScript). This is heap allocated, and must be explicitly destroyed,
  // performed by FinishOffThreadTask().
  CodeGenerator* backgroundCodegen_ = nullptr;

  WarpSnapshot* snapshot_ = nullptr;

  // Alias of the JSContext field of this task, to determine the priority of
  // compiling this script. Contexts are destroyed after the pending tasks are
  // removed from the helper threads. Thus this should be safe.
  const mozilla::Atomic<bool, mozilla::ReleaseAcquire>& isExecuting_;

 public:
  explicit IonCompileTask(JSContext* cx, MIRGenerator& mirGen,
                          WarpSnapshot* snapshot);

  JSScript* script() { return mirGen_.outerInfo().script(); }
  MIRGenerator& mirGen() { return mirGen_; }
  TempAllocator& alloc() { return mirGen_.alloc(); }
  WarpSnapshot* snapshot() { return snapshot_; }

  size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf);
  void trace(JSTracer* trc);

  CodeGenerator* backgroundCodegen() const { return backgroundCodegen_; }
  void setBackgroundCodegen(CodeGenerator* codegen) {
    backgroundCodegen_ = codegen;
  }

  // Return whether the main thread which scheduled this task is currently
  // executing JS code. This changes the way we prioritize tasks.
  bool isMainThreadRunningJS() const { return isExecuting_; }

  ThreadType threadType() override { return THREAD_TYPE_ION; }
  void runTask();
  void runHelperThreadTask(AutoLockHelperThreadState& locked) override;

  const char* getName() override { return "IonCompileTask"; }
};

class IonFreeTask : public HelperThreadTask {
  IonFreeCompileTasks tasks_;

 public:
  explicit IonFreeTask(IonFreeCompileTasks&& tasks) : tasks_(std::move(tasks)) {
    MOZ_ASSERT(!tasks_.empty());
  }

  const IonFreeCompileTasks& compileTasks() const { return tasks_; }

  ThreadType threadType() override { return THREAD_TYPE_ION_FREE; }
  void runHelperThreadTask(AutoLockHelperThreadState& locked) override;

  const char* getName() override { return "IonFreeTask"; }
};

void AttachFinishedCompilations(JSContext* cx);
void FinishOffThreadTask(JSRuntime* runtime, AutoStartIonFreeTask& freeTask,
                         IonCompileTask* task);
void FreeIonCompileTasks(const IonFreeCompileTasks& tasks);
UniquePtr<LifoAlloc> FreeIonCompileTaskAndReuseLifoAlloc(IonCompileTask* task);

}  // namespace jit
}  // namespace js

#endif /* jit_IonCompileTask_h */
