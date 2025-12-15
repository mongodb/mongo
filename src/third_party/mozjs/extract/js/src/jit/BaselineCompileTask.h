/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_BaselineCompileTask_h
#define jit_BaselineCompileTask_h

#include "mozilla/LinkedList.h"
#include "mozilla/Maybe.h"

#include "jit/BaselineCodeGen.h"
#include "jit/OffthreadSnapshot.h"
#include "vm/HelperThreadTask.h"

namespace js::jit {

class BaselineSnapshot {
  OffthreadGCPtr<JSScript*> script_;
  OffthreadGCPtr<GlobalLexicalEnvironmentObject*> globalLexical_;
  OffthreadGCPtr<JSObject*> globalThis_;
  uint32_t baseWarmUpThreshold_;
  bool isIonCompileable_;
  bool compileDebugInstrumentation_;

 public:
  BaselineSnapshot(JSScript* script,
                   GlobalLexicalEnvironmentObject* globalLexical,
                   JSObject* globalThis, uint32_t baseWarmUpThreshold,
                   bool isIonCompileable, bool compileDebugInstrumentation)
      : script_(script),
        globalLexical_(globalLexical),
        globalThis_(globalThis),
        baseWarmUpThreshold_(baseWarmUpThreshold),
        isIonCompileable_(isIonCompileable),
        compileDebugInstrumentation_(compileDebugInstrumentation) {}

  JSScript* script() const { return script_; }
  GlobalLexicalEnvironmentObject* globalLexical() const {
    return globalLexical_;
  }
  JSObject* globalThis() const { return globalThis_; }
  uint32_t baseWarmUpThreshold() const { return baseWarmUpThreshold_; }
  bool isIonCompileable() const { return isIonCompileable_; }
  bool compileDebugInstrumentation() const {
    return compileDebugInstrumentation_;
  }
  void trace(JSTracer* trc);
};

class OffThreadBaselineSnapshot
    : public BaselineSnapshot,
      public mozilla::LinkedListElement<OffThreadBaselineSnapshot> {
 public:
  OffThreadBaselineSnapshot(JSScript* script,
                            GlobalLexicalEnvironmentObject* globalLexical,
                            JSObject* globalThis, uint32_t baseWarmUpThreshold,
                            bool isIonCompileable,
                            bool compileDebugInstrumentation)
      : BaselineSnapshot(script, globalLexical, globalThis, baseWarmUpThreshold,
                         isIonCompileable, compileDebugInstrumentation) {}
  explicit OffThreadBaselineSnapshot(BaselineSnapshot other)
      : BaselineSnapshot(other) {}

  bool compileOffThread(TempAllocator& temp, CompileRealm* realm);

 private:
  mozilla::Maybe<OffThreadMacroAssembler> masm_;
  mozilla::Maybe<BaselineCompiler> compiler_;

  friend class BaselineCompileTask;
};

using BaselineSnapshotList = mozilla::LinkedList<OffThreadBaselineSnapshot>;

class BaselineCompileTask final : public HelperThreadTask {
 public:
  BaselineCompileTask(CompileRealm* realm, LifoAlloc* alloc,
                      BaselineSnapshotList&& snapshots)
      : realm_(realm), alloc_(alloc), snapshots_(std::move(snapshots)) {
#ifdef DEBUG
    for (auto* snapshot : snapshots_) {
      MOZ_ASSERT(CompileRealm::get(snapshot->script()->realm()) == realm);
    }
#endif
  }

  ThreadType threadType() override { return THREAD_TYPE_BASELINE; }
  void runTask();
  void runHelperThreadTask(AutoLockHelperThreadState& locked) override;

  void markScriptsAsCompiling();

  void finishOnMainThread(JSContext* cx);

  JSRuntime* runtimeFromAnyThread() const {
    return firstScript()->runtimeFromAnyThread();
  }
  Zone* zoneFromAnyThread() const { return firstScript()->zoneFromAnyThread(); }
  bool scriptMatches(JSScript* script) {
    for (auto* snapshot : snapshots_) {
      if (snapshot->script() == script) {
        return true;
      }
    }
    return false;
  }

  const char* getName() override { return "BaselineCompileTask"; }

  void trace(JSTracer* trc);

  static void FinishOffThreadTask(BaselineCompileTask* task);

  bool failed() const { return failed_; }

 private:
  // All scripts are in the same realm, so we can use an arbitrary script
  // to access the realm/zone/runtime.
  JSScript* firstScript() const { return snapshots_.getFirst()->script(); }

  CompileRealm* realm_;
  LifoAlloc* alloc_;
  BaselineSnapshotList snapshots_;

  bool failed_ = false;
};

}  // namespace js::jit

#endif /* jit_BaselineCompileTask_h */
