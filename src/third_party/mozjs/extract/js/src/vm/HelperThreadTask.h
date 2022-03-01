/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_HelperThreadTask_h
#define vm_HelperThreadTask_h

#include "js/Utility.h"

namespace js {

class AutoLockHelperThreadState;
struct ParseTask;
class SourceCompressionTask;

namespace jit {
class IonCompileTask;
class IonFreeTask;
}  // namespace jit
namespace wasm {
struct Tier2GeneratorTask;
}  // namespace wasm

template <typename T>
struct MapTypeToThreadType {};

template <>
struct MapTypeToThreadType<jit::IonCompileTask> {
  static const ThreadType threadType = THREAD_TYPE_ION;
};

template <>
struct MapTypeToThreadType<wasm::Tier2GeneratorTask> {
  static const ThreadType threadType = THREAD_TYPE_WASM_GENERATOR_TIER2;
};

template <>
struct MapTypeToThreadType<ParseTask> {
  static const ThreadType threadType = THREAD_TYPE_PARSE;
};

template <>
struct MapTypeToThreadType<SourceCompressionTask> {
  static const ThreadType threadType = THREAD_TYPE_COMPRESS;
};

struct HelperThreadTask {
  virtual void runHelperThreadTask(AutoLockHelperThreadState& locked) = 0;
  virtual ThreadType threadType() = 0;
  virtual ~HelperThreadTask() = default;

  template <typename T>
  bool is() {
    return MapTypeToThreadType<T>::threadType == threadType();
  }

  template <typename T>
  T* as() {
    MOZ_ASSERT(this->is<T>());
    return static_cast<T*>(this);
  }
};

}  // namespace js

#endif /* vm_HelperThreadTask_h */
