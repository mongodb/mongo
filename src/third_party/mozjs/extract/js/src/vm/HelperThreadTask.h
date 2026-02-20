/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_HelperThreadTask_h
#define vm_HelperThreadTask_h

#include "mozilla/TimeStamp.h"

#include "js/Utility.h"

namespace js {

class AutoHelperTaskQueue;
class AutoLockHelperThreadState;
struct DelazifyTask;
struct FreeDelazifyTask;
class GlobalHelperThreadState;
class SourceCompressionTask;

namespace jit {
class BaselineCompileTask;
class IonCompileTask;
class IonFreeTask;
}  // namespace jit
namespace wasm {
struct CompleteTier2GeneratorTask;
struct PartialTier2CompileTask;
}  // namespace wasm

template <typename T>
struct MapTypeToThreadType {};

template <>
struct MapTypeToThreadType<jit::BaselineCompileTask> {
  static const ThreadType threadType = THREAD_TYPE_BASELINE;
};

template <>
struct MapTypeToThreadType<jit::IonCompileTask> {
  static const ThreadType threadType = THREAD_TYPE_ION;
};

template <>
struct MapTypeToThreadType<wasm::CompleteTier2GeneratorTask> {
  static const ThreadType threadType =
      THREAD_TYPE_WASM_GENERATOR_COMPLETE_TIER2;
};

template <>
struct MapTypeToThreadType<wasm::PartialTier2CompileTask> {
  static const ThreadType threadType = THREAD_TYPE_WASM_COMPILE_PARTIAL_TIER2;
};

template <>
struct MapTypeToThreadType<DelazifyTask> {
  static const ThreadType threadType = THREAD_TYPE_DELAZIFY;
};

template <>
struct MapTypeToThreadType<FreeDelazifyTask> {
  static const ThreadType threadType = THREAD_TYPE_DELAZIFY_FREE;
};

template <>
struct MapTypeToThreadType<SourceCompressionTask> {
  static const ThreadType threadType = THREAD_TYPE_COMPRESS;
};

}  // namespace js

namespace JS {

class HelperThreadTask {
 public:
  virtual void runHelperThreadTask(js::AutoLockHelperThreadState& locked) = 0;
  virtual js::ThreadType threadType() = 0;
  virtual ~HelperThreadTask() = default;

  virtual const char* getName() = 0;

  template <typename T>
  bool is() {
    return js::MapTypeToThreadType<T>::threadType == threadType();
  }

  template <typename T>
  T* as() {
    MOZ_ASSERT(this->is<T>());
    return static_cast<T*>(this);
  }

 protected:
  // Called when this task is dispatched to the thread pool.
  virtual void onThreadPoolDispatch() {}
  friend class js::AutoHelperTaskQueue;
};

}  // namespace JS

namespace js {
using JS::HelperThreadTask;
}  // namespace js

#endif /* vm_HelperThreadTask_h */
