/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/IonCompileTask.h"

#include "jit/CodeGenerator.h"
#include "jit/Ion.h"
#include "jit/JitRuntime.h"
#include "jit/JitScript.h"
#include "jit/WarpSnapshot.h"
#include "vm/HelperThreadState.h"
#include "vm/JSScript.h"

#include "vm/JSScript-inl.h"
#include "vm/Realm-inl.h"

using namespace js;
using namespace js::jit;

void IonCompileTask::runHelperThreadTask(AutoLockHelperThreadState& locked) {
  // The build is taken by this thread. Unfreeze the LifoAlloc to allow
  // mutations.
  alloc().lifoAlloc()->setReadWrite();

  {
    AutoUnlockHelperThreadState unlock(locked);
    runTask();
  }

  FinishOffThreadIonCompile(this, locked);

  JSRuntime* rt = script()->runtimeFromAnyThread();

  // Ping the main thread so that the compiled code can be incorporated at the
  // next interrupt callback.
  //
  // This must happen before the current task is reset. DestroyContext
  // cancels in progress Ion compilations before destroying its target
  // context, and after we reset the current task we are no longer considered
  // to be Ion compiling.
  rt->mainContextFromAnyThread()->requestInterrupt(
      InterruptReason::AttachOffThreadCompilations);
}

void IonCompileTask::runTask() {
  // This is the entry point when ion compiles are run offthread.

  jit::JitContext jctx(mirGen_.realm->runtime());
  setBackgroundCodegen(jit::CompileBackEnd(&mirGen_, snapshot_));
}

void IonCompileTask::trace(JSTracer* trc) {
  if (!mirGen_.runtime->runtimeMatches(trc->runtime())) {
    return;
  }

  snapshot_->trace(trc);
}

IonCompileTask::IonCompileTask(JSContext* cx, MIRGenerator& mirGen,
                               WarpSnapshot* snapshot)
    : mirGen_(mirGen),
      snapshot_(snapshot),
      isExecuting_(cx->isExecutingRef()) {}

size_t IonCompileTask::sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) {
  // See js::jit::FreeIonCompileTask.
  // The IonCompileTask and most of its contents live in the LifoAlloc we point
  // to.

  size_t result = alloc().lifoAlloc()->sizeOfIncludingThis(mallocSizeOf);

  if (backgroundCodegen_) {
    result += mallocSizeOf(backgroundCodegen_);
  }

  return result;
}

static inline bool TooManyUnlinkedTasks(JSRuntime* rt) {
  static const size_t MaxUnlinkedTasks = 100;
  return rt->jitRuntime()->ionLazyLinkListSize() > MaxUnlinkedTasks;
}

static void MoveFinishedTasksToLazyLinkList(
    JSRuntime* rt, const AutoLockHelperThreadState& lock) {
  // Incorporate any off thread compilations for the runtime which have
  // finished, failed or have been cancelled.

  GlobalHelperThreadState::IonCompileTaskVector& finished =
      HelperThreadState().ionFinishedList(lock);

  for (size_t i = 0; i < finished.length(); i++) {
    // Find a finished task for the runtime.
    IonCompileTask* task = finished[i];
    if (task->script()->runtimeFromAnyThread() != rt) {
      continue;
    }

    HelperThreadState().remove(finished, &i);
    rt->jitRuntime()->numFinishedOffThreadTasksRef(lock)--;

    JSScript* script = task->script();
    MOZ_ASSERT(script->hasBaselineScript());
    script->baselineScript()->setPendingIonCompileTask(rt, script, task);
    rt->jitRuntime()->ionLazyLinkListAdd(rt, task);
  }
}

static void EagerlyLinkExcessTasks(JSContext* cx,
                                   AutoLockHelperThreadState& lock) {
  JSRuntime* rt = cx->runtime();
  MOZ_ASSERT(TooManyUnlinkedTasks(rt));

  do {
    jit::IonCompileTask* task = rt->jitRuntime()->ionLazyLinkList(rt).getLast();
    RootedScript script(cx, task->script());

    AutoUnlockHelperThreadState unlock(lock);
    AutoRealm ar(cx, script);
    jit::LinkIonScript(cx, script);
  } while (TooManyUnlinkedTasks(rt));
}

void jit::AttachFinishedCompilations(JSContext* cx) {
  JSRuntime* rt = cx->runtime();
  MOZ_ASSERT(CurrentThreadCanAccessRuntime(rt));

  if (!rt->jitRuntime() || !rt->jitRuntime()->numFinishedOffThreadTasks()) {
    return;
  }

  AutoLockHelperThreadState lock;

  while (true) {
    AttachFinishedBaselineCompilations(cx, lock);

    MoveFinishedTasksToLazyLinkList(rt, lock);

    if (!TooManyUnlinkedTasks(rt)) {
      break;
    }

    EagerlyLinkExcessTasks(cx, lock);

    // Linking releases the lock so we must now check the finished list
    // again.
  }

  MOZ_ASSERT(!rt->jitRuntime()->numFinishedOffThreadTasks());
}

static UniquePtr<LifoAlloc> FreeIonCompileTask(IonCompileTask* task) {
  // To correctly free compilation dependencies, which may have virtual
  // destructors we need to explicitly empty the MIRGenerator's list here.
  task->mirGen().tracker.reset();

  // The task is allocated into its LifoAlloc, so destroying that will
  // destroy the task and all other data accumulated during compilation,
  // except any final codegen (which includes an assembler and needs to be
  // explicitly destroyed).
  js_delete(task->backgroundCodegen());

  // Return the LifoAlloc as UniquePtr. Callers can either reuse the LifoAlloc
  // or ignore the return value.
  return UniquePtr<LifoAlloc>(task->alloc().lifoAlloc());
}

void jit::FreeIonCompileTasks(const IonFreeCompileTasks& tasks) {
  MOZ_ASSERT(!tasks.empty());
  for (auto* task : tasks) {
    FreeIonCompileTask(task);
  }
}

UniquePtr<LifoAlloc> jit::FreeIonCompileTaskAndReuseLifoAlloc(
    IonCompileTask* task) {
  UniquePtr<LifoAlloc> lifoAlloc = FreeIonCompileTask(task);

  // We have to call the TempAllocator's destructor first, because releaseAll
  // can only be called if the LifoAlloc's mark-count is 0. Note that the
  // TempAllocator is allocated in the LifoAlloc too.
  //
  // The LifoAllocScope's destructor calls freeAllIfHugeAndUnused and this will
  // free all LifoAlloc memory immediately if the LifoAlloc is huge. That's not
  // what we want here, so we rely on the caller ensuring !isHuge().
  MOZ_ASSERT(!lifoAlloc->isHuge());
  TempAllocator* tempAlloc = &task->alloc();
  tempAlloc->~TempAllocator();
  lifoAlloc->releaseAll();
  return lifoAlloc;
}

void IonFreeTask::runHelperThreadTask(AutoLockHelperThreadState& locked) {
  {
    AutoUnlockHelperThreadState unlock(locked);
    jit::FreeIonCompileTasks(compileTasks());
  }

  js_delete(this);
}

void jit::FinishOffThreadTask(JSRuntime* runtime,
                              AutoStartIonFreeTask& freeTask,
                              IonCompileTask* task) {
  MOZ_ASSERT(runtime);
  MOZ_ASSERT(CurrentThreadCanAccessRuntime(runtime));

  JSScript* script = task->script();

  // Clean the references to the pending IonCompileTask, if we just finished it.
  if (script->baselineScript()->hasPendingIonCompileTask() &&
      script->baselineScript()->pendingIonCompileTask() == task) {
    script->baselineScript()->removePendingIonCompileTask(runtime, script);
  }

  // If the task is still in one of the helper thread lists, then remove it.
  if (task->isInList()) {
    runtime->jitRuntime()->ionLazyLinkListRemove(runtime, task);
  }

  // Clean up if compilation did not succeed.
  if (script->isIonCompilingOffThread()) {
    script->jitScript()->clearIsIonCompilingOffThread(script);

    const AbortReasonOr<Ok>& status = task->mirGen().getOffThreadStatus();
    if (status.isErr() && status.inspectErr() == AbortReason::Disable) {
      script->disableIon();
    }
  }

  // Try to free the Ion LifoAlloc off-thread. Free on the main thread if this
  // OOMs.
  if (!freeTask.addIonCompileToFreeTaskBatch(task)) {
    FreeIonCompileTask(task);
  }
}
