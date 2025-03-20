/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 *
 * Copyright 2020 Mozilla Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef wasm_context_h
#define wasm_context_h

#include "mozilla/DoublyLinkedList.h"

namespace js::wasm {

#ifdef ENABLE_WASM_JSPI

class SuspenderObject;
class SuspenderObjectData;

class SuspenderContext {
 private:
  HeapPtr<SuspenderObject*> activeSuspender_;
  // Using double-linked list to avoid allocation in the JIT code.
  mozilla::DoublyLinkedList<SuspenderObjectData> suspendedStacks_;

 public:
  SuspenderContext();
  ~SuspenderContext();
  SuspenderObject* activeSuspender();
  void setActiveSuspender(SuspenderObject* obj);
  void trace(JSTracer* trc);
  void traceRoots(JSTracer* trc);

  friend class SuspenderObject;
};
#endif  // ENABLE_WASM_JSPI

// wasm::Context lives in JSContext and contains the wasm-related per-context
// state.

class Context {
 public:
  Context()
      : triedToInstallSignalHandlers(false),
        haveSignalHandlers(false)
#ifdef ENABLE_WASM_JSPI
        ,
        suspendableStackLimit(JS::NativeStackLimitMin),
        suspendableStacksCount(0)
#endif
  {
  }

  // Used by wasm::EnsureThreadSignalHandlers(cx) to install thread signal
  // handlers once per JSContext/thread.
  bool triedToInstallSignalHandlers;
  bool haveSignalHandlers;

#ifdef ENABLE_WASM_JSPI
  JS::NativeStackLimit suspendableStackLimit;
  mozilla::Atomic<uint32_t> suspendableStacksCount;
  SuspenderContext promiseIntegration;
#endif
};

}  // namespace js::wasm

#endif  // wasm_context_h
