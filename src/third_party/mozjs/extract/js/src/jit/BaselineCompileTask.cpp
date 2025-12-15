/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/BaselineCompileTask.h"
#include "jit/JitRuntime.h"
#include "jit/JitScript.h"
#include "vm/HelperThreadState.h"

#include "vm/JSScript-inl.h"
#include "vm/Realm-inl.h"

using namespace js;
using namespace js::jit;

void BaselineCompileTask::runHelperThreadTask(
    AutoLockHelperThreadState& locked) {
  {
    AutoUnlockHelperThreadState unlock(locked);
    runTask();
  }

  FinishOffThreadBaselineCompile(this, locked);

  // Ping the main thread so that the compiled code can be incorporated at
  // the next interrupt callback.
  runtimeFromAnyThread()->mainContextFromAnyThread()->requestInterrupt(
      InterruptReason::AttachOffThreadCompilations);
}

// Debugging RAII class which marks the current thread as performing
// an offthread baseline compilation.
class MOZ_RAII AutoEnterBaselineBackend {
 public:
  AutoEnterBaselineBackend() {
#ifdef DEBUG
    JitContext* jcx = GetJitContext();
    jcx->enterBaselineBackend();
#endif
  }

#ifdef DEBUG
  ~AutoEnterBaselineBackend() {
    JitContext* jcx = GetJitContext();
    jcx->leaveBaselineBackend();
  }
#endif
};

void BaselineCompileTask::markScriptsAsCompiling() {
  for (auto* snapshot : snapshots_) {
    JSScript* script = snapshot->script();
    script->jitScript()->setIsBaselineCompiling(script);
  }
}

bool OffThreadBaselineSnapshot::compileOffThread(TempAllocator& temp,
                                                 CompileRealm* realm) {
  masm_.emplace(temp, realm);
  compiler_.emplace(temp, realm->runtime(), masm_.ref(), this);

  if (!compiler_->init()) {
    return false;
  }
  MethodStatus status = compiler_->compileOffThread();
  return status != Method_Error;
}

void BaselineCompileTask::runTask() {
  jit::JitContext jctx(realm_->runtime());
  AutoEnterBaselineBackend enter;

  TempAllocator* temp = alloc_->new_<TempAllocator>(alloc_);
  if (!temp) {
    failed_ = true;
    return;
  }

  for (auto* snapshot : snapshots_) {
    if (!snapshot->compileOffThread(*temp, realm_)) {
      failed_ = true;
      return;
    }
  }
}

/* static */
void BaselineCompileTask::FinishOffThreadTask(BaselineCompileTask* task) {
  for (auto* snapshot : task->snapshots_) {
    JSScript* script = snapshot->script();
    if (script->isBaselineCompilingOffThread()) {
      script->jitScript()->clearIsBaselineCompiling(script);
    }
    snapshot->compiler_.reset();
    snapshot->masm_.reset();
  }

  // The task is allocated into its LifoAlloc, so destroying that will
  // destroy the task and all other data accumulated during compilation.
  js_delete(task->alloc_);
}

void BaselineCompileTask::finishOnMainThread(JSContext* cx) {
  AutoRealm ar(cx, firstScript());
  for (auto* snapshot : snapshots_) {
    if (!snapshot->compiler_->finishCompile(cx)) {
      cx->recoverFromOutOfMemory();
    }
  }
}

void js::AttachFinishedBaselineCompilations(JSContext* cx,
                                            AutoLockHelperThreadState& lock) {
  JSRuntime* rt = cx->runtime();

  while (true) {
    GlobalHelperThreadState::BaselineCompileTaskVector& finished =
        HelperThreadState().baselineFinishedList(lock);

    // Find a finished task for this runtime.
    bool found = false;
    for (size_t i = 0; i < finished.length(); i++) {
      BaselineCompileTask* task = finished[i];
      if (task->runtimeFromAnyThread() != rt) {
        continue;
      }
      found = true;

      HelperThreadState().remove(finished, &i);
      rt->jitRuntime()->numFinishedOffThreadTasksRef(lock)--;
      {
        if (!task->failed()) {
          AutoUnlockHelperThreadState unlock(lock);
          task->finishOnMainThread(cx);
        }
        BaselineCompileTask::FinishOffThreadTask(task);
      }
    }
    if (!found) {
      break;
    }
  }
}

void BaselineSnapshot::trace(JSTracer* trc) {
  TraceOffthreadGCPtr(trc, script_, "baseline-snapshot-script");
  TraceOffthreadGCPtr(trc, globalLexical_, "baseline-snapshot-lexical");
  TraceOffthreadGCPtr(trc, globalThis_, "baseline-snapshot-this");
}

void BaselineCompileTask::trace(JSTracer* trc) {
  if (!realm_->runtime()->runtimeMatches(trc->runtime())) {
    return;
  }
  for (auto* snapshot : snapshots_) {
    snapshot->trace(trc);
  }
}
