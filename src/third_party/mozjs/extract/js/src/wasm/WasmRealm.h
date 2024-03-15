/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 *
 * Copyright 2016 Mozilla Foundation
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

#ifndef wasm_realm_h
#define wasm_realm_h

#include "wasm/WasmTypeDecls.h"

namespace js {
namespace wasm {

// wasm::Realm lives in JS::Realm and contains the wasm-related per-realm state.
// wasm::Realm tracks every live instance in the realm and must be notified, via
// registerInstance(), of any new WasmInstanceObject.

class Realm {
  JSRuntime* runtime_;
  InstanceVector instances_;

 public:
  explicit Realm(JSRuntime* rt);
  ~Realm();

  // Before a WasmInstanceObject can be considered fully constructed and
  // valid, it must be registered with the Realm. If this method fails,
  // an error has been reported and the instance object must be abandoned.
  // After a successful registration, an Instance must call
  // unregisterInstance() before being destroyed.

  bool registerInstance(JSContext* cx, Handle<WasmInstanceObject*> instanceObj);
  void unregisterInstance(Instance& instance);

  // Return a vector of all live instances in the realm. The lifetime of
  // these Instances is determined by their owning WasmInstanceObject.
  // Note that accessing instances()[i]->object() triggers a read barrier
  // since instances() is effectively a weak list.

  const InstanceVector& instances() const { return instances_; }

  // Ensure all Instances in this Realm have profiling labels created.

  void ensureProfilingLabels(bool profilingEnabled);

  // about:memory reporting

  void addSizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf,
                              size_t* realmTables);
};

// Interrupt all running wasm Instances that have been registered with
// wasm::Realms in the given JSContext.

extern void InterruptRunningCode(JSContext* cx);

// After a wasm Instance sees an interrupt request and calls
// CheckForInterrupt(), it should call RunningCodeInterrupted() to clear the
// interrupt request for all wasm Instances to avoid spurious trapping.

void ResetInterruptState(JSContext* cx);

}  // namespace wasm
}  // namespace js

#endif  // wasm_realm_h
